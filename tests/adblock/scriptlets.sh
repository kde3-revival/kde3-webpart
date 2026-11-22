#!/bin/sh
# scriptlets.sh DISPLAY [PORT]: the scriptlet engine against
# tests/adblock/scriptlets.txt with pages/scriptlets.html (tests/server.py
# on 127.0.0.1:PORT and 127.0.0.2:PORT, default 8392).
#
#   1. kwpe-filterconv compiles the list (as a trusted one) into the
#      part's store; kwpepartrc is pointed at that set
#   2. 127.0.0.1: every scriptlet does its job (YouTube's pattern: the
#      inline player answer, fetch and XHR of /youtubei/v1/player), in the
#      page, in a frame of the same site and of another one (127.0.0.2)
#   3. localhost (no scriptlets): nothing is changed, the frame of
#      127.0.0.2 still gets its own
#   4. 127.0.0.1 in the "ad blocking off" list: no scriptlet at all
#
# Needs the test environment of run.sh (temporary HOME/KDEHOME, the part
# in KDEDIRS, a nested X server).
dpy=$1
port=${2:-8392}
here=$(dirname "$(readlink -f "$0")")
tests=$(dirname "$here")
case "$dpy" in :0|:0.*|:1|:1.*|"") echo "scriptlets.sh: a nested display, never :0 or :1" >&2; exit 2;; esac
case "$KDEHOME" in /tmp/*) ;; *) echo "scriptlets.sh: KDEHOME must be a temporary directory (is '$KDEHOME')" >&2; exit 2;; esac
export DISPLAY=$dpy

work=$(mktemp -d /tmp/claude-1000/kwpe-scriptlets.XXXXXX 2>/dev/null || mktemp -d)
log=$work/server.log
: > "$log"
python3 "$tests/server.py" "$port" 127.0.0.1 >> "$log" 2>&1 &
s1=$!
python3 "$tests/server.py" "$port" 127.0.0.2 >> "$log" 2>&1 &
s2=$!
trap 'kill $s1 $s2 2>/dev/null' EXIT
sleep 1

base=$KDEHOME/share/apps/kwpepart/contentfilters
mkdir -p "$base/store"
id=kwpetest$(date +%s)
out=$(kwpe-filterconv --store "$base/store" --id "$id" --scriptlets "$base/$id.scriptlets" --trusted "$here/scriptlets.txt")
filters=$(echo "$out" | awk '$1 == "filter" { printf "%s%s", sep, $2; sep = "," }')
if [ -z "$filters" ] || [ ! -s "$base/$id.scriptlets" ]; then
    echo "FAIL compile: $out"
    exit 1
fi
echo "PASS compile: $filters, $(echo "$out" | awk '$2 == "scriptlets" { print $3 }') scriptlets"
conf() { kwriteconfig --file kwpepartrc --group AdBlock --key "$1" "$2"; }
conf Enabled true
conf Scriptlets true
conf Filters "$filters"
conf EnabledLists ""
conf ScriptletListsAdded true
conf DisabledSites ""
conf LastUpdate "$(date +%s)"
conf LastAttempt "$(date +%s)"

fail=0
view() {
    kwpeview "$1" > "$work/kwpeview.log" 2>&1 &
    pid=$!
    sleep "$2"
    kill $pid; wait $pid 2>/dev/null
}
check() {
    line=$(sed -n "/^--- $1\$/,\$p" "$log" | grep "^REPORT $2: DONE" | tail -1)
    if [ "$line" = "REPORT $2: DONE | expected 0 | got 0" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1: ${line:-no report}"
        sed -n "/^--- $1/,\$p" "$log" | grep "^REPORT $2:" | grep -v "DONE" \
            | awk -F'|' '{ e = $2; g = $3; sub(/^ expected /, "", e); sub(/^ got /, "", g); sub(/ +$/, "", e); sub(/ +$/, "", g); if (e != g) print "    " $0 }'
        fail=1
    fi
}
mark() { echo "--- $1" >> "$log"; }

mark on
view "http://127.0.0.1:$port/scriptlets.html?run=$$" 8
check on 127.0.0.1
mark none
view "http://localhost:$port/scriptlets.html?run=$$" 8
check none localhost
mark off
conf DisabledSites 127.0.0.1
view "http://127.0.0.1:$port/scriptlets.html?expect=off&run=$$" 8
check off 127.0.0.1
conf DisabledSites ""
if grep -q "^SERVED /ads-api/" "$log" && ! sed -n '/^--- none$/,$p' "$log" | grep -q "^SERVED /ads-api/"; then
    echo "FAIL prevented requests reached the server"; fail=1
fi
echo "log: $log"
exit $fail
