/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// kwpeview URL: the WPE WebKit part in a window of its own, loaded the
// way Konqueror loads it (KLibLoader, KParts factory), for debugging.
// KWPE_DEBUG=1 and KWPE_STATS=1 print what the part does.

#include <kaboutdata.h>
#include <kapplication.h>
#include <kcmdlineargs.h>
#include <klibloader.h>
#include <klocale.h>
#include <kmainwindow.h>
#include <kparts/factory.h>
#include <kparts/part.h>
#include <kstatusbar.h>

#include <stdio.h>

static KCmdLineOptions options[] =
{
    { "+[URL]", I18N_NOOP( "Page to open" ), 0 },
    KCmdLineLastOption
};

int main( int argc, char** argv )
{
    KAboutData about( "kwpeview", I18N_NOOP( "KWPEView" ), "0.1",
                      I18N_NOOP( "Shows a web page with the WPE WebKit part" ),
                      KAboutData::License_LGPL, "(c) 2026 The kde3-revival authors" );
    KCmdLineArgs::init( argc, argv, &about );
    KCmdLineArgs::addCmdLineOptions( options );
    KApplication app;
    KCmdLineArgs* args = KCmdLineArgs::parsedArgs();

    KLibFactory* factory = KLibLoader::self()->factory( "libkwpepart" );
    if ( !factory ) {
        fprintf( stderr, "kwpeview: cannot load libkwpepart: %s\n",
                 KLibLoader::self()->lastErrorMessage().local8Bit().data() );
        return 1;
    }

    KMainWindow* window = new KMainWindow();
    KParts::ReadOnlyPart* part = static_cast<KParts::ReadOnlyPart*>(
        static_cast<KParts::Factory*>( factory )->createPart( window, "webview", window, "kwpepart",
                                                               "KParts::ReadOnlyPart" ) );
    if ( !part ) {
        fprintf( stderr, "kwpeview: the factory made no part\n" );
        return 1;
    }
    window->setCentralWidget( part->widget() );
    QObject::connect( part, SIGNAL( setWindowCaption( const QString& ) ), window, SLOT( setCaption( const QString& ) ) );
    QObject::connect( part, SIGNAL( setStatusBarText( const QString& ) ), window->statusBar(), SLOT( message( const QString& ) ) );
    window->statusBar()->message( " " );
    window->setGeometry( 0, 0, 1600, 1000 );
    window->show();
    part->widget()->setFocus();

    app.setMainWidget( window );
    if ( args->count() > 0 )
        part->openURL( args->url( 0 ) );
    args->clear();
    return app.exec();
}
