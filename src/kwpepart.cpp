/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include <qapplication.h>
#include <qclipboard.h>
#include <qdatastream.h>
#include <qguardedptr.h>
#include <qvariant.h>

#include <sys/stat.h>

#include <kaboutdata.h>
#include <kaction.h>
#include <kdebug.h>
#include <kdemacros.h>
#include <kfinddialog.h>
#include <kiconloader.h>
#include <knotifyclient.h>
#include <klocale.h>
#include <kmimetype.h>
#include <kstdaction.h>
#include <kstringhandler.h>
#include <kparts/browserinterface.h>
#include <kparts/event.h>
#include <kparts/genericfactory.h>
#include <kparts/partmanager.h>
#include <kparts/statusbarextension.h>
#include <kpopupmenu.h>
#include <kapplication.h>
#include <kprotocolinfo.h>
#include <krun.h>
#include <kstandarddirs.h>
#include <ktempfile.h>
#include <qfile.h>
#include <qregexp.h>
#include <kstatusbar.h>
#include <qlabel.h>
#include <qtooltip.h>

#include "kwpeadblock.h"
#include "kwpeconfigdialog.h"
#include "kwpepart.h"
#include "kwpewidget.h"

typedef KParts::GenericFactory<KWPEPart> KWPEPartFactory;
#ifdef KWPE_MAIL_TESTING
// tests/render-only's build of the module (not installed)
K_EXPORT_COMPONENT_FACTORY( libkwpepart_test, KWPEPartFactory )
#else
K_EXPORT_COMPONENT_FACTORY( libkwpepart, KWPEPartFactory )
#endif

// KLibLoader must never unload this module: it registers GObject types
// (GLib cannot unregister them) and an X event filter in Qt.
extern "C" {
KDE_EXPORT int __kde_do_not_unload = 1;
}

// The value of the creation argument "key=value" (null if none).
static QString argument( const QStringList& args, const char* key )
{
    QString prefix = QString::fromLatin1( key ) + "=";
    for ( QStringList::ConstIterator it = args.begin(); it != args.end(); ++it )
        if ( ( *it ).startsWith( prefix ) )
            return ( *it ).mid( prefix.length() );
    return QString::null;
}

// App mode: the options of the creation arguments (see isAppMode()).
static KWPEAppOptions appOptions( const QStringList& args )
{
    KWPEAppOptions options;
    QString name = argument( args, "appName" );
    name.replace( '/', '_' );
    if ( name.isEmpty() || name.startsWith( "." ) )
        name = QString::fromLatin1( "kwpeapp" );
    options.dataDir = argument( args, "storage" );
    if ( options.dataDir.isEmpty() )
        options.dataDir = KGlobal::dirs()->saveLocation( "data", name + "/webkit/" );
    options.cacheDir = KGlobal::dirs()->saveLocation( "cache", name + "-webkit/" );
    options.userAgent = argument( args, "userAgent" );
    options.sites = QStringList::split( ',', argument( args, "sites" ) );
    options.pictureOrigins = QStringList::split( ',', argument( args, "pictureOrigins" ) );
    options.debug = argument( args, "debug" ) == "true";
    return options;
}

// Konqueror's KonqMainWindow::PageSecurity
enum { NotCrypted = 0, Encrypted = 1, Mixed = 2 };

static const char stateMagic[] = "kwpepart-1";

// The ad blocker's state in Konqueror's status bar; a click shows its menu.
class KWPEAdBlockLabel : public QLabel
{
public:
    KWPEAdBlockLabel( KWPEPart* part, QWidget* parent ) : QLabel( parent ), m_part( part ) {}

protected:
    virtual void mousePressEvent( QMouseEvent* e )
    {
        m_part->showAdBlockMenu( e->globalPos() );
    }

private:
    KWPEPart* m_part;
};

