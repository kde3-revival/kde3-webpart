/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEDIALOGS_H
#define KWPEDIALOGS_H

#include <kfiledialog.h>

typedef struct _WebKitFileChooserRequest WebKitFileChooserRequest;

/**
 * Dialogs a page asks for.  They are modal to the page only: non-modal
 * windows that the widget (KWPEWidget) keeps out of the page's way: it
 * takes no input while one is open, hides it while its tab is in the
 * background and deletes it with the view.  The other tabs and the rest
 * of Konqueror stay usable, and no nested event loop runs (nothing can
 * delete the part under a dialog's feet).  Each answers WebKit's request
 * once: from the buttons, or with the default (cancel) when it is
 * deleted unanswered.
 */

/**
 * <input type=file>: KDE's file dialog (one or several files, a filter
 * from the accept= mime types, "All Files" too).  Local files only:
 * remote URLs are refused with a message, as are directories, devices
 * and unreadable files.
 */
class KWPEFileChooser : public KFileDialog
{
    Q_OBJECT

public:
    /** @p request is referenced until it is answered. */
    KWPEFileChooser( WebKitFileChooserRequest* request, QWidget* page );
    virtual ~KWPEFileChooser();

protected:
    virtual void done( int result );

private:
    static QString filterFor( const char* const* mimeTypes );

    WebKitFileChooserRequest* m_request;
};

#endif
