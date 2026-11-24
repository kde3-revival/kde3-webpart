#!/bin/sh
# tests/clipboard/run.sh :DISPLAY [BUILD_DIR]: the X selection reader
# (src/kwpexselection.cpp) against owners that behave and owners that do
# not (silent, stalled INCR, oversized), on a nested X server, e.g.
#   Xvfb :61 -nolisten tcp & tests/clipboard/run.sh :61 build
# For the whole paste path: tests/pages/pasteimage.html in kwpeview or
# Konqueror with KWPE_DEBUG=1, an image in the CLIPBOARD (ksnapshot
# --fullscreen, or xclip -selection clipboard -t image/png -i FILE), Ctrl+V
# in the text box: the page lists the file it got and keeps counting; the
# log shows "kwpe: clipboard image/png: ok, N byte(s) in T ms".
case "$1" in
    :0|:0.*|:1|:1.*|"") echo "usage: $0 :DISPLAY (a nested X server, never :0/:1) [BUILD_DIR]"; exit 2;;
esac
exec "${2:-build}/src/xseltest" "$1"