KWPEPart::KWPEPart( QWidget* parentWidget, const char* widgetName,
                    QObject* parent, const char* name, const QStringList& args )
    : KParts::ReadOnlyPart( parent, name ), m_mail( args.contains( "mode=mail" ) ), m_app( !m_mail && args.contains( "mode=app" ) ),
      m_mailLoadExternal( false ),
      m_ownNavigation( false ), m_historyToken( 0 ), m_findOptions( 0 ), m_adblockLabel( 0 )
{
    setInstance( KWPEPartFactory::instance() );

    if ( m_app )
        m_view = new KWPEWidget( parentWidget, widgetName, appOptions( args ) );
    else
        m_view = new KWPEWidget( parentWidget, widgetName, m_mail ? KWPEWidget::Mail : KWPEWidget::Browser );
    setWidget( m_view );
    if ( !m_view->isValid() )
        kdWarning() << "kwpepart: WPE WebKit could not be set up" << endl;

    m_ext = new KWPEBrowserExtension( this );

    connect( m_view, SIGNAL( loadStarted() ), SLOT( slotLoadStarted() ) );
    connect( m_view, SIGNAL( urlChanged( const QString&, bool ) ), SLOT( slotUrlChanged( const QString&, bool ) ) );
    connect( m_view, SIGNAL( loadFinished() ), SLOT( slotLoadFinished() ) );
    connect( m_view, SIGNAL( loadFailed( const QString& ) ), SLOT( slotLoadFailed( const QString& ) ) );
    connect( m_view, SIGNAL( loadHandedOff() ), SLOT( slotLoadHandedOff() ) );
    connect( m_view, SIGNAL( loadProgress( int ) ), SLOT( slotLoadProgress( int ) ) );
    connect( m_view, SIGNAL( titleChanged( const QString& ) ), SLOT( slotTitleChanged( const QString& ) ) );
    connect( m_view, SIGNAL( linkHovered( const QString& ) ), SLOT( slotLinkHovered( const QString& ) ) );
    connect( m_view, SIGNAL( newWindowRequested( const QString& ) ), SLOT( slotNewWindowRequested( const QString& ) ) );
    connect( m_view, SIGNAL( unsupportedContent( const QString&, const QString& ) ),
             SLOT( slotUnsupportedContent( const QString&, const QString& ) ) );
    connect( m_view, SIGNAL( historyNavigationRequested( int ) ), SLOT( slotHistoryNavigationRequested( int ) ) );
    connect( m_view, SIGNAL( externalURLRequested( const QString& ) ), SLOT( slotExternalURL( const QString& ) ) );
    connect( m_view, SIGNAL( linkActivated( const QString& ) ), SLOT( slotLinkActivated( const QString& ) ) );
    connect( m_view, SIGNAL( contextMenuRequested( const QString&, const QString&, bool, const QPoint& ) ),
             SLOT( slotContextMenu( const QString&, const QString&, bool, const QPoint& ) ) );
    // Always: WPE does not report selection changes (WebKitEditorState
    // never changes with the WPE platform API); copying nothing does nothing.
    m_ext->enableAction( "copy", true );
    connect( m_view, SIGNAL( findResult( bool, unsigned ) ), SLOT( slotFindResult( bool, unsigned ) ) );

    KStdAction::find( this, SLOT( slotFind() ), actionCollection(), "find" );
    m_findNext = KStdAction::findNext( this, SLOT( slotFindNext() ), actionCollection(), "findNext" );
    m_findPrevious = KStdAction::findPrev( this, SLOT( slotFindPrevious() ), actionCollection(), "findPrevious" );
    m_findNext->setEnabled( false );
    m_findPrevious->setEnabled( false );

    // Ctrl+= too: "+" needs Shift on most keyboards.
    m_zoomIn = KStdAction::zoomIn( this, SLOT( slotZoomIn() ), actionCollection(), "zoomIn" );
    m_zoomIn->setShortcut( KShortcut( "CTRL++;CTRL+=" ) );
    m_zoomOut = KStdAction::zoomOut( this, SLOT( slotZoomOut() ), actionCollection(), "zoomOut" );
    m_zoomNormal = KStdAction::actualSize( this, SLOT( slotZoomNormal() ), actionCollection(), "zoomNormal" );
    m_zoomNormal->setText( i18n( "Reset Zoom" ) );
    m_zoomNormal->setShortcut( KShortcut( "CTRL+0" ) );
    m_zoomNormal->setEnabled( false );
    connect( m_view, SIGNAL( zoomChanged( int ) ), SLOT( slotZoomChanged( int ) ) );
    connect( m_view, SIGNAL( iconURLChanged( const QString& ) ), SLOT( slotIconURL( const QString& ) ) );

    // KHTML's name and shortcut
    new KAction( i18n( "View Do&cument Source" ), CTRL + Key_U, this, SLOT( slotViewSource() ),
                 actionCollection(), "viewDocumentSource" );
    connect( m_view, SIGNAL( sourceReady( const QString&, const QByteArray& ) ),
             SLOT( slotSourceReady( const QString&, const QByteArray& ) ) );
    connect( m_view, SIGNAL( webProcessFailed( bool ) ), SLOT( slotWebProcessFailed( bool ) ) );
    connect( m_view, SIGNAL( insecureContentDetected() ), SLOT( updatePageSecurity() ) );

    // Ad blocking: off for the site shown (a list in kwpepartrc), the
    // settings dialog.  WebKit does not tell what its content filters
    // blocked, so the status bar shows only whether they apply here.
    m_adblockSite = new KToggleAction( i18n( "Disable Ad &Blocking on This Site" ), 0, this, SLOT( slotAdBlockSite() ),
                                       actionCollection(), "adblockSite" );
    m_configure = new KAction( i18n( "&Configure WPE WebKit..." ), "configure", 0, this, SLOT( slotConfigure() ),
                               actionCollection(), "configureWPE" );
    m_statusBar = new KParts::StatusBarExtension( this );
    if ( !m_mail && !m_app ) {
        connect( KWPEAdBlock::self(), SIGNAL( changed() ), SLOT( updateAdBlockState() ) );
        updateAdBlockState();
    } else {
        m_adblockSite->setEnabled( false );
    }

    if ( m_app ) {
        connect( m_view, SIGNAL( notificationShown( int, const QString&, const QString& ) ),
                 SIGNAL( notificationShown( int, const QString&, const QString& ) ) );
        connect( m_view, SIGNAL( notificationShown( int, const QString&, const QString&, const QImage& ) ),
                 SIGNAL( notificationShown( int, const QString&, const QString&, const QImage& ) ) );
        connect( m_view, SIGNAL( notificationClosed( int ) ), SIGNAL( notificationClosed( int ) ) );
        connect( m_view, SIGNAL( permissionRequested( const QString&, const QString&, bool& ) ),
                 SIGNAL( permissionRequested( const QString&, const QString&, bool& ) ) );
        connect( m_view, SIGNAL( statusMessage( const QString& ) ), SIGNAL( statusMessage( const QString& ) ) );
        KStdAction::redisplay( this, SLOT( reload() ), actionCollection(), "reload" )->setText( i18n( "&Reload" ) );
        // Find, zoom and reload only (kwpeapppart.rc).
        setXMLFile( "kwpeapppart.rc" );
        return;
    }
    setXMLFile( "kwpepart.rc" );
}

