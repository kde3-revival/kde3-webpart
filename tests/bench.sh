#!/bin/sh
# bench.sh DISPLAY URL [LABEL]: kwpeview on a nested X server, idle and
# wheel scrolling, with the part's frame statistics (KWPE_STATS) and the
# memory/CPU of kwpeview and its WebKit processes (wpemem.sh).
#   KWPE_FORCE_SHM=1 bench.sh :83 https://en.wikipedia.org/wiki/KDE shm
# Needs the test environment (KDEDIRS with the part, a temporary HOME).
dpy=$1
url=$2
label=${3:-run}
here=$(dirname "$(readlink -f "$0")")
out=${BENCH_DIR:-/tmp/claude-1000/wpe/bench}
mkdir -p "$out"
log=$out/$label.log
case "$dpy" in :0|:0.*|:1|:1.*) echo "bench.sh: not on the real desktop ($dpy)" >&2; exit 2;; esac

: > "$log"
# O_APPEND: the markers written below must not be overwritten
DISPLAY=$dpy KWPE_STATS=1 kwpeview "$url" >> "$log" 2>&1 &
pid=$!
sleep "${BENCH_LOAD_WAIT:-8}"
python3 "$here/xinject.py" "$dpy" move 1590 990   # pointer away from links
sleep 3

echo "== $label: $url" | tee "$out/$label.txt"
grep -a "first paint\|finished after" "$log" | head -2 | tee -a "$out/$label.txt"
echo "-- idle" | tee -a "$out/$label.txt"
"$here/wpemem.sh" $pid 5 | tee -a "$out/$label.txt"

echo "-- scrolling (60 notches down, 30 ms apart, then 60 up)" | tee -a "$out/$label.txt"
echo "--- scroll start" >> "$log"
"$here/wpemem.sh" $pid 6 > "$out/$label.scrollmem" &
mp=$!
python3 "$here/xinject.py" "$dpy" wheel 800 500 60 wheel 800 500 -60
sleep 1
echo "--- scroll end" >> "$log"
wait $mp
sed -n '/--- scroll start/,/--- scroll end/p' "$log" | grep -a "fps" | tee -a "$out/$label.txt"
tail -1 "$out/$label.scrollmem" | tee -a "$out/$label.txt"
DISPLAY=$dpy import -window root "$out/$label.png" 2>/dev/null
kill $pid
wait $pid 2>/dev/null
