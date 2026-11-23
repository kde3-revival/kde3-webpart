#!/bin/bash
# inner.sh BUILD OUT: the render-only test inside its container (root of a
# user namespace, own network namespace).  See README; run.sh starts it.
#
# Network: the container's namespace gets 10.77.0.1 (a logging DNS server,
# the only resolver, answering every name with 10.77.0.1, and a logging
# HTTP server) and a default route, so that any connection attempt sends a
# packet; nftables counts every DNS query, TCP SYN and other UDP packet
# leaving a process, tcpdump records everything on every interface.
set -u
T=$(cd "$(dirname "$0")" && pwd)
B=$1; O=$2
KDE3_PREFIX=${KDE3_PREFIX:-/opt/kde3}
WAIT=${WAIT:-5000}
CONFIGS=${CONFIGS:-"all no-sanitizer no-settings no-csp no-filter no-proxy external no-layers"}
DRIVER=$B/src/kwpe-render-only
# KLibLoader finds modules by name in $KDEDIRS/lib/kde3: the build's part
# first, the installed one is never used.
mkdir -p /tmp/kwpe-kdedir/lib/kde3
ln -sf $B/src/libkwpepart.so /tmp/kwpe-kdedir/lib/kde3/libkwpepart.so
PART=libkwpepart
mkdir -p $O; : > $O/summary.txt
log(){ echo "$*" | tee -a $O/summary.txt; }

# --- network namespace
ip link set lo up
if ip link add kwpe0 type dummy 2>/dev/null; then
  # no IPv6 link-local address: no router solicitations, no MLD reports
  ip link set kwpe0 addrgenmode none; ip link set kwpe0 up; ip route add default dev kwpe0; ip -6 route add default dev kwpe0 2>/dev/null
else
  ip route add default dev lo; ip -6 route add default dev lo 2>/dev/null
fi
ip addr add 10.77.0.1/32 dev lo
echo "nameserver 10.77.0.1" > /etc/resolv.conf
printf '127.0.0.1 localhost %s\n::1 localhost\n' "$(cat /proc/sys/kernel/hostname)" > /etc/hosts   # KDE's ICE looks up its host name
nft -f - <<'NFT' || { log "FAIL: nftables not usable"; exit 3; }
table inet kwpe {
  counter c_dns {}
  counter c_syn {}
  counter c_loopsyn {}
  counter c_udp {}
  counter c_offlo {}
  chain out {
    type filter hook output priority 0; policy accept;
    meta l4proto { tcp, udp } th dport 53 counter name c_dns
    tcp flags & (syn | ack) == syn ip daddr 127.0.0.0/8 counter name c_loopsyn
    tcp flags & (syn | ack) == syn ip daddr != 127.0.0.0/8 counter name c_syn
    tcp flags & (syn | ack) == syn meta nfproto ipv6 counter name c_syn
    meta l4proto udp th dport != 53 counter name c_udp
    oifname != "lo" counter name c_offlo
  }
}
NFT
count(){ nft list counter inet kwpe c_$1 | awk '/packets/ { for (i = 1; i <= NF; i++) if ($i == "packets") print $(i+1) }'; }
python3 -I $T/dnslog.py 10.77.0.1 $O/dns.log 2> $O/dnslog.err &
python3 -I $T/httplog.py 10.77.0.1 $O/http.log 2> $O/httplog.err &
Xvfb :77 -screen 0 1024x768x24 -nolisten tcp > $O/xvfb.log 2>&1 &
sleep 2
# the servers answer (and nothing of this setup is counted)
python3 -I -c 'import socket; print(socket.gethostbyname("selftest.invalid"))' > $O/selftest.txt 2>&1
: > $O/dns.log; : > $O/http.log

