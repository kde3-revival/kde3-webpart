/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WebKit (GLib) before any Qt header: Qt 3 defines "signals", "slots"
// and "emit" as macros.
#include <wpe/webkit.h>

#include <qcheckbox.h>
#include <qfile.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qfileinfo.h>
#include <qstringlist.h>

#include <kiconloader.h>
#include <klocale.h>
#include <kmimetype.h>

#include "kwpedialogs.h"

// ---------------------------------------------------------------------
// KWPEFileChooser

KWPEFileChooser::KWPEFileChooser( WebKitFileChooserRequest* request, QWidget* page )
    : KFileDialog( ":kwpepart-upload", filterFor( webkit_file_chooser_request_get_mime_types( request ) ),
                   page, "kwpefilechooser", false ),
      m_request( WEBKIT_FILE_CHOOSER_REQUEST( g_object_ref( request ) ) )
{
    bool multiple = webkit_file_chooser_request_get_select_multiple( request );
    setOperationMode( Opening );
    setMode( ( multiple ? KFile::Files : KFile::File ) | KFile::ExistingOnly | KFile::LocalOnly );
    setCaption( multiple ? i18n( "Choose Files to Upload" ) : i18n( "Choose a File to Upload" ) );
}

KWPEFileChooser::~KWPEFileChooser()
{
    // Unanswered (closed, the view deleted): nothing chosen.
    if ( m_request ) {
        webkit_file_chooser_request_cancel( m_request );
        g_object_unref( m_request );
    }
}

void KWPEFileChooser::done( int result )
{
    KFileDialog::done( result );
    if ( !m_request )
        return;
    WebKitFileChooserRequest* request = m_request;
    m_request = 0;

    QStringList paths;
    QString refused;
    if ( result == Accepted ) {
        KURL::List urls = selectedURLs();
        for ( KURL::List::ConstIterator it = urls.begin(); it != urls.end() && refused.isEmpty(); ++it ) {
            if ( !( *it ).isLocalFile() ) {
                refused = i18n( "Only local files can be uploaded:\n%1" ).arg( ( *it ).prettyURL() );
                break;
            }
            // Absolute and clean; a regular file (not a directory, nor a
            // device or a pipe WebKit would read forever) we can read.
            QFileInfo info( ( *it ).path( -1 ) );
            if ( !info.isFile() || !info.isReadable() ) {
                refused = i18n( "This is not a file that can be uploaded:\n%1" ).arg( ( *it ).path( -1 ) );
                break;
            }
            // WebKit takes UTF-8 and opens the path it decodes: a name it
            // cannot express is refused.
            QString path = info.absFilePath();
            if ( QFile::encodeName( path ) != path.utf8() ) {
                refused = i18n( "This file name cannot be uploaded:\n%1" ).arg( path );
                break;
            }
            paths.append( path );
        }
        if ( webkit_file_chooser_request_get_select_multiple( request ) == FALSE && paths.count() > 1 )
            refused = i18n( "Only one file can be uploaded here." );
    }

    if ( result != Accepted || !refused.isEmpty() || paths.isEmpty() ) {
        webkit_file_chooser_request_cancel( request );
    } else {
        // WebKit percent-decodes the paths (WebKitFileChooserRequest.cpp):
        // a file named "%2e%2e%2fsecret" would be "../secret" for it.
        QValueList<QCString> encoded;
        for ( QStringList::ConstIterator it = paths.begin(); it != paths.end(); ++it )
            encoded.append( QString( *it ).replace( '%', "%25" ).utf8() );
        const gchar** files = g_new0( const gchar*, encoded.count() + 1 );
        unsigned i = 0;
        for ( QValueList<QCString>::ConstIterator it = encoded.begin(); it != encoded.end(); ++it )
            files[i++] = ( *it ).data();
        webkit_file_chooser_request_select_files( request, files );
        g_free( files );
    }
    g_object_unref( request );

    // After the request is answered; not modal either (KMessageBox would
    // run a nested event loop for the whole application).
    if ( !refused.isEmpty() && parentWidget() ) {
        KDialogBase* box = new KDialogBase( parentWidget(), "kwpeuploadrefused", false, i18n( "Upload" ), Ok, Ok, true );
        QLabel* text = new QLabel( box );
        text->setTextFormat( PlainText );
        text->setText( refused );
        box->setMainWidget( text );
        connect( box, SIGNAL( finished() ), box, SLOT( delayedDestruct() ) );
        box->show();
    }
    deleteLater();
}

