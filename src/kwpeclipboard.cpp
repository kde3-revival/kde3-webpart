/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WPE (GLib) before Qt: Qt 3 defines "signals", "slots" and "emit".
#include <wpe/wpe-platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qapplication.h>
#include <qbitmap.h>
#include <qbuffer.h>
#include <qclipboard.h>
#include <qimage.h>
#include <qpixmap.h>
#include <qtextcodec.h>
#include <qcstring.h>
#include <qmime.h>
#include <qstrlist.h>
#include <qvaluelist.h>

#include "kwpeclipboard.h"
#include "kwpexselection.h"
#include "wpex11platform.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

extern Time qt_x_time;

// ---------------------------------------------------------------------
// What WebKit copied, for the X CLIPBOARD

namespace
{

class WebKitContent;
WebKitContent* s_forwarded = 0;   // what we put into the X CLIPBOARD (Qt owns it)

class WebKitContent : public QMimeSource
{
public:
    // Qt deletes it when the X CLIPBOARD changes hands: no stale pointer
    // that a new object at the same address could match.
    virtual ~WebKitContent()
    {
        if ( s_forwarded == this )
            s_forwarded = 0;
    }

    void add( const char* format, const QByteArray& data )
    {
        m_formats.append( QCString( format ) );
        m_data.append( data );
    }

    virtual const char* format( int i = 0 ) const
    {
        return i >= 0 && i < int( m_formats.count() ) ? m_formats[i].data() : 0;
    }

    virtual QByteArray encodedData( const char* format ) const
    {
        for ( unsigned i = 0; i < m_formats.count(); ++i )
            if ( qstricmp( m_formats[i], format ) == 0 )
                return m_data[i];
        return QByteArray();
    }

private:
    QValueList<QCString> m_formats;
    QValueList<QByteArray> m_data;
};

bool debugging()
{
    static int debug = -1;
    if ( debug < 0 ) {
        const char* value = getenv( "KWPE_DEBUG" );
        debug = value && *value && strcmp( value, "0" ) != 0;
    }
    return debug;
}

QByteArray toByteArray( const char* data, gsize size )
{
    QByteArray a;
    a.duplicate( data, size );
    return a;
}

}

// ---------------------------------------------------------------------
// KWPEClipboardX11

struct KWPEClipboardX11
{
    WPEClipboard parent;
};

struct KWPEClipboardX11Class
{
    WPEClipboardClass parent_class;
};

G_DEFINE_TYPE( KWPEClipboardX11, kwpe_clipboard_x11, WPE_TYPE_CLIPBOARD )

static WPEClipboard* s_clipboard = 0;           // no reference: the display keeps it
static WPEClipboardContent* s_localContent = 0; // WebKit's last copy (ref): pasted again from s_forwarded
static QClipboard::Mode s_pasteMode = QClipboard::Clipboard;
static gint64 s_pasteUntil = 0;                 // reads allowed until then (a user paste)
static gint64 s_pasteStart = 0;
static Window s_pasteOwner = None;              // the selection's owner at the key
static Time s_pasteTime = CurrentTime;          // the key's (click's) time: every request of this paste
static bool s_pasteExtended = false;            // the window was extended once (a slow read)
static guint s_cacheTimer = 0;
static bool s_updating = false;                 // our own changed() call
static bool s_pasteOwn = false;                 // that selection is ours (Qt has its data, no X)
static QValueList<QCString> s_offered;          // another program's targets for that paste
static QCString s_cachedFormat;                 // WebKit reads an image more than once per paste:
static GBytes* s_cachedBytes = 0;               // the last one read in this paste (ref)

static void dropCache()
{
    if ( s_cachedBytes )
        g_bytes_unref( s_cachedBytes );
    s_cachedBytes = 0;
    s_cachedFormat = QCString();
    if ( s_cacheTimer )
        g_source_remove( s_cacheTimer );
    s_cacheTimer = 0;
}

// Another program's data (a screenshot) is not kept after its paste.
static gboolean cacheExpired( gpointer )
{
    s_cacheTimer = 0;
    gint64 left = s_pasteUntil - g_get_monotonic_time();
    if ( left > 0 ) {
        s_cacheTimer = g_timeout_add( guint( left / 1000 ) + 50, cacheExpired, 0 );
        return G_SOURCE_REMOVE;
    }
    dropCache();
    return G_SOURCE_REMOVE;
}

