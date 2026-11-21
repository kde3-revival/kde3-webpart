/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEDOWNLOAD_H
#define KWPEDOWNLOAD_H

#include <qguardedptr.h>
#include <qobject.h>

#include <kio/jobclasses.h>
#include <kurl.h>

typedef struct _WebKitDownload WebKitDownload;

class QWidget;
class KWPEDownloadJob;

/**
 * A download made by WebKit (an attachment, <a download>, "Save Link
 * As...", a POST response WebKit cannot show), with WebKit's cookies and
 * authentication, saved the KDE way: KDE's save dialog (any KIO URL),
 * progress in KIO's progress window (a KIO::Job, cancellable there).
 * WebKit writes a local destination itself (as "<file>.wkdownload" until
 * it is complete); for a remote one it writes a temporary file that KIO
 * uploads at the end.
 *
 * Deletes itself when the download is over.
 */
class KWPEDownload : public QObject
{
    Q_OBJECT

public:
    /**
     * Takes care of @p download (WebKit's download-started).  @p window:
     * the parent of the save dialog (0: none); @p destination: save
     * there without asking (empty: ask).
     */
    static void start( WebKitDownload* download, QWidget* window, const KURL& destination = KURL() );

    virtual ~KWPEDownload();

    /** From KIO's progress window. */
    void cancel();

private slots:
    void askDestination();
    void slotMoveResult( KIO::Job* job );

private:
    KWPEDownload( WebKitDownload* download, QWidget* window, const KURL& destination );
    void setDestination( const KURL& destination );
    void finish();

    friend struct KWPEDownloadCallbacks;

    WebKitDownload* m_download;     // referenced
    QGuardedPtr<QWidget> m_window;
    QGuardedPtr<KWPEDownloadJob> m_job;
    KURL m_source;
    KURL m_destination;
    QString m_suggestedName;
    QString m_partFile;             // what WebKit writes (local path)
    bool m_failed;
    bool m_cancelled;
};

/** The progress of a KWPEDownload in KIO's progress window. */
class KWPEDownloadJob : public KIO::Job
{
    Q_OBJECT

public:
    KWPEDownloadJob( KWPEDownload* download, const KURL& source, const KURL& destination );

    virtual void kill( bool quietly = true );

    void setTotal( unsigned long long total );
    void setReceived( unsigned long long received, double elapsedSeconds );
    void done( int error = 0, const QString& errorText = QString::null );

private:
    KWPEDownload* m_owner;   // 0 once the download is over
    unsigned long long m_total;
};

#endif
