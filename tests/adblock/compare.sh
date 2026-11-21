#!/bin/sh
# compare.sh DISPLAY URL LABEL [OUTDIR]: a page in kwpeview with the ad
# blocker off and on (kwpepartrc [AdBlock] Enabled), a screenshot of each
# after WAIT seconds (default 15), the load time and the memory of
# kwpeview and its WebKit processes.  The filters must be compiled
# already (run kwpeview once, or "Update Now").  Needs the test
# environment (a temporary KDEHOME with the part in KDEDIRS).
dpy=$1
url=$2
label=$3
out=${4:-/tmp/claude-1000/wpe/adblock}
wait=${WAIT:-15}
here=$(dirname "$(readlink -f "$0")")
case "$dpy" in :0|:0.*|:1|:1.*) echo "compare.sh: not on the real desktop ($dpy)" >&2; exit 2;; esac
[ -n "$KDEHOME" ] || { echo "compare.sh: KDEHOME is not set" >&2; exit 2; }
mkdir -p "$out"
for state in off on; do
    enabled=false; [ $state = on ] && enabled=true
    kwriteconfig --file kwpepartrc --group AdBlock --key Enabled --type bool $enabled
    log=$out/$label-$state.log
    DISPLAY=$dpy KWPE_STATS=1 kwpeview "$url" > "$log" 2>&1 &
    pid=$!
    sleep "$wait"
    DISPLAY=$dpy import -window root "$out/$label-$state.png"
    "$here/../wpemem.sh" $pid 3 > "$out/$label-$state.mem"
    kill $pid; wait $pid 2>/dev/null
    printf '%s %-3s: %s; %s\n' "$label" $state "$(grep -a 'finished after' "$log" | head -1 | sed 's/kwpe: //')" \
        "$(tail -1 "$out/$label-$state.mem" | sed 's/;.*//')"
done
kwriteconfig --file kwpepartrc --group AdBlock --key Enabled --type bool true