// Another program's selection: its targets, and the data of a target, read
// by KWPEXSelection (INCR, bounded waits).  WebKit waits for every read, and
// so does Konqueror's window.
static const KWPEXSelection::Limits targetsLimits = { 2000, 2000, 64 * 1024 };
static const KWPEXSelection::Limits dataLimits = { 3000, 10000, 64 * 1024 * 1024 };

static Atom selectionAtom( QClipboard::Mode mode )
{
    return mode == QClipboard::Selection ? XA_PRIMARY : XInternAtom( qt_xdisplay(), "CLIPBOARD", False );
}

// While we wait, Qt keeps serving this program's own selections: requests
// for PRIMARY/CLIPBOARD (another view's copy) and the steps of its outgoing
// INCR transfers (the program that asked deletes the property it asked
// for: KWPEXSelection::isOutgoingStep(), fed by noteXEvent()).  SelectionClear and
// everything else wait for the event loop (Qt's dataChanged() slots would
// read the clipboard themselves inside WebKit's read).
struct ServiceAtoms
{
    Atom primary;
    Atom clipboard;
};

static bool serviceWants( const XEvent* e, void* data )
{
    const ServiceAtoms* atoms = static_cast<const ServiceAtoms*>( data );
    if ( e->type == SelectionRequest )
        return e->xselectionrequest.selection == atoms->primary || e->xselectionrequest.selection == atoms->clipboard;
    return KWPEXSelection::isOutgoingStep( e );   // Qt's INCR transfer to a program that asked us
}

static void serviceHandle( XEvent* event, void* )
{
    if ( qApp )
        qApp->x11ProcessEvent( event );
}

static const KWPEXSelection::Service* service()
{
    static ServiceAtoms atoms = { XA_PRIMARY, None };
    static KWPEXSelection::Service s = { serviceWants, serviceHandle, &atoms };
    if ( atoms.clipboard == None )
        atoms.clipboard = XInternAtom( qt_xdisplay(), "CLIPBOARD", False );
    return &s;
}

// Another program's names (atom names, formats) in a log line.
static QCString printable( const char* text )
{
    QCString out( text ? text : "" );
    if ( out.length() > 64 )
        out = out.left( 60 ) + "...";
    for ( uint i = 0; i < out.length(); ++i )
        if ( (unsigned char) out[i] < 0x20 || (unsigned char) out[i] >= 0x7f )
            out[i] = '?';
    return out;
}

static bool ownsSelection( QClipboard::Mode mode )
{
    QClipboard* cb = QApplication::clipboard();
    return mode == QClipboard::Selection ? cb->ownsSelection() : cb->ownsClipboard();
}

// (only the atom name requests run under this handler: BadAtom)
static int ignoreAtomErrors( Display*, XErrorEvent* )
{
    return 0;
}

// The other program's targets; false if it does not answer.
static bool readTargets( QClipboard::Mode mode, QValueList<QCString>& names )
{
    Display* dpy = qt_xdisplay();
    if ( XGetSelectionOwner( dpy, selectionAtom( mode ) ) == None )
        return false;
    std::string data;
    int format = 0;
    gint64 start = g_get_monotonic_time();
    KWPEXSelection::Result result = KWPEXSelection::read( dpy, selectionAtom( mode ), XInternAtom( dpy, "TARGETS", False ),
                                                          s_pasteTime, targetsLimits, data, 0, &format, service() );
    if ( debugging() )
        fprintf( stderr, "kwpe: clipboard targets: %s, %u byte(s) in %d ms\n", KWPEXSelection::resultName( result ),
                 unsigned( data.size() ), int( ( g_get_monotonic_time() - start ) / 1000 ) );
    if ( result != KWPEXSelection::Ok || format != 32 )
        return result == KWPEXSelection::Refused;   // no TARGETS (an old program): text is tried
    int count = int( data.size() / 4 );
    if ( count <= 0 )
        return true;
    Atom* atoms = new Atom[count];
    for ( int i = 0; i < count; ++i )
        atoms[i] = Atom( reinterpret_cast<const uint32_t*>( data.data() )[i] );
    // A bogus atom in the list (BadAtom) must not cost the others.
    XSync( dpy, False );
    XErrorHandler previous = XSetErrorHandler( ignoreAtomErrors );
    char** atomNames = new char*[count];
    for ( int i = 0; i < count; ++i )
        atomNames[i] = 0;
    if ( !XGetAtomNames( dpy, atoms, count, atomNames ) ) {
        for ( int i = 0; i < count; ++i ) {
            if ( atomNames[i] )
                XFree( atomNames[i] );
            atomNames[i] = atoms[i] != None ? XGetAtomName( dpy, atoms[i] ) : 0;
        }
    }
    XSync( dpy, False );
    XSetErrorHandler( previous );
    for ( int i = 0; i < count; ++i ) {
        if ( atomNames[i] ) {
            names.append( QCString( atomNames[i] ) );
            XFree( atomNames[i] );
        }
    }
    delete[] atomNames;
    delete[] atoms;
    return true;
}