// accept="image/*,application/pdf": the patterns of these mime types
// first, then all files (accept= is a hint).
QString KWPEFileChooser::filterFor( const char* const* mimeTypes )
{
    QStringList patterns, names;
    KMimeType::List all;
    for ( const char* const* m = mimeTypes; m && *m; ++m ) {
        QString name = QString::fromLatin1( *m ).lower().stripWhiteSpace();
        if ( name.endsWith( "/*" ) ) {
            if ( all.isEmpty() )
                all = KMimeType::allMimeTypes();
            QString prefix = name.left( name.length() - 1 );
            for ( KMimeType::List::ConstIterator it = all.begin(); it != all.end(); ++it )
                if ( ( *it )->name().startsWith( prefix ) )
                    patterns += ( *it )->patterns();
            names.append( name );
        } else {
            KMimeType::Ptr type = KMimeType::mimeType( name );
            if ( !type || type->name() == KMimeType::defaultMimeType() )
                continue;
            patterns += type->patterns();
            names.append( type->comment().isEmpty() ? name : type->comment() );
        }
    }
    QString filter;
    QStringList unique;
    for ( QStringList::ConstIterator it = patterns.begin(); it != patterns.end(); ++it )
        if ( !( *it ).contains( '|' ) && !unique.contains( *it ) )
            unique.append( *it );
    if ( !unique.isEmpty() )
        filter = unique.join( " " ) + "|" + names.join( ", " ).replace( '|', ' ' ) + "\n";
    return filter + "*|" + i18n( "All Files" );
}

// ---------------------------------------------------------------------
// KWPEScriptDialog

KWPEScriptDialog::KWPEScriptDialog( WebKitScriptDialog* dialog, const QString& origin, QWidget* page )
    : KDialogBase( page, "kwpescriptdialog", false, QString::null,
                   webkit_script_dialog_get_dialog_type( dialog ) == WEBKIT_SCRIPT_DIALOG_ALERT ? Ok : Ok | Cancel, Ok, true ),
      m_dialog( webkit_script_dialog_ref( dialog ) ), m_edit( 0 )
{
    WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type( dialog );
    bool leave = type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM;
    // The origin, never the page's words: which site asks.
    setPlainCaption( ( leave ? i18n( "Leave Page? - %1" ) : i18n( "JavaScript - %1" ) ).arg( origin ) );

    QWidget* main = makeMainWidget();
    QGridLayout* grid = new QGridLayout( main, 3, 2, 0, KDialog::spacingHint() );
    QLabel* icon = new QLabel( main );
    icon->setPixmap( DesktopIcon( type == WEBKIT_SCRIPT_DIALOG_ALERT ? "messagebox_info" : "messagebox_warning", 32 ) );
    grid->addWidget( icon, 0, 0, AlignTop );

    QString message;
    if ( leave ) {
        // WebKit's text; pages cannot choose it any more.
        message = i18n( "This page asks you to confirm that you want to leave it: "
                        "what you entered may not be saved." );
        setButtonOK( KGuiItem( i18n( "&Leave Page" ) ) );
        setButtonCancel( KGuiItem( i18n( "&Stay on Page" ) ) );
    } else {
        message = QString::fromUtf8( webkit_script_dialog_get_message( dialog ) );
        if ( message.length() > 4000 )
            message = message.left( 4000 ) + QString::fromUtf8( "\xe2\x80\xa6" );
    }
    QLabel* text = new QLabel( main );
    text->setTextFormat( PlainText );
    text->setAlignment( AlignLeft | AlignTop | WordBreak );
    text->setText( message );
    text->setMinimumWidth( 300 );
    grid->addWidget( text, 0, 1 );

    if ( type == WEBKIT_SCRIPT_DIALOG_PROMPT ) {
        m_edit = new QLineEdit( main );
        m_edit->setText( QString::fromUtf8( webkit_script_dialog_prompt_get_default_text( dialog ) ) );
        m_edit->selectAll();
        grid->addWidget( m_edit, 1, 1 );
        m_edit->setFocus();
    }
    m_suppress = new QCheckBox( i18n( "Do not show more dialogs from this page" ), main );
    grid->addWidget( m_suppress, 2, 1 );
    grid->setColStretch( 1, 1 );
}

KWPEScriptDialog::~KWPEScriptDialog()
{
    // Unanswered (closed with the view): the defaults.
    if ( m_dialog ) {
        webkit_script_dialog_close( m_dialog );
        webkit_script_dialog_unref( m_dialog );
    }
}

void KWPEScriptDialog::answerQuietly( WebKitScriptDialog* dialog )
{
    if ( webkit_script_dialog_get_dialog_type( dialog ) == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM )
        webkit_script_dialog_confirm_set_confirmed( dialog, TRUE );
    webkit_script_dialog_close( dialog );
}

void KWPEScriptDialog::done( int result )
{
    KDialogBase::done( result );
    if ( !m_dialog )
        return;
    WebKitScriptDialog* dialog = m_dialog;
    m_dialog = 0;
    switch ( webkit_script_dialog_get_dialog_type( dialog ) ) {
    case WEBKIT_SCRIPT_DIALOG_CONFIRM:
    case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
        webkit_script_dialog_confirm_set_confirmed( dialog, result == Accepted );
        break;
    case WEBKIT_SCRIPT_DIALOG_PROMPT:
        // Cancel: null for the page.
        if ( result == Accepted && m_edit )
            webkit_script_dialog_prompt_set_text( dialog, m_edit->text().utf8().data() );
        break;
    default:
        break;
    }
    bool stayed = webkit_script_dialog_get_dialog_type( dialog ) == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM
                  && result != Accepted;
    webkit_script_dialog_close( dialog );
    webkit_script_dialog_unref( dialog );
    if ( m_suppress->isChecked() )
        emit suppressRequested();
    if ( stayed )
        emit stayedOnPage();
    deleteLater();
}

#include "kwpedialogs.moc"