KWPEPart::~KWPEPart()
{
    delete m_findDialog;
    // a child of Konqueror's status bar, which outlives the part
    if ( m_adblockLabel ) {
        m_statusBar->removeStatusBarItem( m_adblockLabel );
        delete static_cast<QLabel*>( m_adblockLabel );
    }
}

KAboutData* KWPEPart::createAboutData()
{
    KAboutData* about = new KAboutData( "kwpepart", I18N_NOOP( "WPE WebKit" ), "0.1",
                                        I18N_NOOP( "Embeddable web browser component using WPE WebKit" ),
                                        KAboutData::License_LGPL, "(c) 2026 The kde3-revival authors" );
    return about;
}

bool KWPEPart::openURL( const KURL& url )
{
    if ( !m_view->isValid() ) {
        emit canceled( i18n( "WPE WebKit could not be set up." ) );
        return false;
    }
    if ( m_app ) {
        m_url = url;
        m_view->load( url.url() );
        return true;
    }
    if ( m_mail ) {
        if ( !url.isLocalFile() ) {
            emit canceled( i18n( "Only local files can be shown." ) );
            return false;
        }
        m_url = url;
        m_view->loadMailFile( url.path(), m_mailLoadExternal );
        return true;
    }
    KParts::URLArgs args = m_ext->urlArgs();
    m_url = url;
    m_historyToken = 0;
    m_ownNavigation = true;
    emit setWindowCaption( url.prettyURL() );

    // Konqueror's Reload (F5): revalidate with the server, like KHTML.
    if ( args.reload && urlcmp( m_view->url(), url.url(), true, false ) )
        m_view->reload();
    else
        m_view->load( url.url() );
    // POST data from Konqueror (args.postData): not yet, see README.
    return true;
}

bool KWPEPart::closeURL()
{
    if ( m_view->isLoading() )
        m_view->stop();
    return true;
}

bool KWPEPart::openFile()
{
    // openURL() hands URLs to WebKit, no local copy is ever made.
    return false;
}

void KWPEPart::guiActivateEvent( KParts::GUIActivateEvent* event )
{
    // ReadOnlyPart's shows the URL.
    if ( !event->activated() || m_mail || m_app )
        return;
    emit setWindowCaption( m_view->title().isEmpty() ? m_url.prettyURL() : m_view->title() );
    // The status bar exists now (Konqueror sets it late); the extension
    // shows the label while this part is active.
    if ( !m_adblockLabel && m_statusBar->statusBar() ) {
        m_adblockLabel = new KWPEAdBlockLabel( this, m_statusBar->statusBar() );
        m_adblockLabel->setMargin( 2 );
        // permanent: a status bar message must not hide it
        m_statusBar->addStatusBarItem( m_adblockLabel, 0, true );
        updateAdBlockState();
    }
}