static const char* offeredName( const char* format )
{
    for ( QValueList<QCString>::ConstIterator it = s_offered.begin(); it != s_offered.end(); ++it )
        if ( qstricmp( *it, format ) == 0 )
            return ( *it ).data();
    return 0;
}


static bool offered( const char* format )
{
    return offeredName( format ) != 0;
}

// One read of another program's selection in @p target (bounded).
static bool readForeign( const char* target, std::string& bytes, Atom* type = 0 )
{
    Display* dpy = qt_xdisplay();
    gint64 start = g_get_monotonic_time();
    KWPEXSelection::Result result = KWPEXSelection::read( dpy, selectionAtom( s_pasteMode ), XInternAtom( dpy, target, False ),
                                                          s_pasteTime, dataLimits, bytes, type, 0, service() );
    if ( debugging() )
        fprintf( stderr, "kwpe: clipboard %s: %s, %u byte(s) in %d ms\n", printable( target ).data(), KWPEXSelection::resultName( result ),
                 unsigned( bytes.size() ), int( ( g_get_monotonic_time() - start ) / 1000 ) );
    return result == KWPEXSelection::Ok;
}

static void stripNuls( std::string& s )
{
    while ( !s.empty() && s[s.size() - 1] == '\0' )
        s.erase( s.size() - 1 );
}

// Bytes in a known encoding -> UTF-8 (invalid sequences replaced).
static std::string toUtf8( const std::string& in, const char* encoding )
{
    std::string out;
    gsize written = 0;
    gchar* converted = g_convert_with_fallback( in.data(), gssize( in.size() ), "UTF-8", encoding, "?", 0, &written, 0 );
    if ( converted ) {
        out.assign( converted, written );
        g_free( converted );
    }
    return out;
}

static std::string validUtf8( const std::string& in )
{
    gchar* valid = g_utf8_make_valid( in.data(), gssize( in.size() ) );
    std::string out( valid );
    g_free( valid );
    return out;
}

// The charset of a MIME text target ("text/plain;charset=X"), or null.
static QCString charsetOf( const char* target )
{
    const char* p = strchr( target, ';' );
    if ( !p )
        return QCString();
    QCString params = QCString( p + 1 ).stripWhiteSpace();
    if ( qstrnicmp( params, "charset=", 8 ) != 0 )
        return QCString();
    QCString charset = params.mid( 8 ).stripWhiteSpace();
    int end = charset.find( ';' );
    if ( end >= 0 )
        charset.truncate( end );
    if ( charset.length() > 1 && charset[0] == '"' && charset[int( charset.length() ) - 1] == '"' )
        charset = charset.mid( 1, charset.length() - 2 );
    return charset;
}

static bool isUtf8Name( const QCString& charset )
{
    return qstricmp( charset, "utf-8" ) == 0 || qstricmp( charset, "utf8" ) == 0;
}

