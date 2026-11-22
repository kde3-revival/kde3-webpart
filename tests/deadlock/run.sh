#!/bin/bash
# run.sh LIBDIR PAGE SECONDS [VAR=value...]: one isolated headless run
T=$(cd "$(dirname "$0")" && pwd); L=$1; P=$2; S=$3; shift 3
W=$(mktemp -d); mkdir $W/h $W/r; chmod 700 $W/r
env -i HOME=$W/h XDG_RUNTIME_DIR=$W/r USER=$USER LANG=C.UTF-8 PATH=/usr/bin LD_LIBRARY_PATH=$L \
  PULSE_SERVER=unix:$W/none DBUS_SESSION_BUS_ADDRESS=unix:path=$W/none "$@" \
  timeout $((S+50)) $T/stress $T/$P.html $S > $W/out 2> $W/err &
M=$!
while kill -0 $M 2>/dev/null; do
  if grep -q HUNG $W/out; then
    for p in $(pgrep -u "$(id -u)" -x WPEWebProcess); do
      tr '\0' '\n' 2>/dev/null < /proc/$p/environ | grep -qx "HOME=$W/h" || continue
      env -u DEBUGINFOD_URLS timeout 9 eu-stack -p $p > $W/hung-stack.txt 2>/dev/null
      cat /proc/$p/maps > $W/hung-maps.txt
    done
    break
  fi
  sleep 1
done
wait $M; RC=$?
for p in $(pgrep -u "$(id -u)"); do
  tr '\0' '\n' 2>/dev/null < /proc/$p/environ | grep -qx "HOME=$W/h" && kill -9 $p 2>/dev/null
done
echo "page=$P rc=$RC $(cat $W/out) dir=$W"
head -3 $W/err
exit $RC
