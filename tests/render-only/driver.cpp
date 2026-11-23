/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// kwpe-render-only [--phases r,e,..] [--reader2 r,e,..] [--after MS] [--wait MS] [--shot FILE] MESSAGE.html
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
#include <qregexp.h>
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

Reader::Reader( char tag, KParts::ReadOnlyPart* part, const QStringList& modes, const QStringList& dirs, int afterMs )
    : QObject( 0, "reader" ), m_tag( tag ), m_part( part ), m_modes( modes ), m_dirs( dirs ), m_afterMs( afterMs ),
      m_phase( 0 )
{
    connect( part, SIGNAL( completed() ), SLOT( slotCompleted() ) );
    KParts::BrowserExtension* ext = KParts::BrowserExtension::childObject( part );
    if ( ext )
        connect( ext, SIGNAL( openURLRequest( const KURL&, const KParts::URLArgs& ) ),
                 SLOT( slotLink( const KURL&, const KParts::URLArgs& ) ) );
    slotNextPhase();
}

void Reader::slotNextPhase()
{
    if ( m_phase >= m_dirs.count() )
        return;
    bool external = m_modes[m_phase] == "e";
    printf( "%c PHASE %u %s\n", m_tag, m_phase + 1, external ? "e" : "r" );
    fflush( stdout );
    m_part->setProperty( "mailLoadExternal", QVariant( external, 0 ) );
    m_part->openURL( KURL::fromPathOrURL( m_dirs[m_phase] + "/index.html" ) );
    if ( ++m_phase < m_dirs.count() )
        QTimer::singleShot( m_afterMs, this, SLOT( slotNextPhase() ) );
}

void Reader::slotCompleted()
{
    QPoint origin = m_part->widget()->mapToGlobal( QPoint( 0, 0 ) );
    printf( "%c WIDGET %d %d\n%c COMPLETED\n", m_tag, origin.x(), origin.y(), m_tag );
    fflush( stdout );
}

void Reader::slotLink( const KURL& url, const KParts::URLArgs& )
{
    printf( "%c LINK %s\n", m_tag, url.url().latin1() );
    fflush( stdout );
}

Finisher::Finisher( KParts::ReadOnlyPart* part, int waitMs, const QString& shot )
    : QObject( 0, "finisher" ), m_part( part ), m_shot( shot )
{
    QTimer::singleShot( waitMs, this, SLOT( slotFinish() ) );
}

void Finisher::slotFinish()
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

// The message's directory, as KMail's writer makes it (0700): KMail's
// document (headers, then the HTML part in a frame) and the HTML part.
static QString writeMessage( const QString& file, const QString& tag )
{
    QFile message( file );
    if ( !message.open( IO_ReadOnly ) ) {
        fprintf( stderr, "cannot read %s\n", QFile::encodeName( file ).data() );
        return QString::null;
    }
    QByteArray body = message.readAll();
    if ( !tag.isEmpty() ) {
        // host names of the reader and phase: a web process asks for a
        // name only once, and the logs tell the showings apart
        QCString text( body.data(), body.size() + 1 );
        text.replace( QRegExp( "\\.blocked\\.test" ), QCString( "." ) + tag.latin1() + ".blocked.test" );
        text.replace( QRegExp( "\\.allowed\\.test" ), QCString( "." ) + tag.latin1() + ".allowed.test" );
        body.duplicate( text.data(), text.length() );
    }
    QCString dir = QFile::encodeName( QString( getenv( "TMPDIR" ) ? getenv( "TMPDIR" ) : "/tmp" ) + "/kwpe-ro-XXXXXX" );
    if ( !mkdtemp( dir.data() ) ) {
        fprintf( stderr, "mkdtemp: %s\n", strerror( errno ) );
        return QString::null;
    }
    QString d = QFile::decodeName( dir );
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
        return QString::null;
    }
    return d;
}

static const KCmdLineOptions options[] = {
    { "phases <list>", "The first reader's showings, r (render only) or e (external references), e.g. r,e,r", "r" },
    { "reader2 <list>", "A second reader at the same time, its showings", 0 },
    { "after <ms>", "Time between showings", "2500" },
    { "wait <ms>", "Time before the screenshot and the end", "5000" },
    { "shot <file>", "Save the first reader's screenshot (PNG, or PPM if named .ppm)", 0 },
    { "+message", "The message's HTML", 0 },
    KCmdLineLastOption
};

static Reader* makeReader( char tag, const QString& message, const QStringList& modes, int after, QWidget* top,
                           const QCString& library )
{
    QStringList dirs;
    for ( unsigned i = 0; i < modes.count(); ++i ) {
        QString d = writeMessage( message, QString( "%1%2" ).arg( QChar( tag ) ).arg( i + 1 ) );
        if ( d.isEmpty() )
            return 0;
        dirs.append( d );
    }
    int error = 0;
    KParts::ReadOnlyPart* part = KParts::ComponentFactory::createPartInstanceFromLibrary<KParts::ReadOnlyPart>(
        library.data(), top, "view", top, "part", QStringList( "mode=mail" ), &error );
    if ( !part || !part->property( "mailLoadExternal" ).isValid() ) {
        fprintf( stderr, "no part with a mail mode in %s (error %d)\n", library.data(), error );
        return 0;
    }
    part->widget()->resize( top->size() );
    top->show();
    return new Reader( tag, part, modes, dirs, after );
}

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
    QString message = QFile::decodeName( args->arg( 0 ) );
    int after = QString( args->getOption( "after" ) ).toInt();
    QCString library = getenv( "KWPE_PART" ) ? getenv( "KWPE_PART" ) : "libkwpepart";

    QWidget top;
    top.resize( 800, 600 );
    Reader* a = makeReader( 'a', message, QStringList::split( ',', args->getOption( "phases" ) ), after, &top, library );
    if ( !a )
        return 2;
    QWidget top2;
    Reader* b = 0;
    if ( args->isSet( "reader2" ) ) {
        top2.setGeometry( 0, 620, 800, 140 );
        b = makeReader( 'b', message, QStringList::split( ',', args->getOption( "reader2" ) ), after, &top2, library );
        if ( !b )
            return 2;
    }
    Finisher finisher( a->part(), QString( args->getOption( "wait" ) ).toInt(),
                       args->isSet( "shot" ) ? QFile::decodeName( args->getOption( "shot" ) ) : QString::null );
    int rc = app.exec();
    delete a->part();
    if ( b )
        delete b->part();
    // (the message directories stay in $TMPDIR: the runner's)
    return rc;
}

#include "driver.moc"
