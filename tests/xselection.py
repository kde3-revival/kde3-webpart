#!/usr/bin/env python3
# Own an X selection on a nested X server and serve it, like another
# program would (for paste tests), with python-xlib.
#
#   xselection.py DISPLAY CLIPBOARD|PRIMARY SECONDS [--text T] [--html H] [--png FILE]
#
# --text is offered as UTF8_STRING, STRING, TEXT and text/plain(;charset=utf-8),
# --html as text/html, --png as image/png.  Every request is printed.
# The display must not be :0 or :1 (the real desktop).

import sys
import time

from Xlib import X, Xatom, display
from Xlib.protocol import event

args = sys.argv[1:]
if len(args) < 3 or not args[0].startswith(':') or args[0].split('.')[0] in (':0', ':1'):
    sys.exit('xselection: DISPLAY (nested, never :0/:1) CLIPBOARD|PRIMARY SECONDS [--text T] [--html H] [--png FILE]')
d = display.Display(args[0])
selection = d.intern_atom(args[1])
seconds = float(args[2])
data = {}
i = 3
while i < len(args):
    if args[i] == '--text':
        t = args[i + 1].encode('utf-8')
        for name in ('UTF8_STRING', 'text/plain;charset=utf-8', 'text/plain;charset=UTF-8'):
            data[name] = t
        for name in ('STRING', 'TEXT', 'text/plain'):
            data[name] = args[i + 1].encode('latin-1', 'replace')
    elif args[i] == '--html':
        data['text/html'] = args[i + 1].encode('utf-8')
    elif args[i] == '--png':
        with open(args[i + 1], 'rb') as f:
            data['image/png'] = f.read()
    i += 2

w = d.screen().root.create_window(0, 0, 1, 1, 0, X.CopyFromParent)
w.set_selection_owner(selection, X.CurrentTime)
d.flush()
if d.get_selection_owner(selection) != w:
    sys.exit('xselection: could not own the selection')
targets = d.intern_atom('TARGETS')
atoms = {d.intern_atom(n): n for n in data}
end = time.time() + seconds
while time.time() < end:
    while d.pending_events():
        e = d.next_event()
        if e.type == X.SelectionClear:
            print('lost the selection', flush=True)
            sys.exit(0)
        if e.type != X.SelectionRequest:
            continue
        prop = e.property if e.property != X.NONE else e.target
        name = d.get_atom_name(e.target)
        print('request', name, flush=True)
        if e.target == targets:
            e.requestor.change_property(prop, Xatom.ATOM, 32, [targets] + list(atoms))
        elif e.target in atoms:
            e.requestor.change_property(prop, e.target, 8, data[atoms[e.target]])
        else:
            prop = X.NONE
        e.requestor.send_event(event.SelectionNotify(time=e.time, requestor=e.requestor, selection=e.selection,
                                                     target=e.target, property=prop), 0)
        d.flush()
    time.sleep(0.02)
