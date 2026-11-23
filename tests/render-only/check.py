#!/usr/bin/env python3
# check.py CONFIG GATE PHASES_A PHASES_B RUNDIR MESSAGE COUNTERS...: one run's verdict.
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

config, gate, pa, pb, d, message = sys.argv[1:7]
c = dict(kv.split('=') for kv in sys.argv[7:])
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

# positive: every showing of every reader was loaded (an empty trace of a
# reader that showed nothing must not pass), and in external showings of
# external-allowed.html all three allowed kinds came
out = open(d + '/out').read().split('\n')
for tag, phases in (('a', pa), ('b', pb)):
    k = len([p for p in phases.split(',') if p])
    shown = sum(1 for l in out if l.startswith(tag + ' PHASE '))
    done = sum(1 for l in out if l == tag + ' COMPLETED')
    if shown != k or done < k:
        why.append('reader-%s:%d/%d-shown,%d-completed' % (tag, shown, k, done))
if message == 'external-allowed':
    for t, m in sorted(modes.items()):
        if m != 'e':
            continue
        kinds = set(re.sub(r'\?$', '', dest) for dest, h, _ in requests if tagre.search(h) and tagre.search(h).group(1) == t)
        for kind in ('image', 'style', 'font'):
            if kind not in kinds:
                why.append('no-%s-in-%s' % (kind, t))

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