void KWPEPart::updateAdBlockState()
{
    if ( m_mail || m_app )
        return;
    KWPEAdBlock* adblock = KWPEAdBlock::self();
    QString host = m_url.host();
    bool web = m_url.protocol() == "http" || m_url.protocol() == "https";
    m_adblockSite->setEnabled( adblock->isEnabled() && web && !host.isEmpty() );
    m_adblockSite->setChecked( web && adblock->isSiteDisabled( host ) );
    if ( !m_adblockLabel )
        return;
    QString text, tip;
    if ( !adblock->isEnabled() ) {
        text = i18n( "short for: ad blocking is off", "Ads: off" );
        tip = i18n( "Ad blocking is off." );
    } else if ( web && adblock->isSiteDisabled( host ) ) {
        text = i18n( "short for: ads are allowed on this site", "Ads: allowed" );
        tip = i18n( "Ad blocking is off for %1." ).arg( host );
    } else if ( adblock->isBlocking( host ) ) {
        text = i18n( "short for: ads are blocked", "Ads: blocked" );
        tip = i18n( "Advertisements and trackers are blocked." );
    } else {
        text = i18n( "short for: no ad filters yet", "Ads: none yet" );
    }
    if ( adblock->isUpdating() )
        text += i18n( " (updating)" );
    m_adblockLabel->setText( text );
    QToolTip::remove( m_adblockLabel );
    QString status = adblock->status();
    QToolTip::add( m_adblockLabel, tip.isEmpty() ? status : tip + "\n" + status );
}

void KWPEPart::showAdBlockMenu( const QPoint& globalPos )
{
    KPopupMenu menu( widget() );
    m_adblockSite->plug( &menu );
    m_configure->plug( &menu );
    menu.exec( globalPos );
    m_adblockSite->unplug( &menu );
    m_configure->unplug( &menu );
}

void KWPEPart::slotAdBlockSite()
{
    QString host = m_url.host();
    if ( host.isEmpty() )
        return;
    KWPEAdBlock::self()->setSiteDisabled( host, m_adblockSite->isChecked() );
    // The filters of the view follow the list at once; the page is loaded
    // again with them (or without).
    m_view->reload();
}

void KWPEPart::slotConfigure()
{
    KWPEConfigDialog::showDialog( widget() );
}

void KWPEPart::restore( const KURL& url, int token )
{
    KWPEWidget::HistoryResult result = token ? m_view->goToHistoryToken( token, url.url() ) : KWPEWidget::HistoryUnknown;
    if ( result == KWPEWidget::HistoryCurrent ) {
        // Already there (the view was not switched away).
        m_url = url;
        m_historyToken = token;
        emit m_ext->setLocationBarURL( url.prettyURL() );
        emit started( 0 );
        emit completed();
        return;
    }
    if ( result == KWPEWidget::HistoryNavigating ) {
        m_url = url;
        m_historyToken = token;
        m_ownNavigation = true;
        emit m_ext->setLocationBarURL( url.prettyURL() );
        return;
    }
    m_ext->setURLArgs( KParts::URLArgs() );
    openURL( url );
}

void KWPEPart::slotLoadStarted()
{
    emit started( 0 );
}

void KWPEPart::slotUrlChanged( const QString& url, bool newItem )
{
    // Mail: the host knows what it shows (m_url, the file it opened);
    // no history.  App: no history either.
    if ( m_app )
        m_url = KURL( url );
    if ( m_mail || m_app ) {
        if ( !m_view->isLoading() ) {
            emit started( 0 );
            emit completed();
        }
        return;
    }
    KURL u( url );
    m_lastUnsupported = QString::null;
    if ( m_ownNavigation )
        m_ownNavigation = false;
    else if ( newItem )
        // Konqueror saves the page shown so far (url(), saveState()) into
        // its current history entry and appends a new one.
        emit m_ext->openURLNotify();

    m_url = u;
    m_historyToken = m_view->currentHistoryToken();
    emit m_ext->setLocationBarURL( u.prettyURL() );
    updatePageSecurity();
    updateAdBlockState();

    // A navigation inside the document has no load: Konqueror fills its
    // history entry on completed().
    if ( !m_view->isLoading() ) {
        emit started( 0 );
        emit completed();
    }
}

void KWPEPart::slotLoadFinished()
{
    emit completed();
}

void KWPEPart::slotLoadFailed( const QString& message )
{
    m_ownNavigation = false;
    emit canceled( message );
}

void KWPEPart::slotLoadHandedOff()
{
    // A download, or content for Konqueror: the page shown before stays,
    // and so does its URL (Konqueror opened a URL here without asking KIO
    // for its type first, see kwpepart.desktop).
    m_ownNavigation = false;
    m_url = KURL( m_view->url() );
    m_historyToken = m_view->currentHistoryToken();
    emit m_ext->setLocationBarURL( m_url.prettyURL() );
    emit canceled( QString::null );
}

