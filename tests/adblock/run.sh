#!/bin/sh
# run.sh DISPLAY [PORT]: the ad blocker against tests/adblock/testlist.txt
# with pages/adblock.html (tests/server.py on 127.0.0.1:PORT, default
# 8391, reached as 127.0.0.1 and as localhost: two sites).
#
#   1. compile the test list (the part's own update, kwpe-filterconv)
#   2. 127.0.0.1: every check of the page (network, cosmetic, exceptions)
#   3. 127.0.0.1 in the "ad blocking off" list: everything loads
#   4. a page of localhost (filtered) going to 127.0.0.1 (off) by a link,
#      and back: the filters follow the site, element hiding too
#
# Needs the test environment: a temporary HOME/KDEHOME with the part in
# KDEDIRS (it rewrites kwpepartrc there), and a nested X server.
dpy=$1
port=${2:-8391}
here=$(dirname "$(readlink -f "$0")")
tests=$(dirname "$here")
case "$dpy" in :0|:0.*|:1|:1.*|"") echo "run.sh: a nested display (:83), never :0 or :1" >&2; exit 2;; esac
case "$KDEHOME" in /tmp/*) ;; *) echo "run.sh: KDEHOME must be a temporary directory (is '$KDEHOME')" >&2; exit 2;; esac
export DISPLAY=$dpy

work=$(mktemp -d /tmp/claude-1000/kwpe-adblock.XXXXXX 2>/dev/null || mktemp -d)
log=$work/server.log
# O_APPEND for both: the markers below must not be overwritten
: > "$log"
python3 "$tests/server.py" "$port" >> "$log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null' EXIT
sleep 1

list="file://$here/testlist.txt"
id=custom-$(python3 -c 'import hashlib, sys; print(hashlib.md5(sys.argv[1].encode()).hexdigest()[:12])' "$list")
conf() { kwriteconfig --file kwpepartrc --group AdBlock --key "$1" "$2"; }
conf Enabled true
conf CustomLists "$list"
conf EnabledLists "$id"
conf DisabledSites ""
conf Filters ""
conf ListsHash ""
conf LastAttempt ""

fail=0
# view URL SECONDS: kwpeview on URL for SECONDS
view() {
    kwpeview "$1" > "$work/kwpeview.log" 2>&1 &
    pid=$!
    sleep "$2"
    kill $pid; wait $pid 2>/dev/null
}
# check NAME HOST: the page on HOST reported all its checks as passed
check() {
    line=$(sed -n "/^--- $1\$/,\$p" "$log" | grep "^REPORT $2: DONE" | tail -1)
    if [ "$line" = "REPORT $2: DONE | expected 0 | got 0" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: ${line:-no report}"
        sed -n "/^--- $1/,\$p" "$log" | grep "^REPORT $2:" | grep -v "DONE\|done" \
            | awk -F'|' '{ e = $2; g = $3; sub(/ expected /, "", e); sub(/ got /, "", g); gsub(/ /, "", e); gsub(/ /, "", g); if (e != g) print "    " $0 }'
        fail=1
    fi
}
mark() { echo "--- $1" >> "$log"; }

# 1. compile
mark compile
kwpeview "http://127.0.0.1:$port/still.html" > "$work/kwpeview.log" 2>&1 &
pid=$!
for i in $(seq 1 60); do
    [ -n "$(kreadconfig --file kwpepartrc --group AdBlock --key Filters)" ] && break
    sleep 0.5
done
kill $pid; wait $pid 2>/dev/null
filters=$(kreadconfig --file kwpepartrc --group AdBlock --key Filters)
if [ -n "$filters" ]; then
    echo "PASS compile: $filters ($(kreadconfig --file kwpepartrc --group AdBlock --key Statistics))"
else
    echo "FAIL compile: $(kreadconfig --file kwpepartrc --group AdBlock --key LastResult)"
    exit 1
fi

# 2. filtered
mark filtered
view "http://127.0.0.1:$port/adblock.html" 5
check filtered 127.0.0.1
grep -q "^SERVED /ads/banner.png" "$log" && { echo "FAIL filtered: /ads/banner.png reached the server"; fail=1; }

# 3. off for the site
mark off
conf DisabledSites 127.0.0.1
view "http://127.0.0.1:$port/adblock.html?off=1" 5
check off 127.0.0.1

# 4. links between a filtered and an unfiltered site, both ways
mark cross-filtered-to-off
view "http://localhost:$port/adblock.html?next=http://127.0.0.1:$port/adblock.html%3Foff=1" 9
check cross-filtered-to-off localhost
check cross-filtered-to-off 127.0.0.1
mark cross-off-to-filtered
view "http://127.0.0.1:$port/adblock.html?off=1&next=http://localhost:$port/adblock.html" 9
check cross-off-to-filtered 127.0.0.1
check cross-off-to-filtered localhost

conf DisabledSites ""
[ $fail = 0 ] && echo "all passed" || echo "FAILED (logs in $work)"
exit $fail