// Bytes in a named charset -> UTF-8: Qt's codecs, else iconv's; false if
// neither knows it.
static bool decodeCharset( const QCString& charset, const std::string& in, std::string& utf8 )
{
    if ( isUtf8Name( charset ) ) {
        utf8 = validUtf8( in );
        return true;
    }
    if ( QTextCodec* codec = QTextCodec::codecForName( charset ) ) {
        QCString out = codec->toUnicode( in.data(), int( in.size() ) ).utf8();
        utf8.assign( out.data(), out.length() );
        return true;
    }
    GIConv cd = g_iconv_open( "UTF-8", charset.data() );
    if ( cd == (GIConv) -1 )
        return false;
    g_iconv_close( cd );
    utf8 = toUtf8( in, charset.data() );
    return true;
}

static bool knownCharset( const QCString& charset )
{
    std::string ignored;
    return decodeCharset( charset, std::string(), ignored );
}

// A MIME text target (with or without a charset) we can decode.
static bool readableMime( const char* target, const char* type )
{
    size_t n = strlen( type );
    if ( qstrnicmp( target, type, uint( n ) ) != 0 )
        return false;
    if ( !target[n] )
        return true;
    if ( target[n] != ';' )
        return false;
    QCString charset = charsetOf( target );
    return !charset.isEmpty() && knownCharset( charset );
}

// Plain text of another program, as UTF-8: UTF8_STRING, a UTF-8 MIME type,
// a MIME type in another charset we know, COMPOUND_TEXT (Xlib), STRING
// (Latin-1), TEXT, plain text/plain.
static bool readForeignText( std::string& utf8 )
{
    Display* dpy = qt_xdisplay();
    QCString name;
    for ( int pass = 0; pass < 7 && name.isEmpty(); ++pass ) {
        static const char* const fixed[] = { "UTF8_STRING", 0, 0, "COMPOUND_TEXT", "STRING", "TEXT", "text/plain" };
        if ( fixed[pass] ) {
            if ( const char* offeredTarget = offeredName( fixed[pass] ) )
                name = offeredTarget;
            continue;
        }
        // pass 1: text/plain;charset=utf-8, pass 2: any other known charset
        for ( QValueList<QCString>::ConstIterator it = s_offered.begin(); it != s_offered.end(); ++it ) {
            QCString charset = charsetOf( *it );
            if ( charset.isEmpty() || !readableMime( *it, "text/plain" ) )
                continue;
            if ( pass == 1 ? isUtf8Name( charset ) : !isUtf8Name( charset ) ) {
                name = *it;
                break;
            }
        }
    }
    if ( name.isEmpty() )
        return false;
    std::string bytes;
    Atom type = None;
    if ( !readForeign( name, bytes, &type ) )
        return false;
    stripNuls( bytes );
    QCString charset = charsetOf( name );
    if ( !charset.isEmpty() ) {
        if ( !decodeCharset( charset, bytes, utf8 ) )
            return false;
    } else if ( type == XInternAtom( dpy, "COMPOUND_TEXT", False ) ) {
        XTextProperty property;
        property.value = reinterpret_cast<unsigned char*>( const_cast<char*>( bytes.data() ) );
        property.encoding = type;
        property.format = 8;
        property.nitems = bytes.size();
        char** list = 0;
        int count = 0;
        utf8.clear();
        if ( Xutf8TextPropertyToTextList( dpy, &property, &list, &count ) >= 0 && list ) {
            for ( int i = 0; i < count; ++i )
                utf8 += list[i];
            XFreeStringList( list );
        }
    } else if ( type == XA_STRING ) {
        utf8 = toUtf8( bytes, "ISO-8859-1" );
    } else if ( g_utf8_validate( bytes.data(), gssize( bytes.size() ), 0 ) ) {
        utf8 = bytes;
    } else if ( qstricmp( name, "text/plain" ) == 0 ) {
        utf8 = toUtf8( bytes, "ISO-8859-1" );   // an old MIME owner: Latin-1
    } else {
        utf8 = validUtf8( bytes );
    }
    return true;
}

