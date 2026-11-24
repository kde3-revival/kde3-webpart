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
#include <qclipboard.h>
#include <qcstring.h>
#include <qmime.h>
#include <qstrlist.h>
#include <qvaluelist.h>

#include "kwpeclipboard.h"
#include "kwpexselection.h"
#include "wpex11platform.h"

#include <X11/Xatom.h>

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

// Requests to our own selections (e.g. another tab's copy, still in the
// CLIPBOARD) while we wait: Qt answers them.
static void serviceEvent( XEvent* event )
{
    if ( qApp )
        qApp->x11ProcessEvent( event );
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
                                                          qt_x_time, targetsLimits, data, 0, &format, serviceEvent );
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
        fprintf( stderr, "kwpe: clipboard read %s: %s\n", format, g_get_monotonic_time() > s_pasteUntil ? "refused" : "allowed" );
    if ( !qApp || g_get_monotonic_time() > s_pasteUntil || !format ) {
        dropCache();   // that paste is over
        return 0;
    }
    // What the user pasted, not what was copied after the key (text a
    // password manager put there meanwhile).
    if ( XGetSelectionOwner( qt_xdisplay(), selectionAtom( s_pasteMode ) ) != s_pasteOwner ) {
        if ( debugging() )
            fprintf( stderr, "kwpe: clipboard read %s: refused, the selection changed hands\n", format );
        s_pasteUntil = 0;
        dropCache();
        return 0;
    }
    QClipboard* cb = QApplication::clipboard();
    QCString data;
    if ( qstrnicmp( format, "text/plain", 10 ) == 0 ) {
        data = cb->text( s_pasteMode ).utf8();
    } else if ( qstricmp( format, "text/html" ) == 0 ) {
        QCString subtype = "html";
        data = cb->text( subtype, s_pasteMode ).utf8();
    } else if ( s_pasteOwn ) {
        QMimeSource* source = cb->data( s_pasteMode );
        QByteArray bytes = source ? source->encodedData( format ) : QByteArray();
        if ( bytes.isEmpty() )
            return 0;
        return g_bytes_new( bytes.data(), bytes.size() );
    } else {
        // Only what the owner offers (WebKit also asks for its own private
        // type): a target it does not know costs a round trip, or the whole
        // wait if it does not answer.
        const char* name = offeredName( format );
        if ( !name )
            return 0;
        if ( s_cachedBytes && s_cachedFormat == name )
            return g_bytes_ref( s_cachedBytes );
        Display* dpy = qt_xdisplay();
        std::string bytes;
        gint64 start = g_get_monotonic_time();
        KWPEXSelection::Result result = KWPEXSelection::read( dpy, selectionAtom( s_pasteMode ), XInternAtom( dpy, name, False ),
                                                              qt_x_time, dataLimits, bytes, 0, 0, serviceEvent );
        if ( debugging() )
            fprintf( stderr, "kwpe: clipboard %s: %s, %u byte(s) in %d ms\n", name, KWPEXSelection::resultName( result ),
                     unsigned( bytes.size() ), int( ( g_get_monotonic_time() - start ) / 1000 ) );
        if ( result != KWPEXSelection::Ok || bytes.empty() )
            return 0;
        dropCache();
        s_cachedFormat = name;
        s_cachedBytes = g_bytes_new( bytes.data(), bytes.size() );
        // A slow owner (KSnapshot encodes a big screenshot, a pieces
        // transfer) used up the window: WebKit's further reads of this
        // paste come right after.  Once per paste.
        gint64 now = g_get_monotonic_time();
        if ( !s_pasteExtended && now - start > 300000 && now + G_USEC_PER_SEC > s_pasteUntil
             && now < s_pasteStart + 15 * G_USEC_PER_SEC ) {
            s_pasteUntil = now + G_USEC_PER_SEC;
            s_pasteExtended = true;
        }
        s_cacheTimer = g_timeout_add( guint( ( s_pasteUntil - now ) / 1000 ) + 50, cacheExpired, 0 );
        return g_bytes_ref( s_cachedBytes );
    }
    if ( data.isNull() )
        return 0;
    return g_bytes_new( data.data(), data.length() );
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
    s_pasteOwn = ownsSelection( mode );
    s_offered.clear();
    if ( s_pasteOwn ) {
        for ( int i = 0; source && source->format( i ); ++i )
            s_offered.append( QCString( source->format( i ) ) );
    } else if ( !readTargets( mode, s_offered ) ) {
        s_offered.clear();   // no owner, or it does not answer: nothing to paste
    } else if ( s_offered.isEmpty() ) {
        s_offered.append( "text/plain" );
    }
    GPtrArray* formats = g_ptr_array_new();
    bool text = false;
    for ( QValueList<QCString>::ConstIterator it = s_offered.begin(); it != s_offered.end(); ++it ) {
        const char* format = ( *it ).data();
        if ( qstrnicmp( format, "text/plain", 10 ) == 0 || qstrcmp( format, "UTF8_STRING" ) == 0
             || qstrcmp( format, "STRING" ) == 0 || qstrcmp( format, "TEXT" ) == 0 || qstrcmp( format, "COMPOUND_TEXT" ) == 0 )
            text = true;
        else if ( ( qstricmp( format, "text/html" ) == 0 || qstricmp( format, "text/uri-list" ) == 0
                    || qstrnicmp( format, "image/", 6 ) == 0 ) )
            g_ptr_array_add( formats, const_cast<char*>( g_intern_string( QCString( format ).lower().data() ) ) );
    }
    if ( text ) {
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "text/plain" ) ) );
        g_ptr_array_add( formats, const_cast<char*>( g_intern_static_string( "text/plain;charset=utf-8" ) ) );
    }
    bool empty = formats->len == 0;
    g_ptr_array_add( formats, 0 );   // null-terminated (wpe_clipboard_get_formats)
    g_ptr_array_set_size( formats, formats->len - 1 );

    if ( debugging() )
        fprintf( stderr, "kwpe: paste from %s, %u format(s)\n", mode == QClipboard::Selection ? "PRIMARY" : "CLIPBOARD", formats->len );
    s_pasteMode = mode;
    s_pasteStart = g_get_monotonic_time();
    s_pasteUntil = empty ? 0 : s_pasteStart + G_USEC_PER_SEC;
    s_pasteExtended = false;
    s_pasteOwner = XGetSelectionOwner( qt_xdisplay(), selectionAtom( mode ) );
    s_updating = true;
    WPE_CLIPBOARD_GET_CLASS( s_clipboard )->changed( s_clipboard, formats, FALSE, 0 );
    s_updating = false;
    g_ptr_array_unref( formats );
    return !empty;
}
