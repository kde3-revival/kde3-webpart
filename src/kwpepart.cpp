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
#include <knotifyclient.h>
#include <klocale.h>
#include <kmimetype.h>
#include <kstdaction.h>
#include <kstringhandler.h>
#include <kparts/browserinterface.h>
#include <kparts/event.h>
#include <kparts/genericfactory.h>
#include <kparts/partmanager.h>

#include "kwpepart.h"
#include "kwpewidget.h"

typedef KParts::GenericFactory<KWPEPart> KWPEPartFactory;
K_EXPORT_COMPONENT_FACTORY( libkwpepart, KWPEPartFactory )

// KLibLoader must never unload this module: it registers GObject types
// (GLib cannot unregister them) and an X event filter in Qt.
extern "C" {
KDE_EXPORT int __kde_do_not_unload = 1;
}

// Konqueror's KonqMainWindow::PageSecurity
enum { NotCrypted = 0, Encrypted = 1 };

static const char stateMagic[] = "kwpepart-1";

KWPEPart::KWPEPart( QWidget* parentWidget, const char* widgetName,
                    QObject* parent, const char* name, const QStringList& )
    : KParts::ReadOnlyPart( parent, name ), m_ownNavigation( false ), m_historyToken( 0 ), m_findOptions( 0 )
{
    setInstance( KWPEPartFactory::instance() );

    m_view = new KWPEWidget( parentWidget, widgetName );
    setWidget( m_view );
    if ( !m_view->isValid() )
        kdWarning() << "kwpepart: WPE WebKit could not be set up" << endl;

    m_ext = new KWPEBrowserExtension( this );

    connect( m_view, SIGNAL( loadStarted() ), SLOT( slotLoadStarted() ) );
    connect( m_view, SIGNAL( urlChanged( const QString&, bool ) ), SLOT( slotUrlChanged( const QString&, bool ) ) );
    connect( m_view, SIGNAL( loadFinished() ), SLOT( slotLoadFinished() ) );
    connect( m_view, SIGNAL( loadFailed( const QString& ) ), SLOT( slotLoadFailed( const QString& ) ) );
    connect( m_view, SIGNAL( loadProgress( int ) ), SLOT( slotLoadProgress( int ) ) );
    connect( m_view, SIGNAL( titleChanged( const QString& ) ), SLOT( slotTitleChanged( const QString& ) ) );
    connect( m_view, SIGNAL( linkHovered( const QString& ) ), SLOT( slotLinkHovered( const QString& ) ) );
    connect( m_view, SIGNAL( newWindowRequested( const QString& ) ), SLOT( slotNewWindowRequested( const QString& ) ) );
    connect( m_view, SIGNAL( unsupportedContent( const QString&, const QString& ) ),
             SLOT( slotUnsupportedContent( const QString&, const QString& ) ) );
    connect( m_view, SIGNAL( historyNavigationRequested( int ) ), SLOT( slotHistoryNavigationRequested( int ) ) );
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
    connect( m_view, SIGNAL( webProcessFailed( bool ) ), SLOT( slotWebProcessFailed( bool ) ) );

    setXMLFile( "kwpepart.rc" );
}

KWPEPart::~KWPEPart()
{
    delete m_findDialog;
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
    if ( event->activated() )
        emit setWindowCaption( m_view->title().isEmpty() ? m_url.prettyURL() : m_view->title() );
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
    emit m_ext->setPageSecurity( u.protocol() == "https" && m_view->isSecure() ? Encrypted : NotCrypted );

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

void KWPEPart::slotLoadProgress( int percent )
{
    emit m_ext->loadingProgress( percent );
}

void KWPEPart::slotTitleChanged( const QString& title )
{
    emit setWindowCaption( title.isEmpty() ? m_url.prettyURL() : title );
}

void KWPEPart::slotLinkHovered( const QString& url )
{
    emit setStatusBarText( url );
}

void KWPEPart::slotNewWindowRequested( const QString& url )
{
    emit m_ext->createNewWindow( KURL( url ) );
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
    KParts::URLArgs args;
    args.serviceType = mimeType;
    emit m_ext->openURLRequest( KURL( url ), args );
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

void KWPEPart::slotWebProcessFailed( bool crashed )
{
    emit setStatusBarText( crashed ? i18n( "The page crashed again. Reload to try once more." )
                                   : i18n( "The page used too much memory and was stopped. Reload to show it again." ) );
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