// HTML of another program as UTF-8.  Plain text/html: Mozilla and Chrome
// send UTF-16 (with or without a byte order mark), most others UTF-8;
// text/html;charset=X: in X (a byte order mark still wins).
static bool readForeignHtml( std::string& utf8 )
{
    QCString name;
    if ( const char* plain = offeredName( "text/html" ) )
        name = plain;
    for ( QValueList<QCString>::ConstIterator it = s_offered.begin(); name.isEmpty() && it != s_offered.end(); ++it )
        if ( !charsetOf( *it ).isEmpty() && readableMime( *it, "text/html" ) )
            name = *it;
    std::string bytes;
    if ( name.isEmpty() || !readForeign( name, bytes ) )
        return false;
    const unsigned char* b = reinterpret_cast<const unsigned char*>( bytes.data() );
    QCString charset = charsetOf( name );
    if ( bytes.size() >= 2 && b[0] == 0xff && b[1] == 0xfe )
        utf8 = toUtf8( bytes.substr( 2 ), "UTF-16LE" );
    else if ( bytes.size() >= 2 && b[0] == 0xfe && b[1] == 0xff )
        utf8 = toUtf8( bytes.substr( 2 ), "UTF-16BE" );
    else if ( !charset.isEmpty() ) {
        if ( !decodeCharset( charset, bytes, utf8 ) )
            return false;
    } else if ( bytes.size() >= 2 && bytes.size() % 2 == 0 && b[0] && !b[1] )
        utf8 = toUtf8( bytes, "UTF-16LE" );
    else if ( g_utf8_validate( bytes.data(), gssize( bytes.size() ), 0 ) )
        utf8 = bytes;
    else
        utf8 = toUtf8( bytes, "ISO-8859-1" );
    stripNuls( utf8 );
    return true;
}

// An old program that offers only a PIXMAP (a server-side drawable) as its
// image: copied and encoded as PNG here, as Qt 3 did (as PPM), within size
// limits; a drawable that is gone or does not fit gives nothing.
static const unsigned maxPixmapSide = 16384;
static const unsigned long maxPixmapPixels = 32UL * 1024 * 1024;
static bool s_drawableError = false;

static int drawableErrors( Display*, XErrorEvent* )
{
    s_drawableError = true;
    return 0;
}

static bool readForeignPixmap( std::string& png )
{
    const char* name = offeredName( "PIXMAP" );
    std::string bytes;
    Atom type = None;
    if ( !name || !readForeign( name, bytes, &type ) || bytes.size() < 4 )
        return false;
    uint32_t id;
    memcpy( &id, bytes.data(), sizeof( id ) );
    Display* dpy = qt_xdisplay();
    XSync( dpy, False );
    s_drawableError = false;
    XErrorHandler previous = XSetErrorHandler( drawableErrors );
    Window root;
    int x, y;
    unsigned w = 0, h = 0, border, depth = 0;
    bool ok = id && XGetGeometry( dpy, Pixmap( id ), &root, &x, &y, &w, &h, &border, &depth );
    XSync( dpy, False );
    ok = ok && !s_drawableError && w && h && w <= maxPixmapSide && h <= maxPixmapSide
         && (unsigned long) w * h <= maxPixmapPixels && ( depth == 1 || int( depth ) == QPixmap::defaultDepth() );
    QImage image;
    if ( ok ) {
        GC gc = XCreateGC( dpy, Pixmap( id ), 0, 0 );
        if ( depth == 1 ) {
            QBitmap bitmap( w, h );
            XCopyArea( dpy, Pixmap( id ), bitmap.handle(), gc, 0, 0, w, h, 0, 0 );
            XSync( dpy, False );
            if ( !s_drawableError )
                image = bitmap.convertToImage();
        } else {
            QPixmap pixmap( w, h );
            XCopyArea( dpy, Pixmap( id ), pixmap.handle(), gc, 0, 0, w, h, 0, 0 );
            XSync( dpy, False );
            if ( !s_drawableError )
                image = pixmap.convertToImage();
        }
        XFreeGC( dpy, gc );
        XSync( dpy, False );
    }
    XSetErrorHandler( previous );
    if ( debugging() )
        fprintf( stderr, "kwpe: clipboard PIXMAP %ux%u depth %u: %s\n", w, h, depth, image.isNull() ? "refused" : "converted" );
    if ( image.isNull() )
        return false;
    QByteArray encoded;
    QBuffer buffer( encoded );
    buffer.open( IO_WriteOnly );
    QImageIO io( &buffer, "PNG" );
    io.setImage( image );
    if ( !io.write() )
        return false;
    buffer.close();
    png.assign( encoded.data(), encoded.size() );
    return true;
}

