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

#include <qfile.h>
#include <qfileinfo.h>
#include <qstringlist.h>

#include <klocale.h>
#include <kmessagebox.h>
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
            paths.append( info.absFilePath() );
        }
        if ( webkit_file_chooser_request_get_select_multiple( request ) == FALSE && paths.count() > 1 )
            refused = i18n( "Only one file can be uploaded here." );
    }

    if ( result != Accepted || !refused.isEmpty() || paths.isEmpty() ) {
        webkit_file_chooser_request_cancel( request );
    } else {
        QValueList<QCString> encoded;
        for ( QStringList::ConstIterator it = paths.begin(); it != paths.end(); ++it )
            encoded.append( QFile::encodeName( *it ) );
        const gchar** files = g_new0( const gchar*, encoded.count() + 1 );
        unsigned i = 0;
        for ( QValueList<QCString>::ConstIterator it = encoded.begin(); it != encoded.end(); ++it )
            files[i++] = ( *it ).data();
        webkit_file_chooser_request_select_files( request, files );
        g_free( files );
    }
    g_object_unref( request );

    // Not modal: the message comes after the request is answered and
    // nothing refers to this dialog any more.
    if ( !refused.isEmpty() )
        KMessageBox::queuedMessageBox( parentWidget(), KMessageBox::Sorry, refused, i18n( "Upload" ) );
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

#include "kwpedialogs.moc"
