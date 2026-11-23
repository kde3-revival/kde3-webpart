#!/usr/bin/env python3
# sanitize-timing.py KWPE_SANITIZE [LIMIT_MS]: hostile documents that make
# an HTML parser slow (deep nesting, closing tags the parser ignores, tags
# in quoted attributes, unclosed raw text, formatting reconstruction, ...)
# must each be sanitized within LIMIT_MS (default 100); each must either
# be parsed or shown as its source text (never anything else), and the
# source text must keep a NUL's neighbours.  Exit 0 if all passed.
import subprocess, sys, time

tool = sys.argv[1]
limit = float(sys.argv[2]) if len(sys.argv) > 2 else 100.0
FALLBACK = b'[This message is too complex to be shown; its source:]'

cases = [
    ('nested-div-100000', '<div>' * 100000 + 'x' + '</div>' * 100000),
    ('ignored-closers-30000', '<div></z>' * 30000 + 'x'),
    ('quoted-closers-10000', '<div title="</z>">' * 10000),
    ('unclosed-script-32000', '<script>' * 32000),
    ('unclosed-style-32000', '<style>' * 32000),
    ('span-div-10000', '<span><div></span>' * 10000),
    ('li-ul-10000', '<li><ul>' * 10000),
    ('font-reconstruction', '<p>' + ''.join('<font color=%d>' % i for i in range(2000)) + '</p>' + '<p>x' * 20000),
    ('b-div-20000', '<b><div>' * 20000 + 'x'),
    ('a-nested-30000', '<a href=x>' * 30000 + 'x'),
    ('tables-20000', '<table><tr><td>' * 20000),
    ('comments-100000', '<!--<div>-->' * 100000),
    ('paragraphs-50000', '<p>x' * 50000),
    # under the counting bound, parsed: nesting just below it, and node
    # reconstruction that only the allocation budget stops
    ('nested-div-1500', '<div>' * 1500 + 'x' + '</div>' * 1500),
    ('font-reconstruction-150x30000', '<p>' + ''.join('<font color=%d>' % i for i in range(150)) + '</p>' + '<p>x' * 30000),
    ('nul-source', '<div>' * 20000 + 'BEFORE\0AFTER'),
]
failed = 0
for name, doc in cases:
    data = doc.encode()
    start = time.monotonic()
    p = subprocess.run([tool, 'html', 'content', 'render-only'], input=data, capture_output=True, timeout=60)
    ms = (time.monotonic() - start) * 1000
    out = p.stdout
    fallback = FALLBACK in out
    why = []
    if p.returncode != 0:
        why.append('exit %d' % p.returncode)
    if ms > limit:
        why.append('%.0f ms > %.0f ms' % (ms, limit))
    if not out.strip():
        why.append('no output')
    if name == 'nul-source' and not (fallback and b'BEFORE\xef\xbf\xbdAFTER' in out):
        why.append('NUL: source text cut')
    if fallback and b'<div' in out.split(FALLBACK)[1]:
        why.append('markup in the source text')
    failed |= bool(why)
    print('%-4s %-24s %8d bytes %7.1f ms %s%s' % ('FAIL' if why else 'ok', name, len(data), ms,
          'source text' if fallback else 'parsed', (' -> ' + ', '.join(why)) if why else ''))
print('PASS' if not failed else 'FAILED')
sys.exit(failed)