// The paste's data in the format WebKit asks for, from another program.
static bool readForeignFormat( const char* format, std::string& bytes )
{
    if ( qstrnicmp( format, "text/plain", 10 ) == 0 )
        return readForeignText( bytes );
    if ( qstricmp( format, "text/html" ) == 0 )
        return readForeignHtml( bytes );
    if ( qstricmp( format, "image/png" ) == 0 && !offered( "image/png" ) && offered( "PIXMAP" ) )
        return readForeignPixmap( bytes );
    // Only what the owner offers: a target it does not know costs a round
    // trip, or the whole wait if it does not answer.
    const char* name = offeredName( format );
    return name && readForeign( name, bytes );
}

// WebKit's own copies (is_local) go to the X CLIPBOARD.
static void kwpeClipboardChanged( WPEClipboard* clipboard, GPtrArray* formats, gboolean isLocal, WPEClipboardContent* content )
{
    WPE_CLIPBOARD_CLASS( kwpe_clipboard_x11_parent_class )->changed( clipboard, formats, isLocal, content );
    if ( s_updating || !isLocal || !qApp )
        return;
    if ( s_localContent )
        wpe_clipboard_content_unref( s_localContent );
    s_localContent = content ? wpe_clipboard_content_ref( content ) : 0;
    QClipboard* cb = QApplication::clipboard();
    if ( !content ) {
        if ( s_forwarded && cb->ownsClipboard() && cb->data( QClipboard::Clipboard ) == s_forwarded )
            cb->clear( QClipboard::Clipboard );
        return;
    }
    WebKitContent* source = new WebKitContent;
    if ( const char* text = wpe_clipboard_content_get_text( content ) ) {
        QByteArray utf8 = toByteArray( text, strlen( text ) );
        source->add( "text/plain;charset=UTF-8", utf8 );
        // Latin-1 for old programs that ask for plain "text/plain"
        QCString latin1 = QString::fromUtf8( text ).latin1();
        source->add( "text/plain", toByteArray( latin1.data(), latin1.length() ) );
    }
    static const char* const others[] = { "text/html", "text/uri-list" };
    for ( unsigned i = 0; i < sizeof( others ) / sizeof( others[0] ); ++i )
        if ( GBytes* bytes = wpe_clipboard_content_get_bytes( content, others[i] ) ) {
            gsize size = 0;
            const char* data = static_cast<const char*>( g_bytes_get_data( bytes, &size ) );
            source->add( others[i], toByteArray( data, size ) );
        }
    if ( !source->format( 0 ) ) {
        delete source;
        return;
    }
    s_forwarded = source;
    cb->setData( source, QClipboard::Clipboard );
}