FAILED=0
DETECTED_DNS=0; DETECTED_SYN=0
run_one(){ # CONFIG MESSAGE
  local c=$1 m=$2 n; n=$(basename $m .html)
  local d=$O/$c/$n; mkdir -p $d/home $d/rt $d/kdetmp $d/kdevartmp; chmod 700 $d/rt $d/kdetmp $d/kdevartmp
  local layers= ext=
  case $c in
    no-layers) layers=sanitizer,settings,csp,filter,proxy ;;
    no-*) layers=${c#no-} ;;
    external) ext=--external ;;
  esac
  : > $O/dns.log; : > $O/http.log
  nft reset counters table inet kwpe > /dev/null
  tcpdump -i any -nn -U -w $d/traffic.pcap > /dev/null 2> $d/tcpdump.err & local tp=$!
  sleep 0.5
  env -i HOME=$d/home XDG_RUNTIME_DIR=$d/rt TMPDIR=$d PATH=$KDE3_PREFIX/bin:/usr/bin LANG=C.UTF-8 DISPLAY=:77 \
    KDEHOME=$d/home/.kde KDETMP=$d/kdetmp KDEVARTMP=$d/kdevartmp KDEDIRS=/tmp/kwpe-kdedir:$KDE3_PREFIX KWPE_PART=$PART KWPE_MAIL_TEST_DISABLE_LAYERS=$layers \
    WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 DBUS_SESSION_BUS_ADDRESS=unix:path=$d/none \
    timeout 90 $DRIVER $ext --wait $WAIT $m > $d/out 2> $d/err & local dp=$!
  if [ $n = ping-click ]; then
    # a real click (XTest) into the red link, 60 px below the view's top
    sleep 3; local wx wy; read wx wy < <(awk '/^WIDGET/ { print $2, $3 }' $d/out)
    DISPLAY=:77 xdotool mousemove $(( ${wx:-0} + 100 )) $(( ${wy:-0} + 90 )) sleep 0.3 click 1
  fi
  wait $dp; local rc=$?
  sleep 1.5   # late lookups (WebKit coalesces DNS prefetching for 1 s)
  # what the run started: WebKit's processes, KDE's (kdeinit, dcopserver,
  # klauncher, kded the driver's KApplication may start in its HOME)
  pkill -9 -f "WPEWebProcess|WPENetworkProcess|kdeinit|dcopserver|klauncher|kded|knotify" 2>/dev/null
  kill -INT $tp; wait $tp 2>/dev/null
  cp $O/dns.log $d/dns.log; cp $O/http.log $d/http.log
  local dns syn loopsyn udp offlo pkts blocked rendered link q h
  dns=$(count dns); syn=$(count syn); loopsyn=$(count loopsyn); udp=$(count udp); offlo=$(count offlo)
  pkts=$(tcpdump -r $d/traffic.pcap -nn 2>/dev/null | wc -l)
  blocked=$(grep -c "BLOCKED a network connection" $d/err)
  rendered=$(awk '/^RENDERED/ { print $2 }' $d/out); rendered=${rendered:-none}
  link=$(awk '/^LINK/ { print $2 }' $d/out | head -1)
  q=$(wc -l < $d/dns.log); h=$(wc -l < $d/http.log)
  local verdict=ok why=
  if [ $c = no-layers ]; then
    # the measurement itself: without any layer the tricks must be seen
    DETECTED_DNS=$((DETECTED_DNS + q)); DETECTED_SYN=$((DETECTED_SYN + syn))
    verdict=info
  elif [ $c = external ]; then
    # only images, style sheets and fonts, only names of hosts they came from
    local bad; bad=$(awk '$1 !~ /^(image|style|font)\??$/' $d/http.log)
    [ -n "$bad" ] && { verdict=FAIL; why="$why disallowed-requests:[$(echo $bad | tr '\n' ';')]"; }
    local hosts; hosts=$(awk '{ sub(/:.*/, "", $2); print $2 }' $d/http.log | sort -u)
    for qn in $(awk '$1 == 1 { print $2 }' $d/dns.log | sort -u); do
      echo "$hosts" | grep -qx "$qn" || { verdict=FAIL; why="$why lookup-without-allowed-load:$qn"; }
    done
    [ $offlo = 0 ] || { verdict=FAIL; why="$why packets-off-loopback:$offlo"; }
  else
    [ $dns = 0 ] && [ $q = 0 ] || { verdict=FAIL; why="$why dns:$dns/$q[$(awk '{print $2}' $d/dns.log | sort -u | tr '\n' ' ')]"; }
    [ $syn = 0 ] || { verdict=FAIL; why="$why tcp-connects:$syn"; }
    [ $udp = 0 ] || { verdict=FAIL; why="$why udp:$udp"; }
    [ $offlo = 0 ] || { verdict=FAIL; why="$why packets-off-loopback:$offlo"; }
    [ $h = 0 ] || { verdict=FAIL; why="$why http:$h"; }
    if [ $c = all ]; then
      # every layer in place: not even the closed proxy sees anything
      [ $loopsyn = 0 ] && [ $blocked = 0 ] && [ $pkts = 0 ] || { verdict=FAIL; why="$why loopback-connects:$loopsyn proxy-blocked:$blocked packets:$pkts"; }
    fi
  fi
  [ $rc = 0 ] || { verdict=FAIL; why="$why driver-exit:$rc"; }
  [ $verdict = info ] && [ $rc = 0 ] && { log "$(printf 'info %-13s %-20s dns=%s/%s syn=%s udp=%s offlo=%s http=%s' $c $n $dns $q $syn $udp $offlo $h)"; return; }
  if [ $n = 00-control ] && { [ "$rendered" = none ] || [ "$rendered" -lt 30000 ]; }; then verdict=FAIL; why="$why not-rendered:$rendered"; fi
  if [ $n = ping-click ] && [ "$link" != "http://link.ping.blocked.test/" ]; then verdict=FAIL; why="$why click-not-handed-to-host:${link:-none}"; fi
  [ $verdict = ok ] || FAILED=1
  log "$(printf '%-4s %-13s %-20s dns=%s/%s syn=%s loopsyn=%s udp=%s offlo=%s pcap=%s proxy-blocked=%s http=%s rendered=%s%s%s' \
    $verdict $c $n $dns $q $syn $loopsyn $udp $offlo $pkts $blocked $h $rendered "${link:+ link=$link}" "${why:+ ->$why}")"
}

log "render-only test $(date -u +%FT%TZ) part=$B/src/libkwpepart.so wait=${WAIT}ms selftest=$(cat $O/selftest.txt)"
for c in $CONFIGS; do
  for m in ${MESSAGES:-$T/messages/*.html}; do
    run_one $c $m
  done
done
# external mode: the allowed kinds really load
E=$O/external/external-allowed/http.log
if echo " $CONFIGS " | grep -q " external "; then
  for k in image style font; do
    grep -q "^$k" $E 2>/dev/null || { log "FAIL external-allowed: no $k request"; FAILED=1; }
  done
fi
# no layer at all: the same messages must be caught looking names up and
# connecting, or the measurement is blind
if echo " $CONFIGS " | grep -q " no-layers "; then
  if [ $DETECTED_DNS -gt 0 ] && [ $DETECTED_SYN -gt 0 ]; then
    log "measurement check: without any layer $DETECTED_DNS lookups, $DETECTED_SYN connection attempts were seen"
  else
    log "FAIL measurement check: without any layer only $DETECTED_DNS lookups, $DETECTED_SYN connections were seen"; FAILED=1
  fi
fi
[ $FAILED = 0 ] && log "PASS" || log "FAILED (see above: the messages marked FAIL)"
exit $FAILED
