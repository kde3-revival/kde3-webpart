#!/usr/bin/env python3
# sanitize-timing.py KWPE_SANITIZE [LIMIT_MS]: hostile documents that make
# an HTML parser slow (deep nesting, closing tags the parser ignores, tags
# in quoted attributes, unclosed raw text, formatting reconstruction, ...)
# and large ordinary ones (KMail's HTML of long plain text messages) must
# each be sanitized within LIMIT_MS (default 100) plus four times the time
# per byte of an ordinary message on this machine (parsing is linear at
# best: a large message may take longer, but not more than linearly).
# Each must either be parsed or shown as its source text (never anything
# else), as expected where a verdict is expected: what the parser would
# work on too long (or allocate too much for) shown as source text, the
# ordinary ones parsed; the source text must keep a NUL's neighbours.
# Every run's peak RSS must stay under what Gumbo may hold (128 MB plus 48
# bytes per byte of input, 512 MB at most) plus 64 MB and twice the
# input, its output under 8 MB plus 8 times the input
# (reconstructed formatting elements copy their attributes: a long one
# cloned for every word must not make gigabytes).
# Then the CPU time limit (2 s plus 0.25 us per byte, 4 s at most), the
# net under the estimate: a shape the estimate does not count (Gumbo
# comparing the attributes of a new formatting element with those in its
# list, byte by byte) shown as source text within that limit, abandoned
# for time (not for memory).
# Last, with the limit lowered to 1 ms, an ordinary message must be
# shown as source text.  Exit 0 if all passed.
import os, re, subprocess, sys, time

tool = sys.argv[1]
limit = float(sys.argv[2]) if len(sys.argv) > 2 else 100.0
FALLBACK = b'[This message is too complex to be shown; its source:]'


