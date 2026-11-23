#!/bin/bash
# run.sh BUILD_DIR [OUT_DIR]: the render-only test in podman containers,
# each with its own network namespace.  Exit 0 only if every message
# produced no DNS query and no connection attempt in a render-only
# showing (and the control message rendered); OUT_DIR/summary.txt.
#
#   BUILD_DIR  kde3-wpepart build directory configured with
#              -DKWPE_RENDER_ONLY_TEST=ON (tests/render-only/kwpe-render-only,
#              tests/render-only/libkwpepart_test.so), built against KDE3_PREFIX
#   IMAGE      container image with WPE WebKit, gumbo, Xvfb, xdotool,
#              nftables, iproute, tcpdump, python3 (see README; default
#              localhost/kwpe-render-only)
#   KDE3_PREFIX, QT3_PREFIX  mounted read-only (default /opt/kde3,
#              /opt/kde3-qt)
#   JOBS       containers in parallel (default 3), the configurations
#              shared out among them
#   AFTER, CONFIGS, MESSAGES  passed on (see inner.sh)
#   LIMIT_MS   sanitize-timing.py's bound per hostile document (default 100)
set -u
T=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$1" && pwd); O=${2:-$(mktemp -d)}; mkdir -p "$O"; O=$(cd "$O" && pwd)
KDE3_PREFIX=${KDE3_PREFIX:-/opt/kde3}; QT3_PREFIX=${QT3_PREFIX:-/opt/kde3-qt}
JOBS=${JOBS:-3}
LAYERS="sanitizer settings csp filter proxy"
CONFIGS=${CONFIGS:-"all $(for l in $LAYERS; do printf 'no-%s ' $l; done)$(for l in $LAYERS; do printf 'only-%s ' $l; done)external transitions two-readers no-layers"}
echo "results in $O"
# the sanitizer alone on hostile documents: each within LIMIT_MS
podman run --rm --network=none --security-opt label=disable -v "$T":/test:ro -v "$B":/build:ro \
  "${IMAGE:-localhost/kwpe-render-only}" python3 -I /test/sanitize-timing.py /build/tests/render-only/kwpe-sanitize \
  "${LIMIT_MS:-100}" > "$O/sanitize-timing.txt" 2>&1
timing=$?
set -- $CONFIGS
i=0; declare -a part
for c in "$@"; do part[$((i % JOBS))]="${part[$((i % JOBS))]:-} $c"; i=$((i + 1)); done
pids=
for j in $(seq 0 $((JOBS - 1))); do
  [ -n "${part[$j]:-}" ] || continue
  podman run --rm --network=none --cap-add NET_ADMIN,NET_RAW --security-opt label=disable --shm-size=512m \
    -v "$T":/test:ro -v "$B":/build:ro -v "$O":/out \
    -v "$KDE3_PREFIX":"$KDE3_PREFIX":ro -v "$QT3_PREFIX":"$QT3_PREFIX":ro \
    -e KDE3_PREFIX="$KDE3_PREFIX" -e AFTER="${AFTER:-2500}" -e CONFIGS="${part[$j]}" -e MESSAGES="${MESSAGES:-}" \
    "${IMAGE:-localhost/kwpe-render-only}" /test/inner.sh /build /out/job$j > "$O/job$j.log" 2>&1 &
  pids="$pids $!"
done
rc=0
for p in $pids; do wait $p || rc=1; done
cat "$O/sanitize-timing.txt" "$O"/job*/summary.txt > "$O/summary.txt" 2>/dev/null
[ $timing = 0 ] || rc=1
grep -v '^ok ' "$O/summary.txt"
[ $rc = 0 ] && echo PASS || echo FAILED
exit $rc
