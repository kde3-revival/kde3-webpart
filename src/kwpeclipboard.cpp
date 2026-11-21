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
#include "wpex11platform.h"

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
static bool s_updating = false;                 // our own changed() call

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
    if ( !qApp || g_get_monotonic_time() > s_pasteUntil || !format )
        return 0;
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

static void kwpeClipboardDispose( GObject* object )
{
    if ( s_clipboard == reinterpret_cast<WPEClipboard*>( object ) ) {
        s_clipboard = 0;
        if ( s_localContent )
            wpe_clipboard_content_unref( s_localContent );
        s_localContent = 0;
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
    GPtrArray* formats = g_ptr_array_new();
    bool text = false;
    for ( int i = 0; source && source->format( i ); ++i ) {
        const char* format = source->format( i );
        if ( qstrnicmp( format, "text/plain", 10 ) == 0 )
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
    s_pasteUntil = empty ? 0 : g_get_monotonic_time() + G_USEC_PER_SEC;
    s_updating = true;
    WPE_CLIPBOARD_GET_CLASS( s_clipboard )->changed( s_clipboard, formats, FALSE, 0 );
    s_updating = false;
    g_ptr_array_unref( formats );
    return !empty;
}