// Another program's data: only while a paste the user started runs.
static GBytes* kwpeClipboardRead( WPEClipboard*, const char* format )
{
    if ( debugging() )
        fprintf( stderr, "kwpe: clipboard read %s: %s\n", printable( format ).data(), g_get_monotonic_time() > s_pasteUntil ? "refused" : "allowed" );
    if ( !qApp || g_get_monotonic_time() > s_pasteUntil || !format ) {
        dropCache();   // that paste is over
        return 0;
    }
    // What the user pasted, not what was copied after the key (text a
    // password manager put there meanwhile).
    if ( XGetSelectionOwner( qt_xdisplay(), selectionAtom( s_pasteMode ) ) != s_pasteOwner ) {
        if ( debugging() )
            fprintf( stderr, "kwpe: clipboard read %s: refused, the selection changed hands\n", printable( format ).data() );
        s_pasteUntil = 0;
        dropCache();
        return 0;
    }
    if ( s_pasteOwn ) {
        // Our own selection (another view's copy, the location bar): Qt has
        // the data, no X traffic.
        QClipboard* cb = QApplication::clipboard();
        QCString data;
        if ( qstrnicmp( format, "text/plain", 10 ) == 0 ) {
            data = cb->text( s_pasteMode ).utf8();
        } else if ( qstricmp( format, "text/html" ) == 0 ) {
            QCString subtype = "html";
            data = cb->text( subtype, s_pasteMode ).utf8();
        } else {
            QMimeSource* source = cb->data( s_pasteMode );
            QByteArray bytes = source ? source->encodedData( format ) : QByteArray();
            if ( bytes.isEmpty() )
                return 0;
            return g_bytes_new( bytes.data(), bytes.size() );
        }
        if ( data.isNull() )
            return 0;
        return g_bytes_new( data.data(), data.length() );
    }

    // Another program's: read with KWPEXSelection; WebKit asks for the same
    // data more than once per paste, the last answer is kept for that.
    QCString key = QCString( format ).lower();
    if ( key.left( 10 ) == "text/plain" )
        key = "text/plain";   // both of WebKit's names are the same UTF-8 text
    if ( s_cachedBytes && s_cachedFormat == key )
        return g_bytes_ref( s_cachedBytes );
    gint64 start = g_get_monotonic_time();
    std::string bytes;
    if ( !readForeignFormat( format, bytes ) || bytes.empty() )
        return 0;
    dropCache();
    s_cachedFormat = key;
    s_cachedBytes = g_bytes_new( bytes.data(), bytes.size() );
    // A slow owner (KSnapshot encodes a big screenshot, a pieces transfer)
    // used up the window: WebKit's further reads of this paste come right
    // after.  Once per paste.
    gint64 now = g_get_monotonic_time();
    if ( !s_pasteExtended && now - start > 300000 && now + G_USEC_PER_SEC > s_pasteUntil
         && now < s_pasteStart + 15 * G_USEC_PER_SEC ) {
        s_pasteUntil = now + G_USEC_PER_SEC;
        s_pasteExtended = true;
    }
    if ( now < s_pasteUntil )
        s_cacheTimer = g_timeout_add( guint( ( s_pasteUntil - now ) / 1000 ) + 50, cacheExpired, 0 );
    else
        dropCache();   // (the bytes are returned below)
    return g_bytes_new( bytes.data(), bytes.size() );
}

static void kwpeClipboardDispose( GObject* object )
{
    if ( s_clipboard == reinterpret_cast<WPEClipboard*>( object ) ) {
        s_clipboard = 0;
        if ( s_localContent )
            wpe_clipboard_content_unref( s_localContent );
        s_localContent = 0;
        dropCache();
    }
    G_OBJECT_CLASS( kwpe_clipboard_x11_parent_class )->dispose( object );
}

static void kwpe_clipboard_x11_init( KWPEClipboardX11* )
{
}

static void kwpe_clipboard_x11_class_init( KWPEClipboardX11Class* klass )
{
    G_OBJECT_CLASS( klass )->dispose = kwpeClipboardDispose;
    WPEClipboardClass* clipboardClass = WPE_CLIPBOARD_CLASS( klass );
    clipboardClass->changed = kwpeClipboardChanged;
    clipboardClass->read = kwpeClipboardRead;
}

WPEClipboard* KWPE::createClipboard( WPEDisplay* display )
{
    WPEClipboard* clipboard = WPE_CLIPBOARD( g_object_new( kwpe_clipboard_x11_get_type(), "display", display, NULL ) );
    s_clipboard = clipboard;
    return clipboard;
}

// ---------------------------------------------------------------------

void KWPEClipboard::noteXEvent( const XEvent* event )
{
    KWPEXSelection::noteRequest( event );
}

