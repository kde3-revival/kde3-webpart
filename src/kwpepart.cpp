/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include <qdatastream.h>
#include <qvariant.h>

#include <kaboutdata.h>
#include <kdebug.h>
#include <kdemacros.h>
#include <klocale.h>
#include <kparts/browserinterface.h>
#include <kparts/event.h>
#include <kparts/genericfactory.h>

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
    : KParts::ReadOnlyPart( parent, name ), m_ownNavigation( false ), m_historyToken( 0 )
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
}

KWPEPart::~KWPEPart()
{
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
    KParts::URLArgs args = m_ext->urlArgs();
    m_url = url;
    m_historyToken = 0;
    m_ownNavigation = true;
    emit setWindowCaption( url.prettyURL() );

    // Konqueror's Reload (F5): revalidate with the server, like KHTML.
    if ( args.reload && m_view->url() == url.url() )
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
    if ( token && token == m_view->currentHistoryToken() && m_view->url() == url.url() ) {
        // Already there (the view was not switched away).
        m_url = url;
        m_historyToken = token;
        emit m_ext->setLocationBarURL( url.prettyURL() );
        emit started( 0 );
        emit completed();
        return;
    }
    if ( token && m_view->goToHistoryToken( token ) ) {
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
    KParts::BrowserInterface* iface = m_ext->browserInterface();
    if ( iface )
        iface->callMethod( "goHistory(int)", QVariant( steps ) );
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
