/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEPART_H
#define KWPEPART_H

#include <kparts/browserextension.h>
#include <kparts/part.h>
#include <kxmlguiclient.h>
#include <qguardedptr.h>
#include <qstringlist.h>

class KAboutData;
class KAction;
class KFindDialog;
class KToggleAction;
class QLabel;
namespace KParts { class StatusBarExtension; }
class KWPEWidget;
class KWPEBrowserExtension;

/**
 * A browser part (KParts::ReadOnlyPart + BrowserExtension) showing web
 * pages with WPE WebKit.
 *
 * History: WebKit navigates by itself (links, forms, scripts) and keeps
 * its own back/forward list; Konqueror keeps the history the user sees.
 * The part tells Konqueror about every new page (openURLNotify(), like
 * KHTML) and stores a token of WebKit's back/forward item in Konqueror's
 * history entry (saveState()); when Konqueror goes back or forward it
 * hands the token back (restoreState()) and WebKit goes to that item,
 * with its page cache, scroll position and form state.  A script going
 * back (history.back()) is turned into Konqueror's goHistory(), so the
 * two histories never disagree.  Tokens of another part instance (the
 * view showed another part in between) are unknown: the URL is loaded.
 */
class KWPEPart : public KParts::ReadOnlyPart
{
    Q_OBJECT
    /**
     * Mail mode: remote images, styles and fonts of the messages opened
     * from now on (KMail's "load external references").
     */
    Q_PROPERTY( bool mailLoadExternal READ mailLoadExternal WRITE setMailLoadExternal )
    /** Mail mode: the message is scrolled to its end (KMail's "Read On"). */
    Q_PROPERTY( bool mailAtBottom READ mailAtBottom )
    /** Mail mode: the selected text, also in a frame of the message. */
    Q_PROPERTY( QString mailSelectedText READ mailSelectedText )
    /** The part was made in app mode (an older part has no such property). */
    Q_PROPERTY( bool appMode READ isAppMode )

public:
    KWPEPart( QWidget* parentWidget, const char* widgetName,
              QObject* parent, const char* name,
              const QStringList& args = QStringList() );
    virtual ~KWPEPart();

    static KAboutData* createAboutData();

    virtual bool openURL( const KURL& url );
    virtual bool closeURL();

    /**
     * Mail mode (argument "mode=mail" at creation, for a mail program):
     * openURL() takes a local HTML file the host wrote into a private
     * directory, see KWPEWidget::loadMailFile().  Links are not followed:
     * the browser extension's openURLRequest() hands them to the host;
     * hovering emits onURL(), a right click popupMenu() (as KHTMLPart).
     * No history, no ad blocker, no downloads, no dialogs of the page.
     */
    bool isMailMode() const { return m_mail; }
    bool mailLoadExternal() const { return m_mailLoadExternal; }
    void setMailLoadExternal( bool load ) { m_mailLoadExternal = load; }
    bool mailAtBottom() const;
    QString mailSelectedText() const;

    /**
     * App mode (arguments "mode=app" and "appName=NAME" at creation, for
     * an application around one web site, e.g. kwhatsapp): a WebKit
     * context and a persistent network session of the application's own
     * (cookies, website data, permissions in "storage=DIR", by default
     * $KDEHOME/share/apps/NAME/webkit/; HTTP cache in KDE's cache
     * directory), optional "userAgent=STRING" and "sites=HOST,HOST" (the
     * hosts shown in the part; default: the host of the first URL
     * opened).  Links to other sites, new windows, mailto: and other
     * schemes go to the host through the browser extension's
     * openURLRequest(); downloads, uploads, the page's dialogs as in
     * Konqueror; notifications and permission requests are signals for
     * the host.  No history, no ad blocker.
     */
    bool isAppMode() const { return m_app; }

public slots:
    /** Scroll the page by @p dx, @p dy pixels (the host's scroll keys). */
    void scrollBy( int dx, int dy );
    /** Select the whole page. */
    void selectAll();
    /** App mode: the user clicked the notification @p id. */
    void activateNotification( int id );
    /** Load the page again. */
    void reload();

signals:
    /** Mail mode: the pointer is over a link to @p url (empty: none). */
    void onURL( const QString& url );
    /** Mail mode: a right click at @p point, on a link to @p url or not. */
    void popupMenu( const QString& url, const QPoint& point );