bool KWPEClipboard::preparePaste( QClipboard::Mode mode )
{
    if ( !s_clipboard || !qApp )
        return false;
    dropCache();
    QClipboard* cb = QApplication::clipboard();
    QMimeSource* source = cb->data( mode );
    // WebKit's own copy, still in the X CLIPBOARD: WebKit pastes it from
    // its own content (with a page's custom formats), given back to it in
    // case a PRIMARY paste replaced it meanwhile.
    if ( mode == QClipboard::Clipboard && s_forwarded && s_localContent && cb->ownsClipboard() && source == s_forwarded ) {
        s_pasteUntil = 0;
        if ( wpe_clipboard_get_content( s_clipboard ) != s_localContent ) {
            s_updating = true;
            wpe_clipboard_set_content( s_clipboard, s_localContent );
            s_updating = false;
        }
        return true;
    }

    // The formats WebKit sees: text (both of WebKit's names), HTML, URI
    // lists and images; not every private format of other programs.
    // Another program's targets are read here directly (Qt 3 would wait 5 s
    // for an owner that does not answer, and claim text then).
    // The paste is of what the owner at the key had, asked for with the
    // key's time (ICCCM); an owner change meanwhile ends it.
    s_pasteTime = qt_x_time;
    s_pasteMode = mode;
    s_pasteOwner = XGetSelectionOwner( qt_xdisplay(), selectionAtom( mode ) );
    s_pasteOwn = ownsSelection( mode );
    s_offered.clear();
    if ( s_pasteOwn ) {
        for ( int i = 0; source && source->format( i ); ++i )
            s_offered.append( QCString( source->format( i ) ) );
    } else if ( !readTargets( mode, s_offered ) ) {
        s_offered.clear();   // no owner, or it does not answer: nothing to paste
    } else if ( s_offered.isEmpty() ) {
        s_offered.append( "TEXT" );   // no TARGETS (an old program): text is tried
    }
    if ( !s_pasteOwn && XGetSelectionOwner( qt_xdisplay(), selectionAtom( mode ) ) != s_pasteOwner ) {
        if ( debugging() )
            fprintf( stderr, "kwpe: paste: the selection changed hands while asking for its formats\n" );
        s_offered.clear();
    }
    bool image = false, pixmap = false, html = false;
    bool htmlPublished = false;
    GPtrArray* formats = g_ptr_array_new();
    bool text = false;
    for ( QValueList<QCString>::ConstIterator it = s_offered.begin(); it != s_offered.end(); ++it ) {
        const char* format = ( *it ).data();
        if ( s_pasteOwn ? qstrnicmp( format, "text/plain", 10 ) == 0
                        : ( readableMime( format, "text/plain" ) || qstrcmp( format, "UTF8_STRING" ) == 0
                            || qstrcmp( format, "STRING" ) == 0 || qstrcmp( format, "TEXT" ) == 0
                            || qstrcmp( format, "COMPOUND_TEXT" ) == 0 ) )
            text = true;
        else if ( !s_pasteOwn && qstrcmp( format, "PIXMAP" ) == 0 )
            pixmap = true;
        else if ( !s_pasteOwn && qstrnicmp( format, "text/html;", 10 ) == 0 )
            html = html || readableMime( format, "text/html" );   // published as plain text/html
        else if ( ( qstricmp( format, "text/html" ) == 0 || qstricmp( format, "text/uri-list" ) == 0
                    || qstrnicmp( format, "image/", 6 ) == 0 ) ) {
            html = html || qstricmp( format, "text/html" ) == 0;
            image = image || qstrnicmp( format, "image/", 6 ) == 0;
            htmlPublished = htmlPublished || qstricmp( format, "text/html" ) == 0;
            g_ptr_array_add( formats, const_cast<char*>( g_intern_string( QCString( format ).lower().data() ) ) );
        }
    }
    if ( html && !htmlPublished )
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "text/html" ) ) );
    if ( pixmap && !image )   // converted from the drawable (readForeignPixmap)
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "image/png" ) ) );
    if ( text ) {
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "text/plain" ) ) );
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "text/plain;charset=utf-8" ) ) );
    }
    bool empty = formats->len == 0;
    g_ptr_array_add( formats, 0 );   // null-terminated (wpe_clipboard_get_formats)
    g_ptr_array_set_size( formats, formats->len - 1 );

    if ( debugging() )
        fprintf( stderr, "kwpe: paste from %s, %u format(s)\n", mode == QClipboard::Selection ? "PRIMARY" : "CLIPBOARD", formats->len );
    s_pasteStart = g_get_monotonic_time();
    s_pasteUntil = empty ? 0 : s_pasteStart + G_USEC_PER_SEC;
    s_pasteExtended = false;
    s_updating = true;
    WPE_CLIPBOARD_GET_CLASS( s_clipboard )->changed( s_clipboard, formats, FALSE, 0 );
    s_updating = false;
    g_ptr_array_unref( formats );
    return !empty;
}
