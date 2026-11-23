/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEWIDGET_H
#define KWPEWIDGET_H

#include <qstringlist.h>
#include <qwidget.h>

class QTimer;
class KWPEWidgetPrivate;

/**
 * App mode: what an application around one web site gives the widget
 * (see KWPEWidget's App mode).
 */
struct KWPEAppOptions
{
    /** Website data (cookies, local storage, IndexedDB, service workers)
        and the permissions the user gave; kept across restarts. */
    QString dataDir;
    /** WebKit's HTTP cache. */
    QString cacheDir;
    /** The User-Agent header and navigator.userAgent; empty: WebKit's. */
    QString userAgent;
    /** The hosts whose pages the view shows itself; a link to any other
        goes to the host (linkActivated()).  Empty: the host of the first
        URL loaded. */
    QStringList sites;
};

/**
 * A widget showing a WebKitWebView (WPE WebKit) through the X11 WPE
 * platform of wpex11platform.h.  Qt only in this header: WebKit and
 * GLib stay in kwpewidget.cpp (Qt 3's "signals"/"slots" macros).
 *
 * WebKit navigates by itself (links, forms, scripts, redirects); the
 * widget reports what happened, the part maps that onto Konqueror.
 */
class KWPEWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * Browser: Konqueror's web browser.  Mail: a message viewer (KMail):
     * a WebKit context, network session and settings of its own, no page
     * scripts, no remote content unless allowed, every navigation handed
     * to the host (see loadMailFile()).  App: the web application of an
     * application of its own (a "site-specific browser"): a WebKit
     * context, a persistent network session and settings of its own (see
     * KWPEAppOptions), links to other sites, new windows and other
     * schemes handed to the host (linkActivated()), the page's
     * notifications and permission requests too; downloads, uploads and
     * the page's dialogs as in Browser mode; local files dropped on the
     * view go to the page as a drop.  No ad blocker, no history for the
     * host.
     */
    enum Mode { Browser, Mail, App };

    KWPEWidget( QWidget* parent = 0, const char* name = 0, Mode mode = Browser );
    /** App mode with @p options. */
    KWPEWidget( QWidget* parent, const char* name, const KWPEAppOptions& options );
    virtual ~KWPEWidget();

    /** False if WebKit could not be set up (no X11, no WPE platform). */
    bool isValid() const;

    void load( const QString& url );

    Mode mode() const;
    /**
     * Mail mode: show the HTML file @p path (written by the host into a
     * private directory).  WebKit sees it under a URL of its own
     * (kwpemail://m<n>/...): a new origin for every message, which only
     * this widget's view can read.  Only files in its directory (and
     * KDE's icons) are served; @p path is the host's document (the
     * headers), every other HTML file is message content.  Render only:
     * every HTML file is rebuilt from a white list first; no scripts, no
     * forms, no plugins, no frames of its own, no network at all (content
     * filter, a closed proxy).  @p loadExternal: remote images, styles
     * and fonts of this message, in a view and network session of its own.
     */
    void loadMailFile( const QString& path, bool loadExternal );
    /** Mail mode: the message is scrolled to its end (as last reported). */
    bool mailAtBottom() const;
    /** Mail mode: the selected text (as last reported), frames included. */
    QString mailSelectedText() const;
    /** Mail mode: scroll the page by @p dx, @p dy pixels. */
    void scrollBy( int dx, int dy );
    /** Select the whole page. */
    void selectAll();
    void reload( bool bypassCache = false );
    void stop();
    /**
     * Download @p url with WebKit (its cookies): KDE's save dialog, KIO's
     * progress window (see KWPEDownload).
     */
    void downloadURL( const QString& url );

    /**
     * Find @p text in the page (wrapping around), select the match and
     * highlight all of them; findResult() tells how it went.
     */
    void find( const QString& text, bool caseSensitive, bool wholeWords, bool backwards );
    /** The next (or previous) match of the last find(). */
    void findNext( bool forward );

    /** Page zoom in percent (text and images; 100: none). */
    int zoom() const;
    void setZoom( int percent );
    /** The next zoom step up/down (30 % ... 300 %). */
    void zoomIn();
    void zoomOut();
    bool canZoomIn() const;
    bool canZoomOut() const;

    /** The selected text (in the page or in a field) into the clipboard. */
    void copySelection();
    /** WebKit's editing commands (a text field has the focus). */
    void cut();
    void paste();
    /** The last context menu was asked for over a text field. */
    bool contextMenuOnEditable() const;

    /**
     * App mode: the user clicked the notification @p id (see
     * notificationShown()): the page gets its click event.
     */
    void activateNotification( int id );

    /**
     * Fetch the main resource's data as WebKit has it (no new request,
     * also for answers to POST): sourceReady() follows.
     */
    void requestSource();

    QString url() const;
    QString title() const;
    bool isLoading() const;
    /** https with a certificate WebKit accepted. */
    bool isSecure() const;
    /** The (https) page loaded or ran http content. */
    bool hasInsecureContent() const;

    /**
     * A token for WebKit's current back/forward item (0 if none), valid
     * as long as WebKit keeps the item; goToHistoryToken() navigates to
     * it (false if it is gone).  The part stores tokens in Konqueror's
     * history entries.
     */
    int currentHistoryToken();
    enum HistoryResult { HistoryUnknown, HistoryCurrent, HistoryNavigating };
    /**
     * Go to the back/forward item of @p token if it shows @p url:
     * HistoryNavigating (urlChanged() follows), HistoryCurrent (it is
     * shown already) or HistoryUnknown (load the URL).
     */
    HistoryResult goToHistoryToken( int token, const QString& url );

    /** "DMA-BUF" or "SHM": how frames reach the X server. */
    static QString bufferPathName();

    /** The settings of kwpepartrc (smooth scrolling) for every view. */
    static void applySettings();