    /** App mode: the page's title changed (empty: it has none). */
    void pageTitleChanged( const QString& title );
    /**
     * App mode: the page shows a notification (the Notification API,
     * once allowed: see permissionRequested()); activateNotification()
     * with @p id when the user clicks it.  No event loop in a slot.
     */
    void notificationShown( int id, const QString& title, const QString& body );
    /** App mode: the page closed the notification @p id. */
    void notificationClosed( int id );
    /**
     * App mode: the page at @p origin asks for @p kind: "notifications",
     * "microphone", "camera", "screen", "geolocation" or "other".  Set
     * @p allow to grant it (false: denied).  The answer for notifications
     * is kept in the storage directory: the page is not asked again.
     * Only notifications are decided by the host: for the other kinds
     * WebKit does not say which frame asks (a frame of another site can),
     * the part refuses them, @p origin is empty and @p allow is not used.
     */
    void permissionRequested( const QString& kind, const QString& origin, bool& allow );
    /**
     * App mode: a message for the user (files dropped on the page that it
     * did not take, the page crashed, ...); setStatusBarText() has the
     * passing ones (hovered links, zoom, find).
     */
    void statusMessage( const QString& text );

protected:
    virtual bool openFile();
    /** The page title as caption when the part becomes active (a tab). */
    virtual void guiActivateEvent( KParts::GUIActivateEvent* event );

private slots:
    void slotLoadStarted();
    void slotUrlChanged( const QString& url, bool newItem );
    void slotLoadFinished();
    void slotLoadFailed( const QString& message );
    void slotLoadHandedOff();
    void slotLoadProgress( int percent );
    void slotTitleChanged( const QString& title );
    void slotLinkHovered( const QString& url );
    void slotNewWindowRequested( const QString& url );
    void slotUnsupportedContent( const QString& url, const QString& mimeType );
    void slotHistoryNavigationRequested( int steps );
    void slotExternalURL( const QString& url );
    void slotLinkActivated( const QString& url );
    void slotContextMenu( const QString& linkUrl, const QString& imageUrl, bool onSelection, const QPoint& globalPos );
    void slotFind();
    void slotFindDialogOk();
    void slotFindNext();
    void slotFindPrevious();
    void slotFindResult( bool found, unsigned matches );
    void slotWebProcessFailed( bool crashed );
    void updatePageSecurity();
    void slotZoomIn();
    void slotZoomOut();
    void slotZoomNormal();
    void slotZoomChanged( int percent );
    void slotIconURL( const QString& url );
    void slotViewSource();
    void slotSourceReady( const QString& url, const QByteArray& data );
    void slotAdBlockSite();
    void slotConfigure();
    void updateAdBlockState();

private:
    friend class KWPEBrowserExtension;
    friend class KWPEPopupGUIClient;
    friend class KWPEAdBlockLabel;
    void restore( const KURL& url, int token );
    void showAdBlockMenu( const QPoint& globalPos );
    void showAppContextMenu( const KURL& link, const KURL& image, bool onSelection, const QPoint& globalPos );

    KWPEWidget* m_view;
    KWPEBrowserExtension* m_ext;
    bool m_mail;
    bool m_app;
    bool m_mailLoadExternal;
    bool m_ownNavigation;   // openURL()/restore() started what commits next
    int m_historyToken;     // WebKit's back/forward item of m_url
    QString m_lastUnsupported;

    QGuardedPtr<KFindDialog> m_findDialog;
    QString m_findText;
    long m_findOptions;
    QStringList m_findHistory;
    KAction* m_findNext;
    KAction* m_findPrevious;
    KAction* m_zoomIn;
    KAction* m_zoomOut;
    KAction* m_zoomNormal;
    KToggleAction* m_adblockSite;
    KAction* m_configure;
    KParts::StatusBarExtension* m_statusBar;
    QGuardedPtr<QLabel> m_adblockLabel;   // in Konqueror's status bar (made when first shown)
};

class KWPEBrowserExtension : public KParts::BrowserExtension
{
    Q_OBJECT
    friend class KWPEPart;

public:
    KWPEBrowserExtension( KWPEPart* part );

    virtual void saveState( QDataStream& stream );
    virtual void restoreState( QDataStream& stream );

public slots:
    /** Konqueror's Edit/Copy (enabled while text is selected). */
    void copy();

private:
    KWPEPart* m_part;
};

/**
 * The part's actions in Konqueror's popup menu (KonqPopupMenu adds the
 * navigation, "Open in New Window/Tab", bookmarks and "Open With").
 */
class KWPEPopupGUIClient : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    KWPEPopupGUIClient( KWPEPart* part, const KURL& linkURL, const KURL& imageURL, bool onSelection );

private slots:
    void slotSaveLinkAs();
    void slotCopyLinkLocation();
    void slotSaveImageAs();
    void slotCopyImageLocation();
    void slotViewImage();

private:
    KWPEPart* m_part;
    KURL m_linkURL;
    KURL m_imageURL;
};

#endif
