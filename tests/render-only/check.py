#!/usr/bin/env python3
# check.py CONFIG GATE PHASES_A PHASES_B RUNDIR COUNTERS...: one run's verdict.
#   GATE   strict  every layer the config has must hold everything:
#                  0 lookups, 0 connections, 0 packets (not even to the
#                  closed proxy) in render-only showings
#          reduced a layer is off: 0 lookups, 0 connections but to the
#                  part's closed proxy on 127.0.0.1, 0 requests
#          info    no verdict (layers alone that are not complete, none)
#   PHASES a1,a2,... of reader a (b): r or e; the logs' host names carry
#          the reader and the showing (*.a2.blocked.test).
# Prints "<verdict> <reasons>"; verdict ok, FAIL or info.
import re, sys

config, gate, pa, pb, d = sys.argv[1:6]
c = dict(kv.split('=') for kv in sys.argv[6:])
modes = {}
for tag, phases in (('a', pa), ('b', pb)):
    for i, m in enumerate(p for p in phases.split(',') if p):
        modes['%s%d' % (tag, i + 1)] = m
external = 'e' in modes.values()
why = []
tagre = re.compile(r'\.([ab]\d+)\.(blocked|allowed)\.test$')

def mode_of(host):
    m = tagre.search(host)
    return modes.get(m.group(1)) if m else None

lookups = [l.split()[1] for l in open(d + '/dns.log') if len(l.split()) > 1]
requests = []
for l in open(d + '/http.log'):
    p = l.split()
    if len(p) >= 4:
        requests.append((p[0], p[1].split(':')[0], p[3]))
allowed_hosts = set(h for dest, h, _ in requests if mode_of(h) == 'e' and re.match(r'^(image|style|font)\??$', dest))

for dest, h, path in requests:
    m = mode_of(h)
    if m != 'e':
        why.append('request-in-render-only:%s%s' % (h, path))
    elif not re.match(r'^(image|style|font)\??$', dest):
        why.append('disallowed-request:%s:%s%s' % (dest, h, path))
for q in sorted(set(lookups)):
    m = mode_of(q)
    if m != 'e':
        why.append('lookup-in-render-only:%s' % q)
    elif q not in allowed_hosts:
        why.append('lookup-without-allowed-load:%s' % q)

n = lambda k: int(c.get(k, '0') or 0)
if not external:
    for k in ('dns', 'syn', 'udp', 'offlo', 'http'):
        if n(k):
            why.append('%s:%d' % (k, n(k)))
else:
    if n('offlo'):
        why.append('offlo:%d' % n('offlo'))
if gate == 'strict':
    for k in ('loopsyn', 'blocked'):
        if n(k):
            why.append('%s:%d' % (k, n(k)))
    if not external and n('pcap'):
        why.append('pcap:%d' % n('pcap'))
if n('rc'):
    why.append('driver-exit:%d' % n('rc'))
if gate == 'info':
    print('info')
else:
    print(('FAIL ' + ' '.join(why)) if why else 'ok')
