/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPENETLOG_H
#define KWPENETLOG_H

typedef struct _WebKitWebView WebKitWebView;

/**
 * KWPE_NETLOG=1: what a web app's "connection lost" could come from, on
 * stderr with the time of day, to be compared with the moment the page
 * said it:
 *
 *  - the network state WebKit gives pages as navigator.onLine (GLib's
 *    network monitor; the network process uses the same one), at the
 *    start and on every change;
 *  - every subresource (fetch, XHR, streams, images...) that failed, or
 *    ended with an HTTP error, and streams (60 s or longer) when they end:
 *    scheme, host and path only (no query, no data);
 *  - the page's view shown / hidden and its window active / inactive.
 *
 * WebSockets are not resources: WebKit tells the embedder nothing of them.
 */
namespace KWPENetLog
{

bool enabled();

/** Starts the network state lines (once). */
void start();

/** The subresources of @p view. */
void watch( WebKitWebView* view );

/** A free line ("view hidden", ...), with the time. */
void note( const char* format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );

}

#endif