void KWPEPart::slotLoadProgress( int percent )
{
    emit m_ext->loadingProgress( percent );
}

void KWPEPart::slotTitleChanged( const QString& title )
{
    emit setWindowCaption( title.isEmpty() ? m_url.prettyURL() : title );
    if ( m_app )
        emit pageTitleChanged( title );
}

void KWPEPart::slotLinkHovered( const QString& url )
{
    if ( m_mail ) {
        emit onURL( url );
        return;
    }
    emit setStatusBarText( url );
}

void KWPEPart::slotNewWindowRequested( const QString& url )
{
    emit m_ext->createNewWindow( KURL( url ) );
}

void KWPEPart::slotLinkActivated( const QString& url )
{
    // The host decides (KMail: its own URLs, attachments, mailto:, a
    // browser for the rest).
    emit m_ext->openURLRequest( KURL( url ) );
}

bool KWPEPart::mailAtBottom() const
{
    return m_view->mailAtBottom();
}

QString KWPEPart::mailSelectedText() const
{
    return m_view->mailSelectedText();
}

void KWPEPart::scrollBy( int dx, int dy )
{
    m_view->scrollBy( dx, dy );
}

void KWPEPart::selectAll()
{
    m_view->selectAll();
}

void KWPEPart::activateNotification( int id )
{
    m_view->activateNotification( id );
}

void KWPEPart::reload()
{
    m_view->reload();
}

void KWPEPart::slotUnsupportedContent( const QString& url, const QString& mimeType )
{
    // Konqueror finds a part or an application for it, or offers to save
    // it.  It must not come back here: a mime type that Konqueror thinks
    // is HTML but WebKit does not want.
    if ( url == m_lastUnsupported ) {
        m_lastUnsupported = QString::null;
        emit canceled( i18n( "Cannot show %1 (%2)" ).arg( url ).arg( mimeType ) );
        return;
    }
    m_lastUnsupported = url;
    // The view still shows the page before (or nothing): that is our URL
    // again.  Otherwise Konqueror takes the request for a link to the page
    // itself (a URL Konqueror had us open without asking KIO for its mime
    // type) and only tries to embed it, never offering to save it.
    m_url = KURL( m_view->url() );
    m_historyToken = m_view->currentHistoryToken();
    KParts::URLArgs args;
    args.serviceType = mimeType;
    // Konqueror may replace this part (another part embeds it): deleted.
    QGuardedPtr<KWPEPart> self( this );
    KURL shown = m_url;
    emit m_ext->openURLRequest( KURL( url ), args );
    // Saved, given to an application or cancelled: the location bar shows
    // the page on screen again.
    if ( self && m_url == shown )
        emit m_ext->setLocationBarURL( m_url.prettyURL() );
}

void KWPEPart::slotExternalURL( const QString& url )
{
    // Konqueror runs the mail program for mailto:, embeds KIO's view of
    // ftp:, smb:, ... (as for links in KHTML).  KDE's URL policy first
    // (KHTML's check), and no local schemes (system:, settings:, help:,
    // programs:, ...) from a page that is not local itself.
    KURL target( url );
    if ( !kapp->authorizeURLAction( "redirect", m_url, target )
         || ( KProtocolInfo::protocolClass( target.protocol() ) == ":local"
              && KProtocolInfo::protocolClass( m_url.protocol() ) != ":local" ) ) {
        emit setStatusBarText( i18n( "This page may not open %1" ).arg( target.prettyURL() ) );
        return;
    }
    KParts::URLArgs args;
    args.metaData()["referrer"] = m_url.url();
    emit m_ext->openURLRequest( target, args );
}

void KWPEPart::slotHistoryNavigationRequested( int steps )
{
    // Konqueror's goHistory() moves the active view: a script in a
    // background tab must not move the tab the user looks at.
    if ( manager() && manager()->activePart() != this )
        return;
    KParts::BrowserInterface* iface = m_ext->browserInterface();
    if ( iface )
        iface->callMethod( "goHistory(int)", QVariant( steps ) );
}

