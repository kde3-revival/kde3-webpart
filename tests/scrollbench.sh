#!/bin/sh
# scrollbench.sh DISPLAY URL [LABEL]: wheel latency of kwpeview on a
# nested X server, from the part's statistics (KWPE_STATS): the time from
# a wheel event to the first frame on screen, and from the last wheel
# event of a burst to the last frame (the page at rest).
#   1. 10 single notches, 600 ms apart (a slow reader)
#   2. a burst of 25 notches, 12 ms apart (fast scrolling), twice
# Needs the test environment (KDEDIRS with the part, a temporary HOME).
dpy=$1
url=$2
label=${3:-run}
here=$(dirname "$(readlink -f "$0")")
out=${BENCH_DIR:-/tmp/claude-1000/wpe/bench}
mkdir -p "$out"
log=$out/$label.scroll.log
case "$dpy" in :0|:0.*|:1|:1.*) echo "scrollbench.sh: not on the real desktop ($dpy)" >&2; exit 2;; esac

: > "$log"
DISPLAY=$dpy KWPE_STATS=1 kwpeview "$url" >> "$log" 2>&1 &
pid=$!
sleep "${BENCH_LOAD_WAIT:-6}"
python3 "$here/xinject.py" "$dpy" move 1590 990
sleep 2

echo "--- single" >> "$log"
for i in 1 2 3 4 5 6 7 8 9 10; do
    python3 "$here/xinject.py" "$dpy" wheelms 800 500 1 0
    sleep 0.6
done
sleep 1.5
echo "--- burst" >> "$log"
python3 "$here/xinject.py" "$dpy" wheelms 800 500 25 12
sleep 1.5
python3 "$here/xinject.py" "$dpy" wheelms 800 500 -25 12
sleep 1.5
echo "--- end" >> "$log"
kill $pid
wait $pid 2>/dev/null

# Sums over the per-second lines: count-weighted averages and maxima.
summary() {
    awk '
    /wheel -> frame/ { n = $(NF-1); sub(/\(/, "", n); f += n; fs += $7 * n; if ($10 > fm) fm = $10 }
    /page at rest/ { n = $(NF-1); sub(/\(/, "", n); b += n; bs += $9 * n; if ($12 > bm) bm = $12 }
    END {
        printf "wheel -> first frame on screen: %.1f ms average, %.1f ms max (%d frames)\n", f ? fs / f : 0, fm, f
        printf "last wheel event -> page at rest: %.1f ms average, %.1f ms max (%d bursts)\n", b ? bs / b : 0, bm, b
    }'
}
echo "== $label: $url"
echo "-- 10 single notches, 600 ms apart"
sed -n '/--- single/,/--- burst/p' "$log" | grep -a "kwpe: .*wheel" | summary
echo "-- 2 bursts of 25 notches, 12 ms apart"
sed -n '/--- burst/,/--- end/p' "$log" | grep -a "kwpe: .*wheel" | summary
