#!/usr/bin/env python3
# Input for testing on a nested X server (Xephyr): XTest events from
# the command line, with python-xlib.
#
#   xinject.py DISPLAY move X Y | click X Y [BUTTON] | wheel X Y NOTCHES
#              | wheelms X Y NOTCHES MS | key KEYSYM [MODIFIER...]
#              | type TEXT | sleep MS ...
#
# The display is always given explicitly and must not be :0 (the real
# desktop): XTest input goes wherever DISPLAY points.
# Commands can be chained: xinject.py :83 click 100 100 sleep 200 type "ăîșț"
# "type" looks every character up in the server's current keymap
# (setxkbmap ro std for Romanian), pressing Shift or AltGr as needed.
# NOTCHES > 0 scrolls down.

import re
import sys
import time
import unicodedata

from Xlib import X, XK, display
from Xlib.ext import xtest

if len(sys.argv) < 2 or not sys.argv[1].startswith(':') or sys.argv[1].split('.')[0] in (':0', ':1'):
    sys.exit('xinject: the first argument must be a nested display (:83), never :0 or :1')
d = display.Display(sys.argv[1])


def flush():
    d.sync()


def keycode_for(sym):
    """(keycode, needs_shift, needs_altgr) for keysym sym, or None."""
    for code, index in d.keysym_to_keycodes(sym):
        # index: 0 plain, 1 shift, 2/4 AltGr (level 3), 3/5 AltGr+Shift
        if index in (0, 1):
            return code, index == 1, False
        if index in (2, 3, 4, 5):
            return code, index in (3, 5), True
    return None


def char_keysym(ch):
    name = {' ': 'space', '\n': 'Return', '\t': 'Tab'}.get(ch)
    if name:
        return XK.string_to_keysym(name)
    cp = ord(ch)
    if cp < 0x100:
        return cp
    sym = XK.string_to_keysym(ch)
    if sym:
        return sym
    # Legacy keysyms first in layouts: abreve is 0x1e3, not 0x1000103.
    m = re.match(r'LATIN (SMALL|CAPITAL) LETTER (\w) WITH (\w+)$', unicodedata.name(ch, ''))
    if m:
        for group in ('latin2', 'latin3', 'latin4'):
            XK.load_keysym_group(group)
        letter = m.group(2).lower() if m.group(1) == 'SMALL' else m.group(2)
        legacy = XK.string_to_keysym(letter + m.group(3).lower())
        if legacy and keycode_for(legacy):
            return legacy
    return 0x01000000 | cp


def press_key(code, shift=False, altgr=False):
    shift_code = d.keysym_to_keycode(XK.XK_Shift_L)
    altgr_code = d.keysym_to_keycode(XK.string_to_keysym('ISO_Level3_Shift'))
    if shift:
        xtest.fake_input(d, X.KeyPress, shift_code)
    if altgr:
        xtest.fake_input(d, X.KeyPress, altgr_code)
    xtest.fake_input(d, X.KeyPress, code)
    xtest.fake_input(d, X.KeyRelease, code)
    if altgr:
        xtest.fake_input(d, X.KeyRelease, altgr_code)
    if shift:
        xtest.fake_input(d, X.KeyRelease, shift_code)
    flush()


def main(args):
    i = 0
    while i < len(args):
        cmd = args[i]
        if cmd == 'move':
            xtest.fake_input(d, X.MotionNotify, x=int(args[i + 1]), y=int(args[i + 2]))
            flush()
            i += 3
        elif cmd == 'click':
            x, y = int(args[i + 1]), int(args[i + 2])
            button = 1
            i += 3
            if i < len(args) and args[i].isdigit():
                button = int(args[i])
                i += 1
            xtest.fake_input(d, X.MotionNotify, x=x, y=y)
            flush()
            time.sleep(0.05)
            xtest.fake_input(d, X.ButtonPress, button)
            xtest.fake_input(d, X.ButtonRelease, button)
            flush()
        elif cmd in ('wheel', 'wheelms'):
            x, y, n = int(args[i + 1]), int(args[i + 2]), int(args[i + 3])
            interval = 0.03
            if cmd == 'wheelms':
                interval = int(args[i + 4]) / 1000.0
                i += 1
            i += 4
            xtest.fake_input(d, X.MotionNotify, x=x, y=y)
            flush()
            button = 5 if n > 0 else 4
            for _ in range(abs(n)):
                xtest.fake_input(d, X.ButtonPress, button)
                xtest.fake_input(d, X.ButtonRelease, button)
                flush()
                time.sleep(interval)
        elif cmd == 'key':
            sym = XK.string_to_keysym(args[i + 1])
            i += 2
            mods = []
            while i < len(args) and args[i] in ('ctrl', 'shift', 'alt'):
                mods.append(args[i])
                i += 1
            names = {'ctrl': 'Control_L', 'shift': 'Shift_L', 'alt': 'Alt_L'}
            mod_codes = [d.keysym_to_keycode(XK.string_to_keysym(names[m])) for m in mods]
            for c in mod_codes:
                xtest.fake_input(d, X.KeyPress, c)
            press_key(d.keysym_to_keycode(sym))
            for c in reversed(mod_codes):
                xtest.fake_input(d, X.KeyRelease, c)
            flush()
        elif cmd == 'type':
            for ch in args[i + 1]:
                found = keycode_for(char_keysym(ch))
                if not found:
                    print('xinject: no key for %r' % ch, file=sys.stderr)
                    continue
                press_key(*found)
                time.sleep(0.02)
            i += 2
        elif cmd == 'sleep':
            time.sleep(int(args[i + 1]) / 1000.0)
            i += 2
        else:
            sys.exit('xinject: unknown command ' + cmd)


main(sys.argv[2:])
