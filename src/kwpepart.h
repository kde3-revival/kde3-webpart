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

public:
    KWPEPart( QWidget* parentWidget, const char* widgetName,
              QObject* parent, const char* name,
              const QStringList& args = QStringList() );
    virtual ~KWPEPart();

    static KAboutData* createAboutData();

    virtual bool openURL( const KURL& url );
    virtual bool closeURL();

protected:
    virtual bool openFile();
    /** The page title as caption when the part becomes active (a tab). */
    virtual void guiActivateEvent( KParts::GUIActivateEvent* event );

private slots:
    void slotLoadStarted();
    void slotUrlChanged( const QString& url, bool newItem );
    void slotLoadFinished();
    void slotLoadFailed( const QString& message );
    void slotLoadProgress( int percent );
    void slotTitleChanged( const QString& title );
    void slotLinkHovered( const QString& url );
    void slotNewWindowRequested( const QString& url );
    void slotUnsupportedContent( const QString& url, const QString& mimeType );
    void slotHistoryNavigationRequested( int steps );
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
    void slotAdBlockSite();
    void slotConfigure();
    void updateAdBlockState();

private:
    friend class KWPEBrowserExtension;
    friend class KWPEPopupGUIClient;
    friend class KWPEAdBlockLabel;
    void restore( const KURL& url, int token );
    void showAdBlockMenu( const QPoint& globalPos );

    KWPEWidget* m_view;
    KWPEBrowserExtension* m_ext;
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
