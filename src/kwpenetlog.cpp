/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include <wpe/webkit.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "kwpenetlog.h"

namespace
{

const char startKey[] = "kwpe-netlog-start";
const gint64 streamUsec = 60 * G_USEC_PER_SEC;

void stamp( char* buffer, size_t size )
{
    struct timeval tv;
    gettimeofday( &tv, 0 );
    struct tm tm;
    localtime_r( &tv.tv_sec, &tm );
    snprintf( buffer, size, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, int( tv.tv_usec / 1000 ) );
}

// scheme://host:port/path: no query, no fragment, no user name; the path
// as sent (percent-encoded, no control characters); data: and blob: only
// as their scheme (the data, or another URL, is in the "path")
gchar* shortURI( WebKitWebResource* resource )
{
    const char* uri = webkit_web_resource_get_uri( resource );
    if ( !uri )
        return g_strdup( "?" );
    if ( g_str_has_prefix( uri, "data:" ) || g_str_has_prefix( uri, "blob:" ) )
        return g_strndup( uri, 5 );
    GUri* parsed = g_uri_parse( uri, G_URI_FLAGS_ENCODED, 0 );
    if ( !parsed )
        return g_strdup( "(unparsable URL)" );
    const char* host = g_uri_get_host( parsed );
    int port = g_uri_get_port( parsed );
    gchar* portText = port > 0 ? g_strdup_printf( ":%d", port ) : g_strdup( "" );
    gchar* text = g_strdup_printf( "%s://%s%s%s", g_uri_get_scheme( parsed ), host ? host : "", portText, g_uri_get_path( parsed ) );
    g_free( portText );
    g_uri_unref( parsed );
    for ( char* c = text; *c; ++c )
        if ( (unsigned char) *c < 0x20 || *c == 0x7f )
            *c = '?';
    if ( strlen( text ) > 160 )
        strcpy( text + 150, "..." );
    return text;
}

gint64 elapsedMs( WebKitWebResource* resource )
{
    gint64* start = static_cast<gint64*>( g_object_get_data( G_OBJECT( resource ), startKey ) );
    return start ? ( g_get_monotonic_time() - *start ) / 1000 : -1;
}

void onFailed( WebKitWebResource* resource, GError* error, gpointer )
{
    // a load the page or WebKit gave up itself (navigation, abort())
    if ( g_error_matches( error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED ) )
        return;
    gchar* uri = shortURI( resource );
    KWPENetLog::note( "FAILED %s after %lld ms: %s %d: %s", uri, (long long) elapsedMs( resource ),
                      g_quark_to_string( error->domain ), error->code, error->message );
    g_free( uri );
}

void onFinished( WebKitWebResource* resource, gpointer )
{
    WebKitURIResponse* response = webkit_web_resource_get_response( resource );
    guint status = response ? webkit_uri_response_get_status_code( response ) : 0;
    gint64 ms = elapsedMs( resource );
    if ( status >= 400 || ms * 1000 >= streamUsec ) {
        gchar* uri = shortURI( resource );
        KWPENetLog::note( "%s %s: HTTP %u after %lld ms", status >= 400 ? "ERROR" : "ended", uri, status, (long long) ms );
        g_free( uri );
    }
}

void onResourceLoadStarted( WebKitWebView*, WebKitWebResource* resource, WebKitURIRequest*, gpointer )
{
    gint64* start = g_new( gint64, 1 );
    *start = g_get_monotonic_time();
    g_object_set_data_full( G_OBJECT( resource ), startKey, start, g_free );
    g_signal_connect( resource, "failed", G_CALLBACK( onFailed ), 0 );
    g_signal_connect( resource, "finished", G_CALLBACK( onFinished ), 0 );
}

void noteNetwork( GNetworkMonitor* monitor )
{
    static int last = -1;
    int available = g_network_monitor_get_network_available( monitor );
    if ( available == last )
        return;
    last = available;
    KWPENetLog::note( "network %s (pages: navigator.onLine = %s), %s, connectivity %d", available ? "available" : "NOT available",
                      available ? "true" : "false", G_OBJECT_TYPE_NAME( monitor ), int( g_network_monitor_get_connectivity( monitor ) ) );
}

void onNetworkChanged( GNetworkMonitor* monitor, gboolean, gpointer )
{
    noteNetwork( monitor );
}

}

bool KWPENetLog::enabled()
{
    static int on = -1;
    if ( on < 0 ) {
        const char* value = getenv( "KWPE_NETLOG" );
        on = value && *value && strcmp( value, "0" ) != 0;
    }
    return on;
}

void KWPENetLog::start()
{
    static bool started = false;
    if ( !enabled() || started )
        return;
    started = true;
    GNetworkMonitor* monitor = g_network_monitor_get_default();
    // (some monitors know the state only after a D-Bus round trip: the
    // first line can be "NOT available", followed by the real state)
    noteNetwork( monitor );
    g_signal_connect( monitor, "network-changed", G_CALLBACK( onNetworkChanged ), 0 );
}

void KWPENetLog::watch( WebKitWebView* view )
{
    if ( !enabled() )
        return;
    start();
    g_signal_connect( view, "resource-load-started", G_CALLBACK( onResourceLoadStarted ), 0 );
}

void KWPENetLog::note( const char* format, ... )
{
    if ( !enabled() )
        return;
    char time[32];
    stamp( time, sizeof( time ) );
    char text[512];
    va_list args;
    va_start( args, format );
    vsnprintf( text, sizeof( text ), format, args );
    va_end( args );
    fprintf( stderr, "kwpe: net %s %s\n", time, text );
}