signals:
    void loadStarted();
    /**
     * The main frame shows @p url now: a load committed, or a navigation
     * inside the document (anchor, history.pushState).  @p newItem: WebKit
     * made a new back/forward item for it (not for back/forward, reload,
     * location.replace()).
     */
    void urlChanged( const QString& url, bool newItem );
    void loadFinished();
    void loadFailed( const QString& message );
    /**
     * The main frame load became a download or went to Konqueror
     * (unsupportedContent()): the page before stays.
     */
    void loadHandedOff();
    void loadProgress( int percent );
    void titleChanged( const QString& title );
    void linkHovered( const QString& url );
    /** target=_blank, window.open(), a middle click on a link. */
    void newWindowRequested( const QString& url );
    /** A main frame response WebKit cannot show (PDF, archive, ...). */
    void unsupportedContent( const QString& url, const QString& mimeType );
    /**
     * A link (a click, a user gesture) to a URL WebKit does not load
     * (mailto:, ftp:, smb:, ...): for Konqueror (KIO, an application).
     */
    void externalURLRequested( const QString& url );
    /**
     * Mail mode: a link was clicked (or another navigation the user
     * started): WebKit does not follow it, the host does.
     */
    void linkActivated( const QString& url );
    /** A script went back/forward (history.back()), @p steps from here. */
    void historyNavigationRequested( int steps );
    /**
     * A context menu (right click) at @p globalPos, on a link and/or an
     * image (empty: not on one), on selected text.
     */
    void contextMenuRequested( const QString& linkUrl, const QString& imageUrl, bool onSelection, const QPoint& globalPos );
    /** The result of find(), findNext(): @p matches (up to 1000). */
    void findResult( bool found, unsigned matches );
    /**
     * The web process died and the page was not reloaded (@p crashed:
     * again within 30 s; otherwise: out of memory).  The last frame stays.
     */
    void webProcessFailed( bool crashed );
    /** hasInsecureContent() became true. */
    void insecureContentDetected();
    /** zoom() changed (setZoom(), Ctrl+wheel). */
    void zoomChanged( int percent );
    /** The page's own icon (<link rel=icon>, http/https), after a load. */
    void iconURLChanged( const QString& url );
    /** The answer to requestSource(): the bytes of @p url. */
    void sourceReady( const QString& url, const QByteArray& data );
    /**
     * App mode: the page shows a notification (the Notification API,
     * after the user allowed it): @p id for activateNotification().
     * Emitted outside WebKit's signal.
     */
    void notificationShown( int id, const QString& title, const QString& body );
    /** App mode: the page (or WebKit) closed the notification @p id. */
    void notificationClosed( int id );
    /**
     * App mode: the page asks for @p kind ("notifications", "microphone",
     * "camera", "screen", "geolocation", "other") for @p origin; @p allow
     * is false unless set.  Emitted outside WebKit's signal; the answer
     * for notifications is kept in the data directory (and the page is
     * not asked again).  Only notifications are the host's to decide:
     * WebKit does not tell which frame asks for the others (a frame of
     * another site can), so they are refused, @p origin is empty and
     * @p allow is not used.
     */
    void permissionRequested( const QString& kind, const QString& origin, bool& allow );
    /** Something the user should know (a drop the page did not take, ...). */
    void statusMessage( const QString& text );

protected:
    virtual bool event( QEvent* e );
    virtual void paintEvent( QPaintEvent* e );
    virtual void resizeEvent( QResizeEvent* e );
    virtual void showEvent( QShowEvent* e );
    virtual void hideEvent( QHideEvent* e );
    virtual void focusInEvent( QFocusEvent* e );
    virtual void focusOutEvent( QFocusEvent* e );
    virtual bool focusNextPrevChild( bool next );
    virtual void keyPressEvent( QKeyEvent* e );
    virtual void keyReleaseEvent( QKeyEvent* e );
    virtual void mousePressEvent( QMouseEvent* e );
    virtual void mouseDoubleClickEvent( QMouseEvent* e );
    virtual void mouseReleaseEvent( QMouseEvent* e );
    virtual void mouseMoveEvent( QMouseEvent* e );
    virtual void wheelEvent( QWheelEvent* e );
    virtual void enterEvent( QEvent* e );
    virtual void leaveEvent( QEvent* e );
    virtual void windowActivationChange( bool oldActive );
    virtual void dragEnterEvent( QDragEnterEvent* e );
    virtual void dragMoveEvent( QDragMoveEvent* e );
    virtual void dropEvent( QDropEvent* e );

private slots:
    void printFrameStats();
    void showContextMenu();
    void emitUnsupportedContent();
    void emitExternalURL();
    void suppressDialogs();
    void answerQuietDialogs();
    void reloadAfterCrash();
    void askAuthentication();
    void adBlockChanged();
    void reloadForFilters();
    void loadDeferred();
    void loadPendingMail();
    void askPermissions();
    void emitNotifications();
    void startDownloads();
    void reportBlocked();
    void returnToApp();

private:
    friend class KWPEWidgetPrivate;
    KWPEWidgetPrivate* d;
};

#endif
