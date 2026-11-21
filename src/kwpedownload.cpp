/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WebKit (GLib) before any Qt header (Qt 3's "signals"/"slots" macros).
#include <wpe/webkit.h>

#include <stdio.h>
#include <unistd.h>

#include <qdir.h>
#include <qfile.h>
#include <qtimer.h>
#include <qwidget.h>

#include <kfiledialog.h>
#include <kio/global.h>
#include <kio/job.h>
#include <kio/netaccess.h>
#include <kio/observer.h>
#include <klocale.h>
#include <kmessagebox.h>
#include <ktempfile.h>

#include "kwpedownload.h"

struct KWPEDownloadCallbacks
{
    static gboolean decideDestination( WebKitDownload*, gchar* suggestedFilename, gpointer data )
    {
        KWPEDownload* d = static_cast<KWPEDownload*>( data );
        d->m_suggestedName = QString::fromUtf8( suggestedFilename );
        if ( d->m_destination.isValid() ) {
            d->setDestination( d->m_destination );
            return TRUE;
        }
        // Not inside WebKit's signal: the dialog runs an event loop.  The
        // download waits for webkit_download_set_destination().
        QTimer::singleShot( 0, d, SLOT( askDestination() ) );
        return TRUE;
    }

    static void receivedData( WebKitDownload* download, guint64, gpointer data )
    {
        KWPEDownload* d = static_cast<KWPEDownload*>( data );
        if ( d->m_job )
            d->m_job->setReceived( webkit_download_get_received_data_length( download ),
                                   webkit_download_get_elapsed_time( download ) );
    }

    static void failed( WebKitDownload*, GError* error, gpointer data )
    {
        KWPEDownload* d = static_cast<KWPEDownload*>( data );
        d->m_failed = true;
        if ( d->m_job ) {
            if ( g_error_matches( error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER ) )
                d->m_job->done( KIO::ERR_USER_CANCELED );
            else
                d->m_job->done( KIO::ERR_SLAVE_DEFINED,
                                i18n( "Could not download %1:\n%2" ).arg( d->m_source.prettyURL() )
                                    .arg( QString::fromUtf8( error->message ) ) );
        }
        // "finished" follows.
    }

    static void finished( WebKitDownload*, gpointer data )
    {
        static_cast<KWPEDownload*>( data )->finish();
    }
};

void KWPEDownload::start( WebKitDownload* download, QWidget* window, const KURL& destination )
{
    new KWPEDownload( download, window, destination );
}

KWPEDownload::KWPEDownload( WebKitDownload* download, QWidget* window, const KURL& destination )
    : QObject( 0, "KWPEDownload" ), m_download( WEBKIT_DOWNLOAD( g_object_ref( download ) ) ), m_window( window ),
      m_destination( destination ), m_failed( false ), m_cancelled( false )
{
    WebKitURIRequest* request = webkit_download_get_request( download );
    m_source = KURL( QString::fromUtf8( webkit_uri_request_get_uri( request ) ) );

    g_signal_connect( download, "decide-destination", G_CALLBACK( KWPEDownloadCallbacks::decideDestination ), this );
    g_signal_connect( download, "received-data", G_CALLBACK( KWPEDownloadCallbacks::receivedData ), this );
    g_signal_connect( download, "failed", G_CALLBACK( KWPEDownloadCallbacks::failed ), this );
    g_signal_connect( download, "finished", G_CALLBACK( KWPEDownloadCallbacks::finished ), this );
}

KWPEDownload::~KWPEDownload()
{
    g_signal_handlers_disconnect_by_data( m_download, this );
    g_object_unref( m_download );
    if ( m_job )
        m_job->done( KIO::ERR_USER_CANCELED );
}

void KWPEDownload::askDestination()
{
    if ( m_cancelled )
        return;

    QString name = m_suggestedName;
    if ( name.isEmpty() )
        name = m_source.fileName();
    if ( name.isEmpty() )
        name = QString::fromLatin1( "index.html" );

    // The directory of the previous download (home at first), not the
    // current directory of the process.
    static KURL lastDirectory;
    if ( lastDirectory.isEmpty() )
        lastDirectory.setPath( QDir::homeDirPath() + '/' );
    KURL start = lastDirectory;
    start.setFileName( name );
    name = start.url();

    // The dialogs run event loops: WebKit may end the download meanwhile
    // and finish() delete this (deleteLater() runs in nested loops too).
    QGuardedPtr<KWPEDownload> self( this );
    QWidget* parent = m_window ? m_window->topLevelWidget() : 0;
    KURL destination;
    for ( ;; ) {
        destination = KFileDialog::getSaveURL( name, QString::null, parent,
                                               i18n( "Save As - %1" ).arg( m_source.prettyURL() ) );
        if ( !self )
            return;
        if ( !destination.isValid() )
            break;
        // A remote destination is overwritten by KIO at the end: ask too.
        bool exists = destination.isLocalFile() ? QFile::exists( destination.path() )
                                                : KIO::NetAccess::exists( destination, false, parent );
        if ( !self )
            return;
        if ( !exists )
            break;
        int answer = KMessageBox::warningContinueCancel( parent,
                 i18n( "A file named \"%1\" already exists. Are you sure you want to overwrite it?" )
                     .arg( destination.fileName() ),
                 i18n( "Overwrite File?" ), i18n( "Overwrite" ) );
        if ( !self )
            return;
        if ( answer == KMessageBox::Continue )
            break;
        name = destination.url();
    }
    if ( m_cancelled )   // finished meanwhile (cancelled by WebKit)
        return;
    if ( !destination.isValid() ) {
        cancel();
        return;
    }
    lastDirectory = destination.upURL();
    setDestination( destination );
}

void KWPEDownload::setDestination( const KURL& destination )
{
    m_destination = destination;
    if ( destination.isLocalFile() ) {
        // WebKit writes "<file>.wkdownload" and renames it at the end.
        m_partFile = destination.path();
    } else {
        // Remote: a local file first, KIO uploads it at the end.
        KTempFile temp( QString::null, QString::fromLatin1( ".part" ) );
        temp.setAutoDelete( false );
        temp.close();
        m_partFile = temp.name();
    }

    m_job = new KWPEDownloadJob( this, m_source, m_destination );
    m_job->setWindow( m_window ? m_window->topLevelWidget() : 0 );
    WebKitURIResponse* response = webkit_download_get_response( m_download );
    if ( response && webkit_uri_response_get_content_length( response ) )
        m_job->setTotal( webkit_uri_response_get_content_length( response ) );

    webkit_download_set_allow_overwrite( m_download, TRUE );
    webkit_download_set_destination( m_download, QFile::encodeName( m_partFile ).data() );
}

void KWPEDownload::cancel()
{
    if ( m_cancelled )
        return;
    m_cancelled = true;
    // "failed" (cancelled) and "finished" follow.
    webkit_download_cancel( m_download );
}

void KWPEDownload::finish()
{
    if ( m_failed || m_cancelled || m_partFile.isEmpty() ) {
        // WebKit removes what it wrote to a local destination itself.
        if ( !m_partFile.isEmpty() && !m_destination.isLocalFile() )
            ::unlink( QFile::encodeName( m_partFile ).data() );
        m_cancelled = true;
        deleteLater();
        return;
    }

    if ( m_destination.isLocalFile() ) {
        if ( m_job )
            m_job->done();
        m_cancelled = true;
        deleteLater();
        return;
    }

    // Complete: upload it with KIO (and its progress window).
    KURL part;
    part.setPath( m_partFile );
    KIO::FileCopyJob* move = KIO::file_move( part, m_destination, -1, true /*overwrite*/, false /*resume*/,
                                             true /*progress*/ );
    connect( move, SIGNAL( result( KIO::Job* ) ), SLOT( slotMoveResult( KIO::Job* ) ) );
}

void KWPEDownload::slotMoveResult( KIO::Job* job )
{
    if ( job->error() )
        ::unlink( QFile::encodeName( m_partFile ).data() );
    if ( m_job )
        m_job->done( job->error(), job->errorText() );
    m_job = 0;
    m_cancelled = true;
    deleteLater();
}

// ---------------------------------------------------------------------

KWPEDownloadJob::KWPEDownloadJob( KWPEDownload* download, const KURL& source, const KURL& destination )
    : KIO::Job( true /*progress window*/ ), m_owner( download ), m_total( 0 )
{
    setAutoErrorHandlingEnabled( true );
    if ( progressId() )
        Observer::self()->slotCopying( this, source, destination );
}

void KWPEDownloadJob::kill( bool quietly )
{
    // KIO's progress window (Cancel), or the job's owner going away.
    KWPEDownload* owner = m_owner;
    m_owner = 0;
    if ( owner )
        owner->cancel();
    KIO::Job::kill( quietly );
}

void KWPEDownloadJob::setTotal( unsigned long long total )
{
    m_total = total;
    emit totalSize( this, total );
}

void KWPEDownloadJob::setReceived( unsigned long long received, double elapsedSeconds )
{
    setProcessedSize( received );
    emit processedSize( this, received );
    if ( m_total )
        emitPercent( received, m_total );
    if ( elapsedSeconds > 0.5 )
        emitSpeed( (unsigned long)( received / elapsedSeconds ) );
}

void KWPEDownloadJob::done( int error, const QString& errorText )
{
    if ( !m_owner )
        return;   // reported already
    m_owner = 0;
    m_error = error;
    m_errorText = errorText;
    emitResult();
}

#include "kwpedownload.moc"
