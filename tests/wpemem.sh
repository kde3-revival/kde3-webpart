#!/bin/sh
# wpemem.sh PID: memory (RSS and PSS, MiB) and CPU over a few seconds of
# the process PID (kwpeview or konqueror) and WebKit's processes
# (WPEWebProcess, WPENetworkProcess, WPEGPUProcess) started by it.
#   wpemem.sh PID [SECONDS]
pid=$1
secs=${2:-5}
[ -d /proc/$pid ] || { echo "no process $pid" >&2; exit 1; }
pids="$pid $(pgrep -f 'WPE(Web|Network|GPU)Process' | while read p; do
    # only WebKit processes of this UI process (descendants of it)
    q=$p; while [ "$q" -gt 1 ]; do
        [ "$q" = "$pid" ] && { echo $p; break; }
        q=$(awk '{print $4}' /proc/$q/stat 2>/dev/null || echo 1)
    done
done)"
ticks() { for p in $pids; do awk '{print $14+$15}' /proc/$p/stat 2>/dev/null; done | awk '{s+=$1} END {print s+0}'; }
t0=$(ticks); sleep "$secs"; t1=$(ticks)
hz=$(getconf CLK_TCK)
total_rss=0; total_pss=0
for p in $pids; do
    [ -r /proc/$p/smaps_rollup ] || continue
    rss=$(awk '/^Rss:/{print $2}' /proc/$p/smaps_rollup)
    pss=$(awk '/^Pss:/{print $2}' /proc/$p/smaps_rollup)
    printf '%-8s %-28s RSS %6.1f MiB  PSS %6.1f MiB\n' "$p" "$(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-28)" "$(echo "$rss/1024" | bc -l)" "$(echo "$pss/1024" | bc -l)"
    total_rss=$((total_rss + rss)); total_pss=$((total_pss + pss))
done
printf 'total    RSS %.1f MiB, PSS %.1f MiB; CPU %.1f%% of one core over %ss\n' \
    "$(echo "$total_rss/1024" | bc -l)" "$(echo "$total_pss/1024" | bc -l)" \
    "$(echo "($t1-$t0)*100/$hz/$secs" | bc -l)" "$secs"
