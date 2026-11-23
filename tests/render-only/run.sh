#!/bin/bash
# run.sh BUILD_DIR [OUT_DIR]: the render-only test in a podman container
# with its own network namespace.  Exit 0 only if every message produced no
# DNS query and no connection attempt (and the control message rendered).
#
#   BUILD_DIR  kde3-wpepart build directory (src/libkwpepart.so,
#              src/kwpe-render-only), built against KDE3_PREFIX
#   IMAGE      container image with WPE WebKit, gumbo, Xvfb, xdotool,
#              nftables, iproute, tcpdump, python3 (see README; default
#              localhost/kwpe-render-only)
#   KDE3_PREFIX, QT3_PREFIX  mounted read-only (default /opt/kde3,
#              /opt/kde3-qt)
#   WAIT, CONFIGS, MESSAGES  passed on (see inner.sh)
set -u
T=$(cd "$(dirname "$0")" && pwd)
B=$(cd "$1" && pwd); O=${2:-$(mktemp -d)}; mkdir -p "$O"; O=$(cd "$O" && pwd)
KDE3_PREFIX=${KDE3_PREFIX:-/opt/kde3}; QT3_PREFIX=${QT3_PREFIX:-/opt/kde3-qt}
echo "results in $O"
podman run --rm --network=none --cap-add NET_ADMIN,NET_RAW --security-opt label=disable --shm-size=512m \
  -v "$T":/test:ro -v "$B":/build:ro -v "$O":/out \
  -v "$KDE3_PREFIX":"$KDE3_PREFIX":ro -v "$QT3_PREFIX":"$QT3_PREFIX":ro \
  -e KDE3_PREFIX="$KDE3_PREFIX" -e WAIT="${WAIT:-5000}" -e CONFIGS="${CONFIGS:-}" -e MESSAGES="${MESSAGES:-}" \
  "${IMAGE:-localhost/kwpe-render-only}" /test/inner.sh /build /out