def kmail(lines, line=None):
    # KMail's reader for a text/plain part (kmail/objecttreeparser.cpp,
    # quotedHTML()): every non-empty line in its own <div dir="ltr">, an
    # empty one <br>, all in <div class="noquote">; spaces kept as
    # "&nbsp; ", URLs as links.  Synthetic: a cron job's log.
    if line is None:
        def line(i):
            if i % 40 == 39:
                return '<br>'
            if i % 25 == 24:
                return '<div dir="ltr">see <a href="https://example.org/log/%d">https://example.org/log/%d</a></div>' % (i, i)
            return '<div dir="ltr">04:%02d:%02d job-%d:&nbsp; step %d done, exit status 0</div>' % (i % 60, i % 60, i // 7, i)
    return ('<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">\n<html><head><title></title>'
            '<style type="text/css">div.noquote { margin: 0 }</style></head><body>\n'
            '<div class="header" dir="ltr"><table cellspacing="1" cellpadding="0" width="100%"><tr><td>'
            '<b>From:</b>&nbsp;</td><td>Cron Daemon &lt;root@example.org&gt;</td></tr></table></div>\n'
            '<div style="position: relative"><div class="noquote">' + ''.join(line(i) for i in range(lines)) +
            '</div></div>\n</body></html>\n')


def clones(size, attribute, filler):
    # a <b> with a long style closed by </p>: each "x" after it clones it
    # (with the style); <td> outside a table is ignored (no element)
    head = '<p><b style="' + 'x' * attribute + '"></p>'
    unit = '<p>x</p>' + '<td>' * filler
    return head + unit * ((size - len(head)) // len(unit))


MB = 1024 * 1024
SOURCE, PARSED, ANY = 'source text', 'parsed', None
cases = [
    # deep stacks of open elements, which closing tags must not hide
    ('nested-div-100000', SOURCE, '<div>' * 100000 + 'x' + '</div>' * 100000),
    ('ignored-closers-30000', SOURCE, '<div></z>' * 30000 + 'x'),
    ('quoted-closers-10000', SOURCE, '<div title="</z>">' * 10000),
    ('quoted-end-tags-10000', SOURCE, '<div title="</div>">' * 10000),
    ('attribute-name-quote-10000', SOURCE, '<div =" b="></div>">' * 10000),
    ('commented-end-tags-10000', SOURCE, '<div><!--</div>-->' * 10000),
    ('cdata-end-tags-10000', SOURCE, '<div><![CDATA[</div>]]>' * 10000),
    ('body-end-30000', SOURCE, ('<div>' * 100 + '</body>') * 300),
    ('form-end-3000', SOURCE, '<form>' + '<div>' * 3000 + '</form>' * 3000 + 'x'),
    ('script-escape-5000', SOURCE, '<script><!--<script></script>' + '</div>' * 10000 + '</script>' + '<div>' * 5000),
    ('svg-style-30000', SOURCE, ('<svg><style>' + '<div>' * 100 + '</style>') * 300),
    ('span-div-10000', SOURCE, '<span><div></span>' * 10000),
    ('li-ul-10000', SOURCE, '<li><ul>' * 10000),
    ('dd-dl-10000', SOURCE, '<dl><dd>' * 10000),
    ('option-span-10000', SOURCE, '<option><span>' * 10000),
    ('heading-span-10000', SOURCE, '<h1><span>' * 10000),
    ('template-div-10000', SOURCE, '<template><div>' * 10000),
    ('b-div-20000', SOURCE, '<b><div>' * 20000 + 'x'),
    ('a-div-10000', SOURCE, '<a href=x><div>' * 10000 + 'x'),
    ('nobr-div-10000', SOURCE, '<nobr><div>' * 10000 + 'x'),
    # where an estimate of the depth went wrong before (found by comparing
    # it with a Gumbo counting its steps): end tags that close nothing
    # there, tags it does not ignore, CDATA outside svg and math
    ('select-details-20000', SOURCE, '<b>' + '<details></select><select><div></i><tbody>' * 20000),
    ('caption-table-20000', SOURCE, '<table>' + '<caption><table><g/>' * 20000),
    ('object-colgroup-20000', SOURCE, '<table>' + '</x><wbr>></em><object><colgroup><</br><em>' * 20000),
    ('svg-a-20000', SOURCE, '<svg>' + '=<a href=x></script>' * 20000),
    ('th-outside-table-3000', SOURCE, '<dl></div></tbody><i><th></li><!-- x --></title><p>' * 3000),
    ('cdata-in-html-10000', SOURCE, '<p><pre></nobr></x><keygen><![CDATA[<rt><ol>' * 10000),
    ('math-br-10000', SOURCE, '<p>' + '<marquee></ x></br><wbr><td></s><table><math><frame>' * 10000),
    ('frameset-script-20000', SOURCE, '<svg>' + '</div><option><h2><colgroup><html><frameset></title><div><script>' * 20000),
    ('math-self-closing-3000', SOURCE, '<!-- x --><frame></nobr></font></applet><math/><<noscript><font color=1>' * 3000),
    # formatting elements: reconstruction, and text after a deep one
    ('font-reconstruction', SOURCE, '<p>' + ''.join('<font color=%d>' % i for i in range(2000)) + '</p>' + '<p>x' * 20000),
    ('font-reconstruction-150x30000', SOURCE, '<p>' + ''.join('<font color=%d>' % i for i in range(150)) + '</p>' + '<p>x' * 30000),
    ('formatting-text-1M', SOURCE, '<div>' * 1500 + '<b>' + 'x' * 1000000),
    ('reconstruction-2x250000', SOURCE, '<p><font color=1><font color=2></p>' + '<p>x' * 250000),
    ('misnested-b-text', SOURCE, '<div>' * 1000 + ('<b>' + '<div>' * 9 + '</b>') * 300 + 'x' * 300000),
    ('nul-source', SOURCE, '<div>' * 20000 + 'BEFORE\0AFTER'),
    # linear for the parser: either verdict
    ('unclosed-script-32000', ANY, '<script>' * 32000),
    ('unclosed-style-32000', ANY, '<style>' * 32000),
    ('a-nested-30000', ANY, '<a href=x>' * 30000 + 'x'),
    ('tables-20000', ANY, '<table><tr><td>' * 20000),
    ('comments-100000', ANY, '<!--<div>-->' * 100000),
    ('paragraphs-50000', ANY, '<p>x' * 50000),
    # memory and output: clones of a long attribute (2.6 GB written once),
    # floods of elements (each 60-100 bytes in Gumbo for 3-4 of input)
    ('clone-attribute-stray-td-20M', SOURCE, clones(20 * MB, 100000, 200)),
    ('clone-attribute-few-20M', ANY, clones(20 * MB, 100000, 20000)),
    ('p-flood-8M', SOURCE, '<p>' * (8 * MB // 3)),
    ('br-flood-20M', SOURCE, '<br>' * (20 * MB // 4)),
    ('a-flood-20M', SOURCE, '<a>' * (20 * MB // 3)),
    # Gumbo looks for a duplicate of each attribute among those before it
    # in the tag: quadratic (once 7.7 s for 21 MB, the CPU limit's)
    ('duplicate-attributes-21M', SOURCE,
     '<b ' + ' '.join('a%d' % i for i in range(30000)) + ' a29999' * 3000000 + '>x'),
    # ordinary: parsed (nesting below the bound; KMail's plain text)
    ('nested-div-1500', PARSED, '<div>' * 1500 + 'x' + '</div>' * 1500),
    ('kmail-2300-lines', PARSED, kmail(2300)),
    ('kmail-20000-lines', PARSED, kmail(20000)),
    ('kmail-100000-lines', PARSED, kmail(100000)),
    ('kmail-200000-empty-lines', PARSED, kmail(200000, lambda i: '<br>')),
    ('kmail-200000-short-lines', PARSED, kmail(200000, lambda i: '<div dir="ltr">%d</div>' % i)),
    # about 780 bytes held per line (of one character), allowed 48 per byte
    ('kmail-300000-1-char-lines', PARSED, kmail(300000, lambda i: '<div dir="ltr">x</div>')),
    ('kmail-400000-1-char-lines', PARSED, kmail(400000, lambda i: '<div dir="ltr">x</div>')),
]


STATS = dict(os.environ, KWPE_SANITIZE_STATS='1')


def run(data, env=STATS):
    start = time.monotonic()
    p = subprocess.run([tool, 'html', 'content', 'render-only'], input=data, capture_output=True, timeout=120,
                       env=env)
    return p, (time.monotonic() - start) * 1000


def bounds(p, data):
    # peak RSS and output size within their bounds: [] or why not
    why = []
    m = re.search(rb'VmHWM:\s*(\d+) kB', p.stderr)
    if not m:
        why.append('no peak RSS')
    elif int(m.group(1)) * 1024 > min(128 * MB + 48 * len(data), 512 * MB) + 64 * MB + 2 * len(data):
        why.append('peak RSS %d MB' % (int(m.group(1)) // 1024))
    if len(p.stdout) > 8 * MB + 8 * len(data):
        why.append('output %d bytes' % len(p.stdout))
    return why


def rss(p):
    m = re.search(rb'VmHWM:\s*(\d+) kB', p.stderr)
    return int(m.group(1)) // 1024 if m else -1


# this machine's time per byte of an ordinary message (the best of three)
reference = kmail(20000).encode()
perByte = min(run(reference)[1] for _ in range(3)) / len(reference)
print('reference: %.0f ns per byte (kmail-20000-lines)' % (perByte * 1e6))
failed = 0
for name, expected, doc in cases:
    data = doc.encode()
    p, ms = run(data)
    allowed = limit + 4 * perByte * len(data)
    out = p.stdout
    fallback = FALLBACK in out
    verdict = SOURCE if fallback else PARSED
    why = []
    if p.returncode != 0:
        why.append('exit %d' % p.returncode)
    if ms > allowed:
        why.append('%.0f ms > %.0f ms' % (ms, allowed))
    if not out.strip():
        why.append('no output')
    if expected and verdict != expected:
        why.append('expected: ' + expected)
    if name == 'nul-source' and not (fallback and b'BEFORE\xef\xbf\xbdAFTER' in out):
        why.append('NUL: source text cut')
    if fallback and b'<div' in out.split(FALLBACK)[1]:
        why.append('markup in the source text')
    why += bounds(p, data)
    failed |= bool(why)
    print('%-4s %-30s %8d bytes %7.1f ms %4d MB %9d out %s%s' % ('FAIL' if why else 'ok', name, len(data), ms,
          rss(p), len(out), verdict, (' -> ' + ', '.join(why)) if why else ''))
# the safety net under the estimate: 8000 nested <b>, each with a style
# of 2500 bytes differing in its last characters; Gumbo compares each
# new one with all in its list (the Noah's Ark clause), byte by byte,
# which the estimate does not count: abandoned by the CPU time limit
# (within it and 2 s for the rest), not by the allocation budget or the
# memory bound
data = (''.join('<b style="%s%d">' % ('x' * 2500, i) for i in range(8000)) + 'x').encode()
p, ms = run(data)
cpuLimit = min(2000 + 0.25e-3 * len(data), 4000)
why = bounds(p, data)
if p.returncode != 0 or FALLBACK not in p.stdout:
    why.append('expected: source text (exit %d)' % p.returncode)
if ms > cpuLimit + 2000:
    why.append('%.0f ms > %.0f ms' % (ms, cpuLimit + 2000))
m = re.search(rb'parse abandoned: allocated (\d+) of (\d+), held (\d+) at most', p.stderr)
if not m or int(m.group(1)) > int(m.group(2)) or int(m.group(3)) > 256 * MB:
    why.append('not abandoned for time')
failed |= bool(why)
print('%-4s %-30s %8d bytes %7.1f ms %4d MB %9d out %s%s' % ('FAIL' if why else 'ok', 'formatting-attributes-cpu-limit',
      len(data), ms, rss(p), len(p.stdout), SOURCE if FALLBACK in p.stdout else PARSED,
      (' -> ' + ', '.join(why)) if why else ''))
# the same net, lowered to 1 ms by the test tool
env = dict(os.environ, KWPE_SANITIZE_CPU_LIMIT_MS='1')
p, ms = run(reference, env)
why = [] if p.returncode == 0 and FALLBACK in p.stdout else ['expected: source text (exit %d)' % p.returncode]
failed |= bool(why)
print('%-4s %-30s %8d bytes %7.1f ms %s%s' % ('FAIL' if why else 'ok', 'cpu-limit-1ms', len(reference), ms,
      SOURCE if FALLBACK in p.stdout else PARSED, (' -> ' + ', '.join(why)) if why else ''))
print('PASS' if not failed else 'FAILED')
sys.exit(failed)