void KWPEPart::slotContextMenu( const QString& linkUrl, const QString& imageUrl, bool onSelection, const QPoint& globalPos )
{
    if ( m_mail ) {
        emit popupMenu( linkUrl, globalPos );
        return;
    }
    if ( m_app ) {
        showAppContextMenu( KURL( linkUrl ), KURL( imageUrl ), onSelection, globalPos );
        return;
    }
    KURL link( linkUrl );
    KURL popupURL = link.isEmpty() ? m_url : link;
    KParts::URLArgs args;
    QString mimeType = QString::fromLatin1( "text/html" );
    KParts::BrowserExtension::PopupFlags flags = KParts::BrowserExtension::ShowBookmark | KParts::BrowserExtension::ShowReload;
    if ( link.isEmpty() ) {
        if ( onSelection )
            flags = KParts::BrowserExtension::ShowTextSelectionItems;
        else
            flags |= KParts::BrowserExtension::ShowNavigationItems;
    } else if ( !link.hasRef() && link.query().isEmpty() && !link.fileName().isEmpty() ) {
        // Like KHTML: the type from the extension, unless it is a script
        // that probably makes HTML.
        KMimeType::Ptr type = KMimeType::findByPath( link.fileName(), 0, true );
        if ( type->name() != KMimeType::defaultMimeType() && !type->is( "application/x-php" )
             && !type->is( "application/x-perl" ) && !type->is( "application/x-python" )
             && !type->is( "application/x-shellscript" ) )
            mimeType = type->name();
    }
    args.serviceType = mimeType;
    args.metaData()["referrer"] = m_url.url();

    // Konqueror may destroy this part while the menu is open.
    KWPEPopupGUIClient* client = new KWPEPopupGUIClient( this, link, KURL( imageUrl ), onSelection );
    QGuardedPtr<QObject> guard( client );
    emit m_ext->popupMenu( client, globalPos, popupURL, args, flags, S_IFREG );
    if ( guard )
        delete client;
}

// App mode: no Konqueror to build the menu; the part's own.
void KWPEPart::showAppContextMenu( const KURL& link, const KURL& image, bool onSelection, const QPoint& globalPos )
{
    enum { OpenLink = 1, CopyLink, SaveLink, SaveImage, CopyImage, Cut, Copy, Paste, SelectAll };
    bool editable = m_view->contextMenuOnEditable();
    KPopupMenu menu( widget() );
    if ( !link.isEmpty() ) {
        if ( link.protocol() == "mailto" ) {
            menu.insertItem( SmallIconSet( "mail_new" ), i18n( "&Send Email..." ), OpenLink );
            menu.insertItem( i18n( "Copy Email Address" ), CopyLink );
        } else {
            menu.insertItem( SmallIconSet( "fileopen" ), i18n( "&Open Link in Browser" ), OpenLink );
            menu.insertItem( i18n( "Copy &Link Address" ), CopyLink );
            if ( link.protocol() == "http" || link.protocol() == "https" || link.protocol() == "blob" )
                menu.insertItem( SmallIconSet( "filesaveas" ), i18n( "&Save Link As..." ), SaveLink );
        }
    }
    if ( !image.isEmpty() ) {
        if ( menu.count() )
            menu.insertSeparator();
        menu.insertItem( SmallIconSet( "filesaveas" ), i18n( "Save Image As..." ), SaveImage );
        if ( image.protocol() == "http" || image.protocol() == "https" )
            menu.insertItem( i18n( "Copy Image Location" ), CopyImage );
    }
    if ( menu.count() )
        menu.insertSeparator();
    if ( editable )
        menu.insertItem( SmallIconSet( "editcut" ), i18n( "Cu&t" ), Cut );
    if ( editable || onSelection )
        menu.insertItem( SmallIconSet( "editcopy" ), i18n( "&Copy" ), Copy );
    if ( editable )
        menu.insertItem( SmallIconSet( "editpaste" ), i18n( "&Paste" ), Paste );
    menu.insertItem( i18n( "Select &All" ), SelectAll );

    // The host may delete the part while the menu is open.
    QGuardedPtr<KWPEPart> self( this );
    int id = menu.exec( globalPos );
    if ( !self )
        return;
    switch ( id ) {
    case OpenLink:
        emit m_ext->openURLRequest( link );
        break;
    case CopyLink: {
        QString text = link.protocol() == "mailto" ? link.path() : link.url();
        QApplication::clipboard()->setText( text, QClipboard::Clipboard );
        QApplication::clipboard()->setText( text, QClipboard::Selection );
        break;
    }
    case SaveLink:
        m_view->downloadURL( link.url() );
        break;
    case SaveImage:
        m_view->downloadURL( image.url() );
        break;
    case CopyImage:
        QApplication::clipboard()->setText( image.url(), QClipboard::Clipboard );
        QApplication::clipboard()->setText( image.url(), QClipboard::Selection );
        break;
    case Cut:
        m_view->cut();
        break;
    case Copy:
        m_view->copySelection();
        break;
    case Paste:
        m_view->paste();
        break;
    case SelectAll:
        m_view->selectAll();
        break;
    default:
        break;
    }
}

