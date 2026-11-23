/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// kwpe-render-only [--external] [--wait MS] [--shot FILE.png] MESSAGE.html
//
// The part (KWPE_PART: a library name or path, default libkwpepart) in
// mail mode shows MESSAGE.html as a message's HTML part: a host document
// (as KMail writes it) with a frame of body-0.html, in a private
// directory.  See README.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <qfile.h>
#include <qimage.h>
#include <qpixmap.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwidget.h>

#include <kaboutdata.h>
#include <kapplication.h>
#include <kcmdlineargs.h>
#include <kparts/browserextension.h>
#include <kparts/componentfactory.h>
#include <kurl.h>

#include "driver.h"

Driver::Driver( KParts::ReadOnlyPart* part, int waitMs, const QString& shot )
    : QObject( 0, "driver" ), m_part( part ), m_waitMs( waitMs ), m_shot( shot )
{
    connect( part, SIGNAL( completed() ), SLOT( slotCompleted() ) );
    KParts::BrowserExtension* ext = KParts::BrowserExtension::childObject( part );
    if ( ext )
        connect( ext, SIGNAL( openURLRequest( const KURL&, const KParts::URLArgs& ) ),
                 SLOT( slotLink( const KURL&, const KParts::URLArgs& ) ) );
    QTimer::singleShot( waitMs, this, SLOT( slotFinish() ) );
}

void Driver::slotCompleted()
{
    QPoint origin = m_part->widget()->mapToGlobal( QPoint( 0, 0 ) );
    printf( "WIDGET %d %d\n", origin.x(), origin.y() );
    printf( "COMPLETED\n" );
    fflush( stdout );
}

void Driver::slotLink( const KURL& url, const KParts::URLArgs& )
{
    printf( "LINK %s\n", url.url().latin1() );
    fflush( stdout );
}

void Driver::slotFinish()
{
    QWidget* w = m_part->widget();
    QImage image = QPixmap::grabWindow( w->winId() ).convertToImage();
    long red = 0;
    for ( int y = 0; y < image.height(); ++y )
        for ( int x = 0; x < image.width(); ++x ) {
            QRgb p = image.pixel( x, y );
            if ( qRed( p ) > 240 && qGreen( p ) < 16 && qBlue( p ) < 16 )
                ++red;
        }
    if ( !m_shot.isEmpty() )
        image.save( m_shot, m_shot.endsWith( ".ppm" ) ? "PPM" : "PNG" );
    printf( "RENDERED %ld\n", red );
    fflush( stdout );
    kapp->quit();
}

static bool writeFile( const QString& path, const QByteArray& data )
{
    QFile f( path );
    return f.open( IO_WriteOnly ) && f.writeBlock( data ) == (int)data.size();
}

static const KCmdLineOptions options[] = {
    { "external", "Load external references (images, style sheets, fonts)", 0 },
    { "wait <ms>", "Time the message gets before the screenshot", "5000" },
    { "shot <file>", "Save the screenshot (PNG, or PPM if named .ppm)", 0 },
    { "+message", "The message's HTML", 0 },
    KCmdLineLastOption
};

int main( int argc, char** argv )
{
    KAboutData about( "kwpe-render-only", "kwpe-render-only", "1" );
    KCmdLineArgs::init( argc, argv, &about );
    KCmdLineArgs::addCmdLineOptions( options );
    KApplication::disableAutoDcopRegistration();
    KApplication app;
    KCmdLineArgs* args = KCmdLineArgs::parsedArgs();
    if ( args->count() != 1 )
        KCmdLineArgs::usage();

    QFile message( QFile::decodeName( args->arg( 0 ) ) );
    if ( !message.open( IO_ReadOnly ) ) {
        fprintf( stderr, "cannot read %s\n", args->arg( 0 ) );
        return 2;
    }
    QByteArray body = message.readAll();

    // The message's directory, as KMail's writer makes it (0700).
    QCString dir = QFile::encodeName( QString( getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" ) + "/kwpe-ro-XXXXXX" );
    if ( !mkdtemp( dir.data() ) ) {
        fprintf( stderr, "mkdtemp: %s\n", strerror( errno ) );
        return 2;
    }
    QString d = QFile::decodeName( dir );
    // KMail's document: headers, then the HTML part in a frame.
    QCString host =
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
        "<html><head><style>body { margin: 0; font-family: sans-serif; }</style></head><body>"
        "<div style=\"background: #ddd; padding: 4px\">From: test &lt;test@example.invalid&gt;</div>"
        "<iframe class=\"kmail-htmlpart\" src=\"body-0.html\" sandbox=\"allow-same-origin allow-popups "
        "allow-popups-to-escape-sandbox allow-top-navigation-by-user-activation\" "
        "style=\"display: block; width: 100%; height: 1px; border: 0; margin: 0;\"></iframe>"
        "</body></html>\n";
    QByteArray hostData;
    hostData.duplicate( host.data(), host.length() );
    if ( !writeFile( d + "/index.html", hostData ) || !writeFile( d + "/body-0.html", body ) ) {
        fprintf( stderr, "cannot write %s\n", dir.data() );
        return 2;
    }

    QWidget top;
    top.resize( 800, 600 );
    QCString library = getenv( "KWPE_PART" ) ? getenv( "KWPE_PART" ) : "libkwpepart";
    int error = 0;
    KParts::ReadOnlyPart* part = KParts::ComponentFactory::createPartInstanceFromLibrary<KParts::ReadOnlyPart>(
        library.data(), &top, "view", &top, "part", QStringList( "mode=mail" ), &error );
    if ( !part || !part->property( "mailLoadExternal" ).isValid() ) {
        fprintf( stderr, "no part with a mail mode in %s (error %d)\n", library.data(), error );
        return 2;
    }
    part->widget()->resize( 800, 600 );
    top.show();
    part->setProperty( "mailLoadExternal", QVariant( args->isSet( "external" ), 0 ) );

    Driver driver( part, QString( args->getOption( "wait" ) ).toInt(),
                   args->isSet( "shot" ) ? QFile::decodeName( args->getOption( "shot" ) ) : QString::null );
    part->openURL( KURL::fromPathOrURL( d + "/index.html" ) );
    int rc = app.exec();

    delete part;
    unlink( QFile::encodeName( d + "/index.html" ).data() );
    unlink( QFile::encodeName( d + "/body-0.html" ).data() );
    rmdir( dir.data() );
    return rc;
}

#include "driver.moc"
