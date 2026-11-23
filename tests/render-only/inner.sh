#!/bin/bash
# inner.sh BUILD OUT: the render-only test inside its container (root of a
# user namespace, own network namespace).  See README; run.sh starts it.
#
# Configurations (CONFIGS): all; no-<layer> (one layer off); only-<layer>
# (one layer on); external (both showings with external references);
# transitions (render only, external, render only in one reader);
# two-readers (one render only, one external at the same time); no-layers
# (the measurement must see the tricks).  See check.py for the verdicts.
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
# Showings AFTER ms apart (see driver.h): a new network process drops DNS
# prefetches until it knows the system's proxy settings, a long-lived one
# (KMail's) does not, so every message is shown at least twice.
AFTER=${AFTER:-2500}
LAYERS="sanitizer settings csp filter proxy"
ALL_CONFIGS="all $(for l in $LAYERS; do printf 'no-%s ' $l; done)$(for l in $LAYERS; do printf 'only-%s ' $l; done)external transitions two-readers no-layers"
CONFIGS=${CONFIGS:-$ALL_CONFIGS}
DRIVER=$B/tests/render-only/kwpe-render-only
# KLibLoader finds modules by name in $KDEDIRS/lib/kde3: the test's build
# of the part (libkwpepart_test, which may switch layers off), never an
# installed one.
mkdir -p /tmp/kwpe-kdedir/lib/kde3
ln -sf $B/tests/render-only/libkwpepart_test.so /tmp/kwpe-kdedir/lib/kde3/libkwpepart_test.so
PART=libkwpepart_test
mkdir -p $O; : > $O/summary.txt
log(){ echo "$*" | tee -a $O/summary.txt; }
# generated messages: nesting deeper than any parser should go
mkdir -p $O/generated
python3 -I -c 'import sys; sys.stdout.write("<!DOCTYPE html><html><body>" + "<div>" * 100000 + "x" + "</div>" * 100000 + "</body></html>")' > $O/generated/deep-nesting.html
python3 -I -c 'import sys; sys.stdout.write("<!DOCTYPE html><html><body>" + "<div>" * 4900 + "x" + "</div>" * 4900 + "<div style=\"width:320px;height:120px;background:#ff0000\"></div></body></html>")' > $O/generated/deep-flattened.html

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
  local layers= gate=strict pa=r,r pb= l
  case $c in
    all) ;;
    no-layers) layers=$(echo $LAYERS | tr ' ' ,); gate=info ;;
    no-*) layers=${c#no-}; gate=reduced ;;
    only-*) layers=$(for l in $LAYERS; do [ $l = ${c#only-} ] || printf '%s,' $l; done)
            # a layer that has to hold everything alone: the sanitizer (a
            # white list) and the content filter (every load); CSP, the
            # settings and the proxy are partial by design (they are
            # reported: what each one stops alone)
            case $c in only-sanitizer|only-filter) gate=strict ;; *) gate=info ;; esac ;;
    external) pa=e,e ;;
    transitions) pa=r,e,r ;;
    two-readers) pb=e,e ;;
  esac
  local k; k=$(echo $pa | tr ',' '\n' | wc -l)
  local wait=$(( (k - 1) * AFTER + 3500 ))
  : > $O/dns.log; : > $O/http.log
  nft reset counters table inet kwpe > /dev/null
  tcpdump -i any -nn -U -w $d/traffic.pcap > /dev/null 2> $d/tcpdump.err & local tp=$!
  sleep 0.5
  env -i HOME=$d/home XDG_RUNTIME_DIR=$d/rt TMPDIR=$d PATH=$KDE3_PREFIX/bin:/usr/bin LANG=C.UTF-8 DISPLAY=:77 \
    KDEHOME=$d/home/.kde KDETMP=$d/kdetmp KDEVARTMP=$d/kdevartmp KDEDIRS=/tmp/kwpe-kdedir:$KDE3_PREFIX KWPE_PART=$PART \
    KWPE_MAIL_TEST_DISABLE_LAYERS=$layers WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 DBUS_SESSION_BUS_ADDRESS=unix:path=$d/none \
    timeout 120 $DRIVER --phases $pa ${pb:+--reader2 $pb} --after $AFTER --wait $wait $m > $d/out 2> $d/err & local dp=$!
  if [ $n = ping-click ]; then
    # a real click (XTest) into the red link of the last showing
    sleep $(( (k - 1) * AFTER / 1000 + 2 )); local wx wy; read wx wy < <(awk '$1 == "a" && $2 == "WIDGET" { print $3, $4 }' $d/out | tail -1)
    DISPLAY=:77 xdotool mousemove $(( ${wx:-0} + 100 )) $(( ${wy:-0} + 90 )) sleep 0.3 click 1
  fi
  wait $dp; local rc=$?
  sleep 1.5   # late lookups (WebKit coalesces DNS prefetching for 1 s)
  # what the run started: WebKit's processes, KDE's (kdeinit, dcopserver,
  # klauncher, kded the driver's KApplication may start in its HOME)
  pkill -9 -f "WPEWebProcess|WPENetworkProcess|kdeinit|dcopserver|klauncher|kded|knotify" 2>/dev/null
  kill -INT $tp; wait $tp 2>/dev/null
  cp $O/dns.log $d/dns.log; cp $O/http.log $d/http.log
  local dns syn loopsyn udp offlo pkts blocked rendered link q h completed
  dns=$(count dns); syn=$(count syn); loopsyn=$(count loopsyn); udp=$(count udp); offlo=$(count offlo)
  pkts=$(tcpdump -r $d/traffic.pcap -nn 2>/dev/null | wc -l)
  blocked=$(grep -c "BLOCKED a network connection" $d/err)
  rendered=$(awk '/^RENDERED/ { print $2 }' $d/out); rendered=${rendered:-none}
  link=$(awk '$1 == "a" && $2 == "LINK" { print $3 }' $d/out | head -1)
  completed=$(grep -c "^a COMPLETED" $d/out)
  q=$(wc -l < $d/dns.log); h=$(wc -l < $d/http.log)
  local verdict why
  read verdict why < <(python3 -I $T/check.py $c $gate "$pa" "$pb" $d dns=$dns syn=$syn loopsyn=$loopsyn udp=$udp \
                       offlo=$offlo pcap=$pkts blocked=$blocked http=$h rc=$rc)
  if [ $c = no-layers ]; then
    DETECTED_DNS=$((DETECTED_DNS + q)); DETECTED_SYN=$((DETECTED_SYN + syn))
  fi
  if [ $verdict != info ]; then
    if [ $n = 00-control ] || [ $n = deep-flattened ]; then
      { [ "$rendered" = none ] || [ "$rendered" -lt 30000 ]; } && { verdict=FAIL; why="$why not-rendered:$rendered"; }
    fi
    if [ $n = deep-nesting ] || [ $n = deep-flattened ]; then
      [ $completed -ge $k ] || { verdict=FAIL; why="$why completed:$completed/$k"; }
    fi
    if [ $n = ping-click ] && ! echo "$link" | grep -q "^http://link\.ping\.a$k\.blocked\.test/$"; then
      verdict=FAIL; why="$why click-not-handed-to-host:${link:-none}"
    fi
  fi
  [ $verdict = FAIL ] && FAILED=1
  log "$(printf '%-4s %-14s %-18s dns=%s/%s syn=%s loopsyn=%s udp=%s offlo=%s pcap=%s proxy-blocked=%s http=%s rendered=%s%s%s' \
    $verdict $c $n $dns $q $syn $loopsyn $udp $offlo $pkts $blocked $h $rendered "${link:+ link=$link}" "${why:+ -> $why}")"
}

log "render-only test $(date -u +%FT%TZ) part=$B/tests/render-only/libkwpepart_test.so after=${AFTER}ms selftest=$(cat $O/selftest.txt)"
for c in $CONFIGS; do
  for m in ${MESSAGES:-$T/messages/*.html $O/generated/*.html}; do
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
