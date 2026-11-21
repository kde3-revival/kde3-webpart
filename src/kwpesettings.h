/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPESETTINGS_H
#define KWPESETTINGS_H

class KConfig;

/**
 * The part's settings ($KDEHOME/share/config/kwpepartrc), shared by all
 * part instances of a process.  Konqueror may run several processes:
 * changed() tells the others (DCOP) to read the file again.
 */
namespace KWPESettings
{

KConfig* config();

enum SmoothScrolling
{
    SmoothNever,          /// wheel notches and keys scroll at once (default)
    SmoothWhenEfficient,  /// animated on the GPU (DMA-BUF) path only
    SmoothAlways          /// animated
};

/**
 * [Settings] SmoothScrolling = Never | WhenEfficient | Always in
 * kwpepartrc; without it the same key of KHTML ([KHTML Settings] in
 * khtmlrc, KDE 4's name), else Never.
 */
SmoothScrolling smoothScrolling();
void setSmoothScrolling( SmoothScrolling mode );

/**
 * [Settings] PDFInPage in kwpepartrc: PDF files shown by WebKit's PDF.js
 * in the page; false (default): they go to Konqueror like other content
 * WebKit cannot show (KDE's PDF part or application, or saving).
 */
bool pdfInPage();
void setPDFInPage( bool inPage );

/**
 * The settings were changed and written: apply them here and in every
 * other process with the part (a DCOP signal).
 */
void changed();

/** Apply the settings of the file (also when another process changed them). */
void apply();

}

#endif