void KWPEPart::slotFind()
{
    if ( m_findDialog ) {
        m_findDialog->show();
        m_findDialog->raise();
        m_findDialog->setActiveWindow();
        return;
    }
    // Non-modal, like KHTML's.
    m_findDialog = new KFindDialog( false, widget(), "kwpefind", m_findOptions, m_findHistory );
    m_findDialog->setHasSelection( false );
    m_findDialog->setHasCursor( false );
    m_findDialog->setSupportsRegularExpressionFind( false );
    if ( !m_findText.isEmpty() )
        m_findDialog->setPattern( m_findText );
    connect( m_findDialog, SIGNAL( okClicked() ), SLOT( slotFindDialogOk() ) );
    connect( m_findDialog, SIGNAL( finished() ), m_findDialog, SLOT( delayedDestruct() ) );
    m_findDialog->show();
}

void KWPEPart::slotFindDialogOk()
{
    if ( !m_findDialog )
        return;
    m_findText = m_findDialog->pattern();
    m_findOptions = m_findDialog->options() & ~KFindDialog::RegularExpression;
    m_findHistory = m_findDialog->findHistory();
    m_findDialog->hide();
    m_findNext->setEnabled( !m_findText.isEmpty() );
    m_findPrevious->setEnabled( !m_findText.isEmpty() );
    if ( !m_findText.isEmpty() )
        m_view->find( m_findText, m_findOptions & KFindDialog::CaseSensitive, m_findOptions & KFindDialog::WholeWordsOnly,
                      m_findOptions & KFindDialog::FindBackwards );
}

void KWPEPart::slotFindNext()
{
    if ( m_findText.isEmpty() )
        slotFind();
    else
        m_view->findNext( !( m_findOptions & KFindDialog::FindBackwards ) );
}

void KWPEPart::slotFindPrevious()
{
    if ( m_findText.isEmpty() )
        slotFind();
    else
        m_view->findNext( m_findOptions & KFindDialog::FindBackwards );
}

void KWPEPart::slotFindResult( bool found, unsigned matches )
{
    if ( found ) {
        emit setStatusBarText( matches >= 1000 ? i18n( "More than 1000 matches" )
                                               : i18n( "1 match", "%n matches", matches ) );
    } else {
        KNotifyClient::beep();
        emit setStatusBarText( i18n( "No matches found for '%1'." ).arg( m_findText ) );
    }
}

void KWPEPart::updatePageSecurity()
{
    int security = NotCrypted;
    if ( m_url.protocol() == "https" && m_view->isSecure() )
        security = m_view->hasInsecureContent() ? Mixed : Encrypted;
    emit m_ext->setPageSecurity( security );
}

void KWPEPart::slotWebProcessFailed( bool crashed )
{
    QString text = crashed ? i18n( "The page crashed again. Reload to try once more." )
                           : i18n( "The page used too much memory and was stopped. Reload to show it again." );
    emit setStatusBarText( text );
    if ( m_app )
        emit statusMessage( text );
}

void KWPEPart::slotIconURL( const QString& url )
{
    // Konqueror hands it to KDE's favicon cache (kded's favicons module),
    // which downloads it for the location bar, the tab and bookmarks.
    KURL icon( url );
    if ( icon.protocol() == "http" || icon.protocol() == "https" )
        emit m_ext->setIconURL( icon );
}

void KWPEPart::slotViewSource()
{
    m_view->requestSource();
}

// The page's bytes as WebKit loaded them (no second request, POST answers
// too) in the text editor, from a temporary file KRun deletes afterwards.
void KWPEPart::slotSourceReady( const QString& url, const QByteArray& data )
{
    KURL source( url );
    QString name = source.fileName();
    if ( name.isEmpty() )
        name = source.host().isEmpty() ? QString::fromLatin1( "source" ) : source.host();
    name.replace( QRegExp( "[^A-Za-z0-9._-]" ), "_" );
    KTempFile file( locateLocal( "tmp", "kwpesource-" + name.left( 60 ) + "-" ), ".html", 0600 );
    if ( file.status() != 0 || file.file()->writeBlock( data ) != Q_LONG( data.size() ) || !file.close() ) {
        file.unlink();
        emit setStatusBarText( i18n( "Could not save the page source." ) );
        return;
    }
    KRun::runURL( KURL::fromPathOrURL( file.name() ), QString::fromLatin1( "text/plain" ), true );
}

void KWPEPart::slotZoomIn()
{
    m_view->zoomIn();
}

void KWPEPart::slotZoomOut()
{
    m_view->zoomOut();
}

void KWPEPart::slotZoomNormal()
{
    m_view->setZoom( 100 );
}

void KWPEPart::slotZoomChanged( int percent )
{
    m_zoomIn->setEnabled( m_view->canZoomIn() );
    m_zoomOut->setEnabled( m_view->canZoomOut() );
    m_zoomNormal->setEnabled( percent != 100 );
    emit setStatusBarText( i18n( "Zoom: %1%" ).arg( percent ) );
}

