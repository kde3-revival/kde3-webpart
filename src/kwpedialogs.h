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

#include <kdialogbase.h>
#include <kfiledialog.h>

class QCheckBox;
class QLineEdit;

typedef struct _WebKitFileChooserRequest WebKitFileChooserRequest;
typedef struct _WebKitScriptDialog WebKitScriptDialog;

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

/**
 * alert(), confirm(), prompt() and the "leave this page?" question of
 * onbeforeunload.  The page's origin is the caption (a page cannot pass
 * itself off as another site or as KDE), the message is plain text (no
 * markup), at most a few thousand characters.  "Do not show more dialogs
 * from this page" (suppressRequested()) answers the page's further
 * dialogs without asking until another page is loaded.
 */
class KWPEScriptDialog : public KDialogBase
{
    Q_OBJECT

public:
    /** @p dialog is referenced until it is answered. */
    KWPEScriptDialog( WebKitScriptDialog* dialog, const QString& origin, QWidget* page );
    virtual ~KWPEScriptDialog();

    /**
     * Answer @p dialog without asking: no to confirm() and prompt(),
     * yes to leaving the page (the page must not keep the user).
     * Never inside WebKit's "script-dialog" signal: WebKit unrefs its
     * current dialog after the signal and closing clears that pointer
     * (a crash in 2.54).
     */
    static void answerQuietly( WebKitScriptDialog* dialog );

signals:
    void suppressRequested();

protected:
    virtual void done( int result );

private:
    WebKitScriptDialog* m_dialog;
    QLineEdit* m_edit;
    QCheckBox* m_suppress;
};

#endif
