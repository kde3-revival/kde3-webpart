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

#include <qwidget.h>

class QTimer;
class KWPEWidgetPrivate;

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
    KWPEWidget( QWidget* parent = 0, const char* name = 0 );
    virtual ~KWPEWidget();

    /** False if WebKit could not be set up (no X11, no WPE platform). */
    bool isValid() const;

    void load( const QString& url );
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

private slots:
    void printFrameStats();
    void showContextMenu();
    void emitUnsupportedContent();
    void emitExternalURL();
    void reloadAfterCrash();
    void askAuthentication();
    void adBlockChanged();
    void reloadForFilters();
    void loadDeferred();

private:
    friend class KWPEWidgetPrivate;
    KWPEWidgetPrivate* d;
};

#endif