// ---------------------------------------------------------------------

static const char popupXML[] =
    "<!DOCTYPE kpartgui SYSTEM \"kpartgui.dtd\">"
    "<kpartgui name=\"kwpepart_popupmenu\" version=\"1\"><Menu name=\"popupmenu\">"
    "<Action name=\"copy\" group=\"editactions\"/>"
    "<Separator weakSeparator=\"1\" group=\"editactions\"/>"
    "<Action name=\"savelinkas\" group=\"linkactions\"/>"
    "<Action name=\"copylinklocation\" group=\"linkactions\"/>"
    "<Separator weakSeparator=\"1\"/>"
    "<Action name=\"saveimageas\"/>"
    "<Action name=\"copyimagelocation\"/>"
    "<Action name=\"viewimage\"/>"
    "</Menu></kpartgui>";

KWPEPopupGUIClient::KWPEPopupGUIClient( KWPEPart* part, const KURL& linkURL, const KURL& imageURL, bool onSelection )
    : QObject( part ), m_part( part ), m_linkURL( linkURL ), m_imageURL( imageURL )
{
    setInstance( part->instance() );

    if ( onSelection ) {
        KAction* copy = KStdAction::copy( part->m_ext, SLOT( copy() ), actionCollection(), "copy" );
        copy->setText( i18n( "&Copy Text" ) );
    }
    if ( !linkURL.isEmpty() ) {
        if ( linkURL.protocol() == "mailto" ) {
            new KAction( i18n( "Copy Email Address" ), 0, this, SLOT( slotCopyLinkLocation() ),
                         actionCollection(), "copylinklocation" );
        } else {
            new KAction( i18n( "&Save Link As..." ), 0, this, SLOT( slotSaveLinkAs() ),
                         actionCollection(), "savelinkas" );
            new KAction( i18n( "Copy &Link Address" ), 0, this, SLOT( slotCopyLinkLocation() ),
                         actionCollection(), "copylinklocation" );
        }
    }
    if ( !imageURL.isEmpty() ) {
        new KAction( i18n( "Save Image As..." ), 0, this, SLOT( slotSaveImageAs() ),
                     actionCollection(), "saveimageas" );
        new KAction( i18n( "Copy Image Location" ), 0, this, SLOT( slotCopyImageLocation() ),
                     actionCollection(), "copyimagelocation" );
        QString name = KStringHandler::csqueeze( imageURL.fileName() + imageURL.query(), 25 );
        new KAction( i18n( "View Image (%1)" ).arg( name.replace( "&", "&&" ) ), 0, this, SLOT( slotViewImage() ),
                     actionCollection(), "viewimage" );
    }

    setXML( QString::fromLatin1( popupXML ) );
}

void KWPEPopupGUIClient::slotSaveLinkAs()
{
    m_part->m_view->downloadURL( m_linkURL.url() );
}

static void copyURL( const KURL& url )
{
    // Both selections, like KHTML.  mailto: as the bare address.
    QString text = url.protocol() == "mailto" ? url.path() : url.url();
    QApplication::clipboard()->setText( text, QClipboard::Clipboard );
    QApplication::clipboard()->setText( text, QClipboard::Selection );
}

void KWPEPopupGUIClient::slotCopyLinkLocation()
{
    copyURL( m_linkURL );
}

void KWPEPopupGUIClient::slotSaveImageAs()
{
    m_part->m_view->downloadURL( m_imageURL.url() );
}

void KWPEPopupGUIClient::slotCopyImageLocation()
{
    copyURL( m_imageURL );
}

void KWPEPopupGUIClient::slotViewImage()
{
    emit m_part->m_ext->openURLRequest( m_imageURL );
}

// ---------------------------------------------------------------------

KWPEBrowserExtension::KWPEBrowserExtension( KWPEPart* part )
    : KParts::BrowserExtension( part, "KWPEBrowserExtension" ), m_part( part )
{
}

void KWPEBrowserExtension::saveState( QDataStream& stream )
{
    stream << QString::fromLatin1( stateMagic ) << m_part->m_url << Q_INT32( m_part->m_historyToken );
}

void KWPEBrowserExtension::copy()
{
    m_part->m_view->copySelection();
}

void KWPEBrowserExtension::restoreState( QDataStream& stream )
{
    QString magic;
    KURL url;
    Q_INT32 token = 0;
    stream >> magic;
    if ( magic != QString::fromLatin1( stateMagic ) )
        return;
    stream >> url >> token;
    m_part->restore( url, token );
}

#include "kwpepart.moc"
