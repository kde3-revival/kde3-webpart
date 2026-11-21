/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPECLIPBOARD_H
#define KWPECLIPBOARD_H

#include <qclipboard.h>

/**
 * WebKit's clipboard (a WPEClipboard of the X11 platform) on the X
 * selections, through QClipboard:
 *
 *  - what WebKit copies (Ctrl+C in a field, a page's execCommand("copy")
 *    on a click) becomes the X CLIPBOARD: text, HTML, URI lists;
 *  - WebKit pastes from the X CLIPBOARD (Ctrl+V, Shift+Insert) or from
 *    PRIMARY (a middle click into a text field): text, HTML, images.
 *
 * Privacy: WPE has a single clipboard that WebKit reads whenever the web
 * process asks.  Other programs' data is only handed out during a paste
 * the user started here: preparePaste() (called for the key or the click)
 * opens a short window and names the selection; at any other time a read
 * gets nothing.  Scripts cannot paste by themselves (WebKit's
 * javascript-can-access-clipboard is off, DOM paste access is denied by
 * WPE without a UI).
 */
namespace KWPEClipboard
{

/**
 * The user asked to paste (a key, a middle click): WebKit may read
 * @p mode's contents for the next second.  The formats WebKit sees are
 * those of that selection.  Returns false if it has nothing.
 */
bool preparePaste( QClipboard::Mode mode );

}

#endif
