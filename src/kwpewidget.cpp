/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WebKit (GLib) before any Qt header: Qt 3 defines "signals", "slots"
// and "emit" as macros, GLib uses those words.
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <qapplication.h>
#include <qclipboard.h>
#include <qcursor.h>
#include <qdragobject.h>
#include <qdatetime.h>
#include <qregexp.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qguardedptr.h>
#include <qimage.h>
#include <qfile.h>
#include <qmap.h>
#include <qvaluelist.h>
#include <qtimer.h>
#include <qevent.h>

#include <kdeversion.h>
#include <kglobal.h>
#include <kstringhandler.h>
#include <kmdcodec.h>
#include <ksimpleconfig.h>
#include <kio/passdlg.h>
#include <klocale.h>
#include <kmimetype.h>
#include <kurl.h>
#include <kstandarddirs.h>

#include "kwpeadblock.h"
#include "kwpemailsanitizer.h"
#include "kwpeclipboard.h"
#include "kwpenetlog.h"
#include "kwpedialogs.h"
#include "kwpedownload.h"
#include "kwpesettings.h"
#include "kwpewidget.h"
#include "wpex11platform.h"

// Qt 3 internals (qapplication_x11.cpp): the X event filter hook and the
// time of the X event being delivered.
typedef int ( *QX11EventFilter )( XEvent* );
extern QX11EventFilter qt_set_x11_event_filter( QX11EventFilter filter );
extern unsigned long qt_x_time;

// ---------------------------------------------------------------------
// process-wide WebKit state

static WebKitWebContext* s_context = 0;
static WebKitNetworkSession* s_session = 0;
static WebKitSettings* s_settings = 0;
static WPEDisplay* s_display = 0;
static QX11EventFilter s_previousFilter = 0;

static int kwpeX11EventFilter( XEvent* event )
{
    KWPEClipboard::noteXEvent( event );   // (requests for our selections)
    if ( KWPE::handleXEvent( event ) )
        return 1;
    return s_previousFilter ? s_previousFilter( event ) : 0;
}

// The KWPEWidget of a WebKitWebView (0 once the widget is gone).
static const char widgetKey[] = "kwpe-widget";

// A download WebKit starts (attachment, <a download>, downloadURL()):
// KDE's save dialog and KIO's progress window.
static void onDownloadStarted( WebKitNetworkSession*, WebKitDownload* download, gpointer )
{
    WebKitWebView* view = webkit_download_get_web_view( download );
    QWidget* widget = view ? static_cast<QWidget*>( g_object_get_data( G_OBJECT( view ), widgetKey ) ) : 0;
    KWPEDownload::start( download, widget );
}

static bool envFlag( const char* name )
{
    const char* value = getenv( name );
    return value && *value && strcmp( value, "0" ) != 0;
}

// The X11 WPE display, once per process.  The module is never unloaded
// (__kde_do_not_unload in the part: GObject types and the X event filter
// cannot go away).
static bool ensureDisplay()
{
    if ( s_display )
        return true;

    // The display first: WebKit uses the WPEPlatform API as soon as the
    // WPEDisplay type exists.
    s_display = KWPE::x11Display( qt_xdisplay() );
    if ( !s_display )
        return false;

    s_previousFilter = qt_set_x11_event_filter( kwpeX11EventFilter );
    // The clipboard now, not at WebKit's first paste: KWPEClipboard tells
    // it what the X selections offer before that.
    wpe_display_get_clipboard( s_display );
    return true;
}

// Konqueror's WebKit, once per process.
static bool ensureWebKit()
{
    if ( s_context )
        return true;
    if ( !ensureDisplay() )
        return false;

    if ( envFlag( "KWPE_EPHEMERAL" ) ) {
        s_session = webkit_network_session_new_ephemeral();
    } else {
        // Website data (local storage, IndexedDB, service workers, HSTS,
        // ...) and cookies in $KDEHOME/share/apps/kwpepart, the HTTP cache
        // in KDE's cache directory.  KDE's cookie policy (kcookiejar)
        // does not apply: WebKit's default (no third-party cookies).
        QString dataDir = KGlobal::dirs()->saveLocation( "data", "kwpepart/" );
        QCString cacheDir = QFile::encodeName( KGlobal::dirs()->saveLocation( "cache", "kwpepart/" ) );
        s_session = webkit_network_session_new( QFile::encodeName( dataDir ).data(), cacheDir.data() );
        webkit_cookie_manager_set_persistent_storage( webkit_network_session_get_cookie_manager( s_session ),
                                                      QFile::encodeName( dataDir + "cookies.sqlite" ).data(),
                                                      WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE );
    }

    g_signal_connect( s_session, "download-started", G_CALLBACK( onDownloadStarted ), 0 );
    // Explicit, whatever WebKit's default: a certificate error fails the
    // load (WebKit's error page), it is never accepted silently.
    webkit_network_session_set_tls_errors_policy( s_session, WEBKIT_TLS_ERRORS_POLICY_FAIL );

    s_context = webkit_web_context_new();
    webkit_web_context_set_cache_model( s_context, WEBKIT_CACHE_MODEL_WEB_BROWSER );

    s_settings = webkit_settings_new();
    KWPEWidget::applySettings();
    webkit_settings_set_enable_developer_extras( s_settings, envFlag( "KWPE_DEVELOPER_EXTRAS" ) );
    // WebKit's user agent with "Konqueror/<KDE version>" after it.  WebKit's
    // own, alone or as another Safari's, gets a Cloudflare challenge on
    // some sites' WebSocket handshakes (claude.ai: 403 "Cf-Mitigated:
    // challenge", the page loses its connection); with the application
    // named it is accepted.  KWPE_USER_AGENT: one of one's choice, for
    // diagnosis.
    const char* userAgent = getenv( "KWPE_USER_AGENT" );
    if ( userAgent && *userAgent )
        webkit_settings_set_user_agent( s_settings, userAgent );
    else
        webkit_settings_set_user_agent_with_application_details( s_settings, "Konqueror", KDE_VERSION_STRING );
    // Explicit, whatever WebKit's defaults: file: pages read no other
    // files, scripts open no windows by themselves.
    webkit_settings_set_allow_file_access_from_file_urls( s_settings, FALSE );
    webkit_settings_set_allow_universal_access_from_file_urls( s_settings, FALSE );
    webkit_settings_set_javascript_can_open_windows_automatically( s_settings, FALSE );
    // Pages read the clipboard only when the user pastes (KWPEClipboard).
    webkit_settings_set_javascript_can_access_clipboard( s_settings, FALSE );
    // WebKit 2.54 drops the contents of GPU canvases of pages kept in the
    // back/forward cache: going back shows them empty.  Skia's CPU canvas
    // keeps them (KWPE_CANVAS_ACCELERATION=1 to compare).
    webkit_settings_set_enable_2d_canvas_acceleration( s_settings, envFlag( "KWPE_CANVAS_ACCELERATION" ) );
    return true;
}

// ---------------------------------------------------------------------
// mail mode (KMail's message viewer)
//
// Its own context (own web processes, no memory cache for documents no
// longer shown, no page cache), its own ephemeral network session (no
// cookies, caches, storage of Konqueror's; nothing on disk; gone with the
// process) and its own settings (no page scripts).  The host writes a
// message as files into a private directory; WebKit reads them through
// the kwpemail: scheme, one host name per message (kwpemail://m<n>/path):
// a new origin for each.  The opened file is the host's document (the
// headers); every other HTML file is message content, which the host
// shows in a sandboxed <iframe>.  The Content-Security-Policy of the
// responses is what keeps remote content out (and scripts, forms,
// frames, plugins), whatever the HTML says.

static WebKitWebContext* s_mailContext = 0;
// Render-only messages: a session that cannot reach anything (see
// ensureMailWebKit()).  A message with external references gets a
// session of its own (KWPEWidgetPrivate::mailExternalSession).
static WebKitNetworkSession* s_mailSession = 0;
static WebKitSettings* s_mailSettings = 0;
static const char mailScheme[] = "kwpemail";
static const char mailWorld[] = "kwpemail";
static const char mailStateHandler[] = "kwpeMailState";

// App mode: the picture of the page's notifications.  WebKit's
// notification has no icon (WebKitNotification: title, body, tag), so
// two scripts (the top frame, before the page's scripts) find it:
//
// appNotificationWrapper, in the page's world, wraps the Notification
// constructor: it converts the constructor's arguments once, as WebKit's
// bindings do (the members of WebKit's NotificationOptions only, in
// their order: body, data, dir, icon, lang, silent, tag; each read and
// converted before the next is read; an invalid dir stops there, with
// WebKit's own error), copies them to an object of their own and gives
// the native constructor exactly these values.  Only when the native
// constructor succeeded (it shows the notification later, from a task
// of its own), it announces title, body, tag and icon (if any) of every
// notification to the other script by a DOM event on the top document
// (a frame of another site cannot reach it): the host sees each one
// constructed, also those without a picture.  It uses only
// what it took before the page's scripts ran.  What it announces is the
// page's word, nothing more.
//
// appNotificationBridge, in a world of its own (appNotificationWorld:
// the page's scripts, of any frame, cannot run there; the message
// handler exists only there), passes every announcement on ("pending",
// with or without a picture) and turns one with an icon of the page's
// own origin, data: or blob:, or of an origin the application lists
// (KWPEAppOptions::pictureOrigins), nothing of any other site, into the
// picture's pixels.  An http(s) picture is fetched first (at most 4 MiB,
// counted as its body comes: more cancels the fetch),
// then decoded from its blob: one of the page's origin in "same-origin"
// mode (a redirect to another origin fails), one of a listed origin with
// CORS, no credentials and no redirect at all (a listed server could send
// it anywhere; the server must allow CORS, or there is no picture); two
// fetches at a time (a flood of announcements is not a flood of
// requests: the others wait, or time out).  The
// picture is drawn into a canvas of at most 96 pixels in the web
// process, sent as RGBA (width x height x 4 bytes, base64); the host
// fetches nothing and decodes no image format.  Title, body and tag
// are taken up to 4096 characters, the icon's URL up to 512 KiB (a
// data: URL; what reaches the host is bounded by the pixels anyway).
// Every message carries
// the document's token (random, made by the script when the document
// starts, sent first as "hello"): the host takes only the current
// document's messages.  "pending" comes before WebKit shows the
// notification (the constructor schedules that for a later task), the
// picture or none when it is ready (at most 1.5 seconds later).
static const char appNotificationHandler[] = "kwpeNotification";
static const char appNotificationWorld[] = "kwpenotification";
static const char appNotificationWrapper[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var N = window.Notification;\n"
    "  if (typeof N !== 'function') return;\n"
    "  var construct = Reflect.construct, apply = Reflect.apply, create = Object.create,\n"
    "      defineProperty = Object.defineProperty, setPrototypeOf = Object.setPrototypeOf,\n"
    "      quote = JSON.stringify, toString = String, TypeError_ = TypeError, Event_ = window.CustomEvent,\n"
    "      dispatch = EventTarget.prototype.dispatchEvent, doc = document;\n"
    "  function string(value) {\n"
    "    if (typeof value === 'symbol') throw new TypeError_('Cannot convert a symbol to a string');\n"
    "    return toString(value);\n"
    "  }\n"
    "  function define(object, key, value) {\n"
    "    var d = create(null);\n"
    "    d.value = value; d.writable = true; d.enumerable = true; d.configurable = true;\n"
    "    defineProperty(object, key, d);\n"
    "  }\n"
    "  function announce(title, options) {\n"
    "    try {\n"
    "      var body = options && options.body !== undefined ? options.body : '',\n"
    "          tag = options && options.tag !== undefined ? options.tag : '',\n"
    "          icon = options && options.icon !== undefined ? options.icon : '';\n"
    "      var detail = '[' + quote(title) + ',' + quote(body) + ',' + quote(tag) + ',' + quote(icon) + ']';\n"
    "      var init = create(null);\n"
    "      init.detail = detail;\n"
    "      apply(dispatch, doc, [construct(Event_, ['kwpe-notification', init])]);\n"
    "    } catch (e) {}\n"
    "  }\n"
    "  function Notification(title) {\n"
    "    if (!new.target) return apply(N, this, arguments);\n"
    "    if (arguments.length === 0) return construct(N, [], new.target);\n"
    "    var t = string(title), options = arguments[1], copy = options, v;\n"
    "    if (options !== undefined && options !== null && (typeof options === 'object' || typeof options === 'function')) {\n"
    "      copy = create(null);\n"
    "      v = options.body; if (v !== undefined) define(copy, 'body', string(v));\n"
    "      v = options.data; if (v !== undefined) define(copy, 'data', v);\n"
    "      v = options.dir;\n"
    "      if (v !== undefined) {\n"
    "        define(copy, 'dir', v = string(v));\n"
    "        if (v !== 'auto' && v !== 'ltr' && v !== 'rtl') return construct(N, [t, copy], new.target);\n"
    "      }\n"
    "      v = options.icon; if (v !== undefined) define(copy, 'icon', string(v));\n"
    "      v = options.lang; if (v !== undefined) define(copy, 'lang', string(v));\n"
    "      v = options.silent; if (v !== undefined) define(copy, 'silent', v === null ? null : !!v);\n"
    "      v = options.tag; if (v !== undefined) define(copy, 'tag', string(v));\n"
    "    }\n"
    "    var n = construct(N, arguments.length > 1 ? [t, copy] : [t], new.target);\n"
    "    announce(t, copy && typeof copy === 'object' ? copy : null);\n"
    "    return n;\n"
    "  }\n"
    "  setPrototypeOf(Notification, N);\n"
    "  defineProperty(Notification, 'prototype', { value: N.prototype, writable: false });\n"
    "  defineProperty(N.prototype, 'constructor', { value: Notification, writable: true, enumerable: false, configurable: true });\n"
    "  defineProperty(window, 'Notification', { value: Notification, writable: true, configurable: true, enumerable: false });\n"
    "})();\n";
// (called with the application's picture origins, appPictureOrigins())
static const char appNotificationBridge[] =
    "(function (pictureOrigins) {\n"
    "  'use strict';\n"
    "  var handlers = window.webkit && window.webkit.messageHandlers;\n"
    "  var handler = handlers && handlers.kwpeNotification;\n"
    "  if (!handler) return;\n"
    "  var apply = Reflect.apply, postMessage = handler.postMessage, parse = JSON.parse, isArray = Array.isArray,\n"
    "      URL_ = URL, Image_ = Image, setTimeout_ = setTimeout, min = Math.min, max = Math.max, round = Math.round,\n"
    "      fetch_ = fetch, AbortController_ = AbortController, create = Object.create,\n"
    "      createObjectURL = URL.createObjectURL, revokeObjectURL = URL.revokeObjectURL, Blob_ = Blob,\n"
    "      getReader = ReadableStream.prototype.getReader, readChunk = ReadableStreamDefaultReader.prototype.read,\n"
    "      fromCharCode = String.fromCharCode, btoa_ = btoa, doc = document,\n"
    "      createElement = Document.prototype.createElement,\n"
    "      getContext = HTMLCanvasElement.prototype.getContext,\n"
    "      drawImage = CanvasRenderingContext2D.prototype.drawImage,\n"
    "      getImageData = CanvasRenderingContext2D.prototype.getImageData,\n"
    "      subarray = Uint8ClampedArray.prototype.subarray,\n"
    "      origin = location.origin, size = 96, maxBytes = 4194304, fetching = 0, queue = [], serial = 0, random = new Uint32Array(4);\n"
    "  crypto.getRandomValues(random);\n"
    "  var token = '';\n"
    "  for (var r = 0; r < random.length; ++r) token += ('0000000' + random[r].toString(16)).slice(-8);\n"
    "  function post(message) { message.token = token; apply(postMessage, handler, [message]); }\n"
    "  post({ kind: 'hello' });\n"
    "  window.addEventListener('pageshow', function (e) { if (e.persisted) post({ kind: 'hello' }); });\n"
    "  function where(url) { return url.protocol === 'data:' || url.protocol === 'blob:' ? url.protocol : url.protocol + '//' + url.host; }\n"
    "  function report(what, url) { post({ kind: 'debug', what: what, origin: where(url) }); }\n"
    "  function listed(url) {\n"
    "    for (var i = 0; i < pictureOrigins.length; ++i) {\n"
    "      var o = pictureOrigins[i];\n"
    "      if (o.scheme !== url.protocol) continue;\n"
    "      if (o.host === url.host || (o.host.charAt(0) === '.' && url.host.length > o.host.length &&\n"
    "          url.host.slice(-o.host.length) === o.host)) return true;\n"
    "    }\n"
    "    return false;\n"
    "  }\n"
    "  function source(icon) {\n"
    "    try {\n"
    "      var url = new URL_(icon, doc.baseURI);\n"
    "      if (url.protocol === 'data:' || url.protocol === 'blob:')\n"
    "        return { href: url.href, url: url, fetch: false, cors: false };\n"
    "      if (url.origin === origin) return { href: url.href, url: url, fetch: true, cors: false };\n"
    "      if (listed(url)) return { href: url.href, url: url, fetch: true, cors: true };\n"
    "      report('refused', url);\n"
    "    } catch (e) {}\n"
    "    return null;\n"
    "  }\n"
    "  function announce(title, body, tag, icon) {\n"
    "    var src = icon ? source(icon) : null, s = ++serial;\n"
    "    post({ kind: 'pending', serial: s, title: title, body: body, tag: tag, picture: !!src });\n"
    "    if (!src) return;\n"
    "    var done = false, started = false, img = new Image_(), abort = new AbortController_(), blobURL = null;\n"
    "    function finish(w, h, pixels, failure) {\n"
    "      if (done) return;\n"
    "      done = true;\n"
    "      try { abort.abort(); } catch (e) {}\n"
    "      if (blobURL) { try { apply(revokeObjectURL, URL_, [blobURL]); } catch (e) {} blobURL = null; }\n"
    "      if (started) { started = false; --fetching; fetchNext(); }\n"
    "      if (failure) report(failure, src.url);\n"
    "      post({ kind: 'icon', serial: s, width: w, height: h, pixels: pixels });\n"
    "    }\n"
    "    img.onload = function () {\n"
    "      try {\n"
    "        var w = img.naturalWidth, h = img.naturalHeight;\n"
    "        if (!w || !h) { finish(0, 0, '', 'failed'); return; }\n"
    "        var k = min(1, size / max(w, h)), c = apply(createElement, doc, ['canvas']);\n"
    "        w = c.width = min(size, max(1, round(w * k)));\n"
    "        h = c.height = min(size, max(1, round(h * k)));\n"
    "        var g = apply(getContext, c, ['2d']);\n"
    "        apply(drawImage, g, [img, 0, 0, w, h]);\n"
    "        var data = apply(getImageData, g, [0, 0, w, h]).data, bytes = '';\n"
    "        for (var i = 0; i < data.length; i += 8192)\n"
    "          bytes += apply(fromCharCode, null, apply(subarray, data, [i, i + 8192]));\n"
    "        finish(w, h, btoa_(bytes));\n"
    "      } catch (e) { finish(0, 0, '', 'failed'); }\n"
    "    };\n"
    "    img.onerror = function () { finish(0, 0, '', 'failed'); };\n"
    "    setTimeout_(function () { finish(0, 0, '', 'timed out'); }, 1500);\n"
    "    if (!src.fetch) { img.src = src.href; return; }\n"
    "    var init = create(null);\n"
    "    init.mode = src.cors ? 'cors' : 'same-origin';\n"
    "    init.credentials = src.cors ? 'omit' : 'same-origin';\n"
    "    init.redirect = src.cors ? 'error' : 'follow';\n"
    "    init.signal = abort.signal;\n"
    "    var job = create(null);\n"
    "    job.waits = function () { return !done; };\n"
    "    job.start = function () { started = true; ++fetching; start(); };\n"
    "    queue.push(job);\n"
    "    fetchNext();\n"
    "    function start() {\n"
    "      fetch_(src.href, init).then(function (response) {\n"
    "        if (done) return null;\n"
    "        var to = new URL_(response.url);\n"
    "        if (!response.ok || (src.cors ? response.redirected || response.type !== 'cors' || !listed(to)\n"
    "                                      : response.type !== 'basic' || to.origin !== origin)) {\n"
    "          finish(0, 0, '', 'failed');\n"
    "          return null;\n"
    "        }\n"
    "        return read(response);\n"
    "      }).then(function (blob) {\n"
    "        if (done || !blob) return;\n"
    "        blobURL = apply(createObjectURL, URL_, [blob]);\n"
    "        img.src = blobURL;\n"
    "      }).catch(function () { finish(0, 0, '', 'failed'); });\n"
    "    }\n"
    "    // the body as it comes, at most maxBytes: more cancels the fetch\n"
    "    // at once (finish() aborts it), so does the timeout\n"
    "    function read(response) {\n"
    "      var length = +response.headers.get('Content-Length'), body = response.body;\n"
    "      if (length > maxBytes || !body) { finish(0, 0, '', 'failed'); return null; }\n"
    "      var reader = apply(getReader, body, []), chunks = [], total = 0, options = create(null);\n"
    "      options.type = response.headers.get('Content-Type') || '';\n"
    "      function next() {\n"
    "        return apply(readChunk, reader, []).then(function (chunk) {\n"
    "          if (done) return null;\n"
    "          if (chunk.done) return new Blob_(chunks, options);\n"
    "          total += chunk.value.byteLength;\n"
    "          if (total > maxBytes) { chunks = []; finish(0, 0, '', 'failed'); return null; }\n"
    "          chunks.push(chunk.value);\n"
    "          return next();\n"
    "        });\n"
    "      }\n"
    "      return next();\n"
    "    }\n"
    "  }\n"
    "  // (at most 2 fetches at a time, the others wait their turn or time out)\n"
    "  function fetchNext() {\n"
    "    while (fetching < 2 && queue.length) {\n"
    "      var job = queue.shift();\n"
    "      if (job.waits()) job.start();\n"
    "    }\n"
    "  }\n"
    "  doc.addEventListener('kwpe-notification', function (e) {\n"
    "    try {\n"
    "      var a = e.detail;\n"
    "      if (e.target !== doc || typeof a !== 'string' || a.length > 1048576) return;\n"
    "      a = parse(a);\n"
    "      if (!isArray(a) || a.length !== 4) return;\n"
    "      for (var i = 0; i < 4; ++i) if (typeof a[i] !== 'string' || a[i].length > (i < 3 ? 4096 : 524288)) return;\n"
    "      announce(a[0], a[1], a[2], a[3]);\n"
    "    } catch (x) {}\n"
    "  }, true);\n"
    "})";
// The picture: at most this size each way (the script's size).
static const int appNotificationIconMax = 96;

// The application's picture origins as the bridge's argument: a JSON
// array of { scheme: "https:", host: "pps.example.net" | ".example.net"
// (any subdomain) }.  Only "http(s)://HOST[:PORT]" and
// "http(s)://*.DOMAIN[:PORT]" with host names of letters, digits, dots
// and hyphens are taken.
static QCString appPictureOrigins( const QStringList& origins )
{
    QCString json = "[";
    QRegExp pattern( "^(https?):\\/\\/(\\*\\.)?([a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*)(:[0-9]{1,5})?$" );
    for ( QStringList::ConstIterator it = origins.begin(); it != origins.end(); ++it ) {
        QString origin = ( *it ).stripWhiteSpace().lower();
        if ( pattern.search( origin ) != 0 ) {
            fprintf( stderr, "kwpe: app: picture origin %s ignored\n", origin.latin1() );
            continue;
        }
        QString host = ( pattern.cap( 2 ).isEmpty() ? QString::null : QString::fromLatin1( "." ) ) + pattern.cap( 3 ) + pattern.cap( 7 );
        if ( json.length() > 1 )
            json += ",";
        json += QString( "{\"scheme\":\"%1:\",\"host\":\"%2\"}" ).arg( pattern.cap( 1 ) ).arg( host ).latin1();
    }
    return json + "]";
}

struct KWPEMailDocument
{
    QString dir;        // the message's directory (no trailing slash)
    QString mainFile;   // the host's document in it
    bool external;      // remote images, styles, fonts allowed
    const QWidget* owner;   // the widget showing it: only its view reads it
};
typedef QMap<QString, KWPEMailDocument> KWPEMailDocuments;
static KWPEMailDocuments* s_mailDocuments = 0;   // host name -> message
static unsigned s_mailSerial = 0;

// The content filters of the mail views (fourth layer): render-only
// blocks every load but our scheme and data:, external also lets remote
// images, style sheets and fonts through.  Compiled asynchronously when
// the first mail view is made: views wait for them.
enum MailFilterState { MailFiltersPending, MailFiltersReady, MailFiltersFailed };
static MailFilterState s_mailFilterState = MailFiltersPending;
static WebKitUserContentFilter* s_mailFilter[2] = { 0, 0 };   // [external]
static int s_mailFiltersToCompile = 0;
static QValueList< QGuardedPtr<KWPEWidget> >* s_mailWaiting = 0;

static const char mailFilterRenderOnly[] =
    "[{\"trigger\":{\"url-filter\":\".*\"},\"action\":{\"type\":\"block\"}},"
    "{\"trigger\":{\"url-filter\":\"^kwpemail:\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^data:\"},\"action\":{\"type\":\"ignore-previous-rules\"}}]";
static const char mailFilterExternal[] =
    "[{\"trigger\":{\"url-filter\":\".*\"},\"action\":{\"type\":\"block\"}},"
    "{\"trigger\":{\"url-filter\":\"^kwpemail:\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^data:\"},\"action\":{\"type\":\"ignore-previous-rules\"}},"
    "{\"trigger\":{\"url-filter\":\"^https?:\",\"resource-type\":[\"image\",\"style-sheet\",\"font\"]},"
    "\"action\":{\"type\":\"ignore-previous-rules\"}}]";

// tests/render-only only (its own build of the module, never installed:
// KWPE_MAIL_TESTING): KWPE_MAIL_TEST_DISABLE_LAYERS=sanitizer,settings,
// csp,filter,proxy switches layers off, to show what the others hold
// without them.  The installed module has no such switch.
#ifdef KWPE_MAIL_TESTING
static bool mailLayerDisabled( const char* layer )
{
    static QStringList* disabled = 0;
    if ( !disabled ) {
        disabled = new QStringList( QStringList::split( ',', QString::fromLatin1( getenv( "KWPE_MAIL_TEST_DISABLE_LAYERS" ) ) ) );
        if ( !disabled->isEmpty() )
            fprintf( stderr, "kwpe: mail: TESTING: layers switched off: %s\n", disabled->join( "," ).latin1() );
    }
    return disabled->contains( QString::fromLatin1( layer ) );
}
#else
static inline bool mailLayerDisabled( const char* )
{
    return false;
}
#endif

static QString realFilePath( const QString& path )
{
    char* real = realpath( QFile::encodeName( path ).data(), 0 );
    if ( !real )
        return QString::null;
    QString result = QFile::decodeName( real );
    free( real );
    return result;
}

// Images of KDE's icon themes and of KMail's own pictures, which the
// host's document uses (absolute paths).  Not $KDEHOME/share/apps in
// general: KMail keeps its mail there.
static bool isAllowedResource( const QString& file )
{
    static QStringList* dirs = 0;
    if ( !dirs ) {
        dirs = new QStringList;
        QStringList candidates = KGlobal::dirs()->resourceDirs( "icon" );
        candidates += KGlobal::dirs()->findDirs( "data", "kmail/pics/" );
        candidates += KGlobal::dirs()->findDirs( "data", "libkdepim/pics/" );
        // KMail's about page
        candidates += KGlobal::dirs()->findDirs( "data", "kmail/about/" );
        candidates += KGlobal::dirs()->findDirs( "data", "libkdepim/about/" );
        for ( QStringList::ConstIterator it = candidates.begin(); it != candidates.end(); ++it ) {
            QString real = realFilePath( *it );
            if ( !real.isEmpty() )
                dirs->append( real + "/" );
        }
    }
    static const char* const extensions[] = { ".png", ".jpg", ".jpeg", ".gif", ".svg", ".svgz", ".xpm" };
    bool image = false;
    for ( unsigned i = 0; i < sizeof( extensions ) / sizeof( extensions[0] ) && !image; ++i )
        image = file.endsWith( extensions[i] );
    if ( !image )
        return false;
    for ( QStringList::ConstIterator it = dirs->begin(); it != dirs->end(); ++it )
        if ( file.startsWith( *it ) )
            return true;
    return false;
}

static QCString mailPolicy( bool hostDocument, bool external )
{
    if ( hostDocument )
        // KMail's headers, attachment list, text parts; the HTML parts in
        // frames of the same message.
        return "default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; font-src 'self' data:; "
               // The frames' own navigations (a link clicked in a message)
               // must get as far as WebKit's policy decision, which hands
               // them to the host: frame-src of this document applies.
               "frame-src 'self' http: https: mailto: ftp: news: kmail:; script-src 'none'; object-src 'none'; "
               "form-action 'none'; base-uri 'none'";
    // Message content.  The sandbox also holds if the host forgot the
    // iframe's: no scripts, forms, plugins, automatic features; links may
    // open windows and the top frame on a click (WebKit asks us first).
    // (<link rel=dns-prefetch>, preconnect: WebKit does not check them
    // against this policy; the sanitizer, the content filter and the
    // session's proxy are what stop them.)
    QCString remote = external ? QCString( " http: https:" ) : QCString();
    return "default-src 'none'; img-src data:" + remote + "; style-src 'unsafe-inline'" + remote
           + "; font-src data:" + remote + "; script-src 'none'; object-src 'none'; frame-src 'none'; "
             "child-src 'none'; worker-src 'none'; connect-src 'none'; manifest-src 'none'; media-src 'none'; "
             "form-action 'none'; base-uri 'none'"
             "; sandbox allow-same-origin allow-popups allow-popups-to-escape-sandbox "
             "allow-top-navigation-by-user-activation";
}

static void finishMailRequest( WebKitURISchemeRequest* request, const char* message )
{
    GError* error = g_error_new_literal( G_IO_ERROR, G_IO_ERROR_NOT_FOUND, message );
    webkit_uri_scheme_request_finish_error( request, error );
    g_error_free( error );
}

static void onMailRequest( WebKitURISchemeRequest* request, gpointer )
{
    KURL url( QString::fromUtf8( webkit_uri_scheme_request_get_uri( request ) ) );
    KWPEMailDocuments::ConstIterator it = s_mailDocuments ? s_mailDocuments->find( url.host() ) : KWPEMailDocuments::ConstIterator();
    if ( !s_mailDocuments || it == s_mailDocuments->end() ) {
        finishMailRequest( request, "no such message" );
        return;
    }
    const KWPEMailDocument& doc = it.data();
    // Only the view of the widget that shows the message reads it.
    WebKitWebView* requester = webkit_uri_scheme_request_get_web_view( request );
    if ( !requester || g_object_get_data( G_OBJECT( requester ), widgetKey ) != doc.owner ) {
        finishMailRequest( request, "not this view's message" );
        return;
    }
    QString file = realFilePath( QDir::cleanDirPath( url.path() ) );
    bool inMessage = !file.isEmpty() && file.startsWith( doc.dir + "/" ) && file.find( '/', doc.dir.length() + 1 ) < 0;
    if ( !inMessage && ( file.isEmpty() || !isAllowedResource( file ) ) ) {
        if ( getenv( "KWPE_DEBUG" ) )
            fprintf( stderr, "kwpe: mail: refused %s\n", url.url().utf8().data() );
        finishMailRequest( request, "not part of the message" );
        return;
    }
    QFile f( file );
    if ( !f.open( IO_ReadOnly ) || f.size() > 256 * 1024 * 1024 ) {
        finishMailRequest( request, "cannot read" );
        return;
    }
    QByteArray data = f.readAll();
    bool html = inMessage && file.endsWith( ".html" );
    bool hostDocument = file == doc.mainFile;
    // HTML files outside the message (none: isAllowedResource() takes
    // images only) would be served as what they are named.
    QCString type = html ? QCString( "text/html; charset=utf-8" ) : QCString( KMimeType::findByPath( file, 0, true )->name().latin1() );
    if ( type == "text/html" || type == "application/xhtml+xml" || type.isEmpty() )
        type = "application/octet-stream";   // never a document by another name

    GBytes* bytes;
    if ( html && !mailLayerDisabled( "sanitizer" ) ) {
        // First layer: the document rebuilt from a white list.
        std::string clean = KWPEMailSanitizer::sanitizeHtml( std::string( data.data(), data.size() ),
                                                             hostDocument ? KWPEMailSanitizer::Host : KWPEMailSanitizer::Content,
                                                             doc.external );
        bytes = g_bytes_new( clean.data(), clean.size() );
    } else {
        bytes = g_bytes_new( data.data(), data.size() );
    }
    gsize size = g_bytes_get_size( bytes );
    GInputStream* stream = g_memory_input_stream_new_from_bytes( bytes );
    g_bytes_unref( bytes );
    WebKitURISchemeResponse* response = webkit_uri_scheme_response_new( stream, size );
    g_object_unref( stream );
    webkit_uri_scheme_response_set_content_type( response, type.data() );
    SoupMessageHeaders* headers = soup_message_headers_new( SOUP_MESSAGE_HEADERS_RESPONSE );
    soup_message_headers_append( headers, "Content-Type", type.data() );
    if ( !mailLayerDisabled( "csp" ) )
        soup_message_headers_append( headers, "Content-Security-Policy", mailPolicy( hostDocument, doc.external ).data() );
    soup_message_headers_append( headers, "Referrer-Policy", "no-referrer" );
    soup_message_headers_append( headers, "X-Content-Type-Options", "nosniff" );
    soup_message_headers_append( headers, "X-DNS-Prefetch-Control", "off" );
    webkit_uri_scheme_response_set_http_headers( response, headers );   // takes them
    webkit_uri_scheme_request_finish_with_response( request, response );
    g_object_unref( response );
}

// WebKit must not save anything a message links to by itself.
static void onMailDownloadStarted( WebKitNetworkSession*, WebKitDownload* download, gpointer )
{
    webkit_download_cancel( download );
}

// The render-only session's proxy: a port of our own on the loopback
// interface that takes connections and closes them at once.  Nothing
// gets there unless every other layer failed: each connection is
// reported (a tripwire), none goes anywhere.
static gboolean onMailProxyConnection( GSocketService*, GSocketConnection* connection, GObject*, gpointer )
{
    fprintf( stderr, "kwpe: mail: BLOCKED a network connection of a render-only message (closed proxy)\n" );
    g_io_stream_close( G_IO_STREAM( connection ), 0, 0 );
    return TRUE;   // handled
}

static guint16 startMailProxy()
{
    GSocketService* service = g_socket_service_new();
    GInetAddress* loopback = g_inet_address_new_loopback( G_SOCKET_FAMILY_IPV4 );
    GSocketAddress* address = g_inet_socket_address_new( loopback, 0 );
    g_object_unref( loopback );
    GSocketAddress* effective = 0;
    GError* error = 0;
    guint16 port = 0;
    if ( g_socket_listener_add_address( G_SOCKET_LISTENER( service ), address, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                        0, &effective, &error ) ) {
        port = g_inet_socket_address_get_port( G_INET_SOCKET_ADDRESS( effective ) );
        g_object_unref( effective );
        g_signal_connect( service, "incoming", G_CALLBACK( onMailProxyConnection ), 0 );
        g_socket_service_start( service );
    } else {
        fprintf( stderr, "kwpe: mail: no proxy port: %s\n", error->message );
        g_error_free( error );
        g_object_unref( service );
    }
    g_object_unref( address );
    return port;   // the service lives as long as the process
}

static void onMailFilterSaved( GObject* store, GAsyncResult* result, gpointer data )
{
    long external = long( data );
    GError* error = 0;
    s_mailFilter[external] = webkit_user_content_filter_store_save_finish( WEBKIT_USER_CONTENT_FILTER_STORE( store ), result, &error );
    if ( !s_mailFilter[external] ) {
        fprintf( stderr, "kwpe: mail: the content filter could not be compiled: %s; no message is shown\n",
                 error ? error->message : "?" );
        if ( error )
            g_error_free( error );
        s_mailFilterState = MailFiltersFailed;
    }
    if ( --s_mailFiltersToCompile > 0 )
        return;
    if ( s_mailFilterState == MailFiltersPending )
        s_mailFilterState = MailFiltersReady;
    QValueList< QGuardedPtr<KWPEWidget> > waiting = *s_mailWaiting;
    s_mailWaiting->clear();
    for ( QValueList< QGuardedPtr<KWPEWidget> >::ConstIterator it = waiting.begin(); it != waiting.end(); ++it )
        if ( *it )
            QTimer::singleShot( 0, *it, SLOT( loadPendingMail() ) );
}

static void compileMailFilters()
{
    // Compiled filters are kept by WebKit in a store (a directory); ours
    // has nothing but these two.
    QString dir = KGlobal::dirs()->saveLocation( "data", "kwpepart/mailfilters/" );
    WebKitUserContentFilterStore* store = webkit_user_content_filter_store_new( QFile::encodeName( dir ).data() );
    s_mailFiltersToCompile = 2;
    const char* sources[2] = { mailFilterRenderOnly, mailFilterExternal };
    const char* ids[2] = { "kwpe-mail-render-only", "kwpe-mail-external" };
    for ( long i = 0; i < 2; ++i ) {
        GBytes* bytes = g_bytes_new_static( sources[i], strlen( sources[i] ) );
        webkit_user_content_filter_store_save( store, ids[i], bytes, 0, onMailFilterSaved, (gpointer)i );
        g_bytes_unref( bytes );
    }
    g_object_unref( store );
}

// @p identifier as WebKitFeature has it: the preference's name without a
// trailing "Enabled" (BeaconAPIEnabled -> BeaconAPI).  A feature WebKit
// does not have is reported: a switch that silently does nothing is what
// happened here before.
static void setFeature( WebKitSettings* settings, WebKitFeatureList* features, const char* identifier, bool enabled )
{
    for ( gsize i = 0; i < webkit_feature_list_get_length( features ); ++i ) {
        WebKitFeature* feature = webkit_feature_list_get( features, i );
        if ( strcmp( webkit_feature_get_identifier( feature ), identifier ) == 0 ) {
            webkit_settings_set_feature_enabled( settings, feature, enabled );
            return;
        }
    }
    fprintf( stderr, "kwpe: mail: WebKit has no feature %s (not switched off)\n", identifier );
}

static bool ensureMailWebKit()
{
    if ( s_mailContext )
        return true;
    if ( !ensureDisplay() )
        return false;

    s_mailDocuments = new KWPEMailDocuments;
    s_mailWaiting = new QValueList< QGuardedPtr<KWPEWidget> >;
    compileMailFilters();

    // Fourth layer: whatever a render-only message would load goes to a
    // proxy that is ours and closes every connection (no name lookups
    // either: an HTTP proxy is asked for the host, the proxy's address is
    // a number).  Name lookups WebKit makes by itself (DNS prefetching)
    // do not go through the proxy: the content filter stops them.
    s_mailSession = webkit_network_session_new_ephemeral();
    g_signal_connect( s_mailSession, "download-started", G_CALLBACK( onMailDownloadStarted ), 0 );
    webkit_network_session_set_tls_errors_policy( s_mailSession, WEBKIT_TLS_ERRORS_POLICY_FAIL );
    if ( !mailLayerDisabled( "proxy" ) ) {
        guint16 port = startMailProxy();
        QCString proxy;
        proxy.sprintf( "http://127.0.0.1:%u", port ? unsigned( port ) : 9u );
        WebKitNetworkProxySettings* proxySettings = webkit_network_proxy_settings_new( proxy.data(), 0 );
        webkit_network_session_set_proxy_settings( s_mailSession, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxySettings );
        webkit_network_proxy_settings_free( proxySettings );
    }

    s_mailContext = webkit_web_context_new();
    // No memory cache of resources a shown document no longer uses, no
    // page cache.
    webkit_web_context_set_cache_model( s_mailContext, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER );
    webkit_web_context_register_uri_scheme( s_mailContext, mailScheme, onMailRequest, 0, 0 );

    // Second layer: WebKit's switches.
    s_mailSettings = webkit_settings_new();
    // Our own script world runs (frame heights, copy, scrolling); the
    // page's scripts, event handler attributes and javascript: URLs not
    // (the parser drops them, Document::allowsContentJavaScript()).
    webkit_settings_set_enable_javascript( s_mailSettings, TRUE );
    webkit_settings_set_enable_javascript_markup( s_mailSettings, mailLayerDisabled( "settings" ) );
    webkit_settings_set_javascript_can_open_windows_automatically( s_mailSettings, FALSE );
    webkit_settings_set_javascript_can_access_clipboard( s_mailSettings, FALSE );
    webkit_settings_set_allow_file_access_from_file_urls( s_mailSettings, FALSE );
    webkit_settings_set_allow_universal_access_from_file_urls( s_mailSettings, FALSE );
    webkit_settings_set_allow_top_navigation_to_data_urls( s_mailSettings, FALSE );
    webkit_settings_set_disable_web_security( s_mailSettings, FALSE );
    webkit_settings_set_enable_html5_local_storage( s_mailSettings, FALSE );
    webkit_settings_set_enable_html5_database( s_mailSettings, FALSE );
    webkit_settings_set_enable_page_cache( s_mailSettings, FALSE );
    webkit_settings_set_enable_media( s_mailSettings, FALSE );
    webkit_settings_set_enable_mediasource( s_mailSettings, FALSE );
    webkit_settings_set_enable_media_stream( s_mailSettings, FALSE );
    webkit_settings_set_enable_media_capabilities( s_mailSettings, FALSE );
    webkit_settings_set_enable_encrypted_media( s_mailSettings, FALSE );
    webkit_settings_set_media_playback_requires_user_gesture( s_mailSettings, TRUE );
    webkit_settings_set_enable_webrtc( s_mailSettings, FALSE );
    webkit_settings_set_enable_webaudio( s_mailSettings, FALSE );
    webkit_settings_set_enable_webgl( s_mailSettings, FALSE );
    webkit_settings_set_enable_fullscreen( s_mailSettings, FALSE );
    webkit_settings_set_allow_modal_dialogs( s_mailSettings, FALSE );
    webkit_settings_set_enable_2d_canvas_acceleration( s_mailSettings, FALSE );
    webkit_settings_set_enable_site_specific_quirks( s_mailSettings, FALSE );
    webkit_settings_set_enable_developer_extras( s_mailSettings, envFlag( "KWPE_DEVELOPER_EXTRAS" ) );
    // (enable-dns-prefetching and enable-hyperlink-auditing do nothing in
    // WebKit 2.54.)  What loads by itself, WebKit's features (all exposed
    // boolean preferences; LinkPreconnect is on by default on GLib; WebXR
    // is not built into WPE WebKit 2.54):
    static const char* const offFeatures[] = {
        "LinkPreconnect", "LinkPreconnectEarlyHints", "LinkPrefetch", "LinkPreload", "MediaPreloading",
        "SpeculationRulesPrefetch", "BeaconAPI", "DownloadAttribute", "WebSocket", "WebTransport", "ServiceWorkers",
        "SharedWorker", "IndexedDBAPI", "CacheAPI", "Notifications", "NotificationEvent", "PushAPI",
        "GeolocationAPI", "Gamepads", "AsyncClipboardAPI", "BroadcastChannel", 0
    };
    if ( !mailLayerDisabled( "settings" ) ) {
        WebKitFeatureList* features = webkit_settings_get_all_features();
        for ( const char* const* f = offFeatures; *f; ++f )
            setFeature( s_mailSettings, features, *f, false );
        webkit_feature_list_unref( features );
    }
    KWPEWidget::applySettings();
    return true;
}

// ---------------------------------------------------------------------
// app mode (an application around one web site, e.g. kwhatsapp)
//
// One WebKit context, network session and settings per data directory:
// the application's own web processes, cookies, website data (its login
// stays across restarts), separate from Konqueror's and KMail's.  The
// notification permissions the user gave are kept there too (WebKit asks
// for them when a web process starts).

struct KWPEAppWebKit
{
    QString dataDir;
    WebKitWebContext* context;
    WebKitNetworkSession* session;
    WebKitSettings* settings;
    QStringList allowedNotifications;   // origins (scheme://host[:port])
    QStringList deniedNotifications;
};
typedef QMap<QString, KWPEAppWebKit*> KWPEAppWebKits;
static KWPEAppWebKits* s_apps = 0;   // data directory -> its WebKit

static const char appPermissionsFile[] = "kwpepart-permissions";

static void loadAppPermissions( KWPEAppWebKit* app )
{
    KSimpleConfig config( app->dataDir + appPermissionsFile, true );
    config.setGroup( "Notifications" );
    app->allowedNotifications = config.readListEntry( "Allowed" );
    app->deniedNotifications = config.readListEntry( "Denied" );
}

static void saveAppPermissions( KWPEAppWebKit* app )
{
    KSimpleConfig config( app->dataDir + appPermissionsFile );
    config.setGroup( "Notifications" );
    config.writeEntry( "Allowed", app->allowedNotifications );
    config.writeEntry( "Denied", app->deniedNotifications );
    config.sync();
}

static GList* securityOrigins( const QStringList& origins )
{
    GList* list = 0;
    for ( QStringList::ConstIterator it = origins.begin(); it != origins.end(); ++it )
        list = g_list_prepend( list, webkit_security_origin_new_for_uri( ( *it ).utf8().data() ) );
    return list;
}

// WebKit asks when it starts a web process: Notification.permission
// is "granted" at once for the origins the user allowed before.
static void onInitializeNotificationPermissions( WebKitWebContext* context, gpointer data )
{
    KWPEAppWebKit* app = static_cast<KWPEAppWebKit*>( data );
    GList* allowed = securityOrigins( app->allowedNotifications );
    GList* denied = securityOrigins( app->deniedNotifications );
    webkit_web_context_initialize_notification_permissions( context, allowed, denied );
    g_list_free_full( allowed, (GDestroyNotify)webkit_security_origin_unref );
    g_list_free_full( denied, (GDestroyNotify)webkit_security_origin_unref );
}

static QString originOf( const char* uri )
{
    if ( !uri )
        return QString::null;
    WebKitSecurityOrigin* origin = webkit_security_origin_new_for_uri( uri );
    char* text = webkit_security_origin_to_string( origin );
    QString result = text ? QString::fromUtf8( text ) : QString::null;
    g_free( text );
    webkit_security_origin_unref( origin );
    return result;
}

// Origins as WebKit writes them (scheme://host[:port]), each once; http
// and https only, anything else (a hand-edited file) dropped.
static QStringList canonicalOrigins( const QStringList& origins )
{
    QStringList result;
    for ( QStringList::ConstIterator it = origins.begin(); it != origins.end(); ++it ) {
        QString origin = originOf( ( *it ).stripWhiteSpace().utf8().data() );
        if ( ( origin.startsWith( "http://" ) || origin.startsWith( "https://" ) ) && !result.contains( origin ) )
            result.append( origin );
    }
    return result;
}

static KWPEAppWebKit* ensureAppWebKit( const KWPEAppOptions& options )
{
    if ( !ensureDisplay() || options.dataDir.isEmpty() )
        return 0;
    QString dataDir = options.dataDir.endsWith( "/" ) ? options.dataDir : options.dataDir + "/";
    if ( !s_apps )
        s_apps = new KWPEAppWebKits;
    KWPEAppWebKits::ConstIterator it = s_apps->find( dataDir );
    if ( it != s_apps->end() )
        return it.data();
    if ( !KStandardDirs::makeDir( dataDir, 0700 ) && !QFile::exists( dataDir ) )
        return 0;
    QString cacheDir = options.cacheDir.isEmpty() ? dataDir + "cache/" : options.cacheDir;

    KWPEAppWebKit* app = new KWPEAppWebKit;
    app->dataDir = dataDir;
    loadAppPermissions( app );
    // One rule everywhere, WebKit's: an origin both allowed and denied is
    // denied.
    app->deniedNotifications = canonicalOrigins( app->deniedNotifications );
    app->allowedNotifications = canonicalOrigins( app->allowedNotifications );
    for ( QStringList::ConstIterator it = app->deniedNotifications.begin(); it != app->deniedNotifications.end(); ++it )
        app->allowedNotifications.remove( *it );
    if ( getenv( "KWPE_DEBUG" ) )
        fprintf( stderr, "kwpe: app: notifications allowed for [%s], denied for [%s]\n",
                 app->allowedNotifications.join( " " ).utf8().data(), app->deniedNotifications.join( " " ).utf8().data() );
    app->session = webkit_network_session_new( QFile::encodeName( dataDir ).data(), QFile::encodeName( cacheDir ).data() );
    webkit_cookie_manager_set_persistent_storage( webkit_network_session_get_cookie_manager( app->session ),
                                                  QFile::encodeName( dataDir + "cookies.sqlite" ).data(),
                                                  WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE );
    // A site that works as an application keeps its cookies (the login).
    webkit_cookie_manager_set_accept_policy( webkit_network_session_get_cookie_manager( app->session ),
                                             WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY );
    g_signal_connect( app->session, "download-started", G_CALLBACK( onDownloadStarted ), 0 );
    webkit_network_session_set_tls_errors_policy( app->session, WEBKIT_TLS_ERRORS_POLICY_FAIL );

    app->context = webkit_web_context_new();
    // One application, no back/forward page cache to keep.
    webkit_web_context_set_cache_model( app->context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER );
    g_signal_connect( app->context, "initialize-notification-permissions",
                      G_CALLBACK( onInitializeNotificationPermissions ), app );

    app->settings = webkit_settings_new();
    if ( !options.userAgent.isEmpty() )
        webkit_settings_set_user_agent( app->settings, options.userAgent.utf8().data() );
    webkit_settings_set_enable_developer_extras( app->settings, envFlag( "KWPE_DEVELOPER_EXTRAS" ) );
    webkit_settings_set_allow_file_access_from_file_urls( app->settings, FALSE );
    webkit_settings_set_allow_universal_access_from_file_urls( app->settings, FALSE );
    webkit_settings_set_javascript_can_open_windows_automatically( app->settings, FALSE );
    webkit_settings_set_javascript_can_access_clipboard( app->settings, FALSE );
    webkit_settings_set_enable_2d_canvas_acceleration( app->settings, envFlag( "KWPE_CANVAS_ACCELERATION" ) );
    s_apps->insert( dataDir, app );
    KWPEWidget::applySettings();
    return app;
}

// The heights of the message's frames (its HTML parts): their documents'
// heights, kept up to date (images, fonts, a narrower window), so the
// view scrolls as one page.  The document stays hidden (mailHideStyle)
// until its frames have their heights: WebKit keeps showing the previous
// message meanwhile (it shows a new page once it is visually non-empty
// or loaded), instead of the headers alone first.  In our script world:
// the page has none.
//
// A document whose height follows its frame's (100vh, min-height: 100%
// of a tall body) would grow without end: a growth reported right after
// ours (200 ms) that is at least as large as ours is the document
// following the frame; the frame keeps its height (the document scrolls
// inside) until the window changes or something of the document loads.
//
// The script also reports (kwpeMailState) whether the view is scrolled to
// the bottom and the selected text, for the host's synchronous questions
// (KMail's "Read On", reply to the selection).
static const char mailFrameScript[] =
    "(function() {"
    "  var frames = document.querySelectorAll('iframe'), pending = frames.length, last = null, post = null;"
    "  try { post = window.webkit.messageHandlers.kwpeMailState; } catch (e) {}"
    "  function selected(w) {"
    "    var t = w.getSelection().toString();"
    "    if (t) return t;"
    "    var f = w.document.querySelectorAll('iframe');"
    "    for (var i = 0; i < f.length; i++) {"
    "      try { t = selected(f[i].contentWindow); if (t) return t; } catch (e) {}"
    "    }"
    "    return '';"
    "  }"
    "  function report() {"
    "    if (!post) return;"
    "    var b = window.scrollY + window.innerHeight >= document.documentElement.scrollHeight - 2, s = '';"
    "    try { s = selected(window); } catch (e) {}"
    "    var key = (b ? '1' : '0') + s;"
    "    if (key === last) return;"
    "    last = key;"
    "    post.postMessage({ b: b, s: s });"
    "  }"
    "  function reveal() { document.documentElement.setAttribute('data-kwpe-ready', ''); report(); }"
    "  function fit(f, observed) {"
    "    try {"
    "      var d = f.contentDocument;"
    "      if (!d || !d.documentElement) return;"
    "      var h = Math.max(d.documentElement.scrollHeight, d.body ? d.body.scrollHeight : 0);"
    "      var cur = f.getBoundingClientRect().height, now = Date.now();"
    "      if (Math.abs(cur - h) <= 1) return;"
    "      if (h > cur) {"
    "        if (observed) {"
    "          if (f.kwpeFollows) return;"
    "          if (f.kwpeGrownAt && now - f.kwpeGrownAt < 200 && h - cur >= f.kwpeGrownBy - 1) { f.kwpeFollows = true; return; }"
    "        } else {"
    "          f.kwpeFollows = false;"
    "        }"
    "        f.kwpeGrownAt = now;"
    "        f.kwpeGrownBy = h - cur;"
    "      }"
    "      f.style.height = Math.min(h, 1000000) + 'px';"
    "      report();"
    "    } catch (e) {}"
    "  }"
    "  function watch(f) {"
    "    if (f.kwpeWatched) return false;"
    "    var d = f.contentDocument;"
    "    if (!d || !d.documentElement || d.readyState != 'complete' || d.URL == 'about:blank') return false;"
    "    f.kwpeWatched = true;"
    "    fit(f, false);"
    "    try { new ResizeObserver(function() { fit(f, true); }).observe(d.documentElement); } catch (e) {}"
    "    d.addEventListener('load', function() { fit(f, false); }, true);"
    "    d.addEventListener('selectionchange', report);"
    "    if (--pending == 0) reveal();"
    "    return true;"
    "  }"
    "  for (var i = 0; i < frames.length; i++) {"
    "    (function(f) {"
    "      f.addEventListener('load', function() { watch(f); fit(f, false); });"
    "      watch(f);"
    "    })(frames[i]);"
    "  }"
    "  if (pending <= 0) reveal();"
    "  window.addEventListener('load', function() { for (var i = 0; i < frames.length; i++) fit(frames[i], false); reveal(); });"
    "  window.addEventListener('resize', function() { for (var i = 0; i < frames.length; i++) fit(frames[i], false); report(); });"
    "  window.addEventListener('scroll', report);"
    "  document.addEventListener('selectionchange', report);"
    "  setTimeout(reveal, 1500);"
    "})();";
static const char mailHideStyle[] = "html:not([data-kwpe-ready]) { visibility: hidden !important; }";

// ---------------------------------------------------------------------
// KWPEWidgetPrivate

class KWPEWidgetPrivate : public KWPE::ViewHost
{
public:
    KWPEWidgetPrivate( KWPEWidget* widget, KWPEWidget::Mode m )
        : q( widget ), mode( m ), pendingMailExternal( false ), mailExternalSession( 0 ), viewExternal( false ),
          mailFilterAdded( false ), mailAtBottom( true ), app( 0 ), notificationHello( false ), notificationGeneration( 0 ), announcementsLost( false ), notificationSerial( 0 ), menuEditable( false ), lastReturnToApp( 0 ), ownBlank( false ), view( 0 ), wpeView( 0 ), toplevel( 0 ), contentManager( 0 ), reloadForFilters( false ), redirected( false ), loading( false ), failed( false ), ownBackForward( false ), shownItem( 0 ),
          menuSelection( false ), overEditable( false ), cancellable( 0 ), suppressDialogs( false ), insecureContent( false ), authRequest( 0 ), authSerial( 0 ), lastCrashReload( 0 ), frameCallback( 0 ), statsTimer( 0 ), loadStart( 0 ), firstPaintPending( false ), statsEnabled( false )
    {
    }

    virtual ~KWPEWidgetPrivate() {}

    virtual void setCursorName( const char* name );

    void create();
    void destroy();
    void attach();
    int tokenForItem( WebKitBackForwardListItem* item, bool* isNew );
    void forgetItems( GList* items );
    guint32 eventTime() const { return guint32( qt_x_time ); }

    static void onLoadChanged( WebKitWebView*, WebKitLoadEvent event, gpointer data );
    static gboolean onLoadFailed( WebKitWebView*, WebKitLoadEvent, gchar* uri, GError* error, gpointer data );
    static void onUriChanged( GObject*, GParamSpec*, gpointer data );
    static void onTitleChanged( GObject*, GParamSpec*, gpointer data );
    static void onProgressChanged( GObject*, GParamSpec*, gpointer data );
    static void onMouseTargetChanged( WebKitWebView*, WebKitHitTestResult* hit, guint modifiers, gpointer data );
    static gboolean onDecidePolicy( WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer data );
    gboolean decideMailPolicy( WebKitPolicyDecision* decision, WebKitPolicyDecisionType type );
    void activateLink( const char* uri );
    static void onMailState( WebKitUserContentManager*, JSCValue* value, gpointer data );
    void forgetMailHosts( bool all );
    bool isMail() const { return mode == KWPEWidget::Mail; }
    bool isApp() const { return mode == KWPEWidget::App; }
    bool isBrowser() const { return mode == KWPEWidget::Browser; }
    gboolean decideAppPolicy( WebKitPolicyDecision* decision, WebKitPolicyDecisionType type );
    bool isAppSite( const QString& host ) const;
    bool isAppDocument( const char* uri ) const;
    static gboolean onShowNotification( WebKitWebView*, WebKitNotification* notification, gpointer data );
    static void onNotificationClosed( WebKitNotification* notification, gpointer data );
    void forgetNotifications();
    static void onAppNotification( WebKitUserContentManager*, JSCValue* value, gpointer data );
    static QImage notificationImage( JSCValue* value );
    static gboolean onQueryPermissionState( WebKitWebView*, WebKitPermissionStateQuery* query, gpointer data );
    static void onDropDone( GObject* view, GAsyncResult* result, gpointer data );
    static void onBackForwardChanged( WebKitBackForwardList*, WebKitBackForwardListItem*, GList* removed, gpointer data );
    static void onFrameDisplayed( WebKitWebView*, gpointer data );
    static void onWebProcessTerminated( WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer data );
    static void onInsecureContent( WebKitWebView*, WebKitInsecureContentEvent, gpointer data );
    static gboolean onAuthenticate( WebKitWebView*, WebKitAuthenticationRequest* request, gpointer data );
    static void onAuthenticationCancelled( WebKitAuthenticationRequest*, gpointer data );
    static void onSubmitForm( WebKitWebView*, WebKitFormSubmissionRequest* request, gpointer data );
    void dropAuthentication();
    static gboolean onScriptDialog( WebKitWebView*, WebKitScriptDialog* dialog, gpointer data );
    static gboolean onRunFileChooser( WebKitWebView*, WebKitFileChooserRequest* request, gpointer data );
    void showPageDialog( QWidget* dialog );
    static gboolean onPermissionRequest( WebKitWebView*, WebKitPermissionRequest* request, gpointer data );
    static gboolean onContextMenu( WebKitWebView*, WebKitContextMenu*, WebKitHitTestResult* hit, gpointer data );
    static void onSelectionText( GObject* view, GAsyncResult* result, gpointer data );
    static void onIconURL( GObject* view, GAsyncResult* result, gpointer data );
    static void onSourceData( GObject* resource, GAsyncResult* result, gpointer data );
    void findIcon();
    static void onFoundText( WebKitFindController*, guint matchCount, gpointer data );
    static void onFailedToFindText( WebKitFindController*, gpointer data );

    KWPEWidget* q;
    KWPEWidget::Mode mode;
    QString mailHost;          // mail mode: the message shown (kwpemail://<host>/)
    QString mailMainURL;       // ... and the host's document
    QString pendingMailPath;   // waits for the content filters
    bool pendingMailExternal;
    // A message with external references: a view and a session of its
    // own, gone with the next message (nothing it stored reaches another).
    WebKitNetworkSession* mailExternalSession;
    bool viewExternal;
    bool mailFilterAdded;
    QStringList mailHosts;     // kwpemail://<host>/ of this widget, the shown one last
    bool mailAtBottom;         // reported by our script (atBottom())
    QString mailSelection;     // ... (selectedText())
    QStringList activatedLinks;   // for the host, outside WebKit's signal

    // app mode
    KWPEAppOptions appOptions;
    KWPEAppWebKit* app;
    struct PendingPermission
    {
        WebKitPermissionRequest* request;   // ref
        QString kind;
        QString origin;
    };
    QValueList<PendingPermission> pendingPermissions;   // for the host, outside WebKit's signal
    QMap<int, WebKitNotification*> notifications;   // shown by the page, not closed yet (ref)
    QValueList<int> notificationsToShow;   // for the host, outside WebKit's signal
    // When WebKit showed each of these: it waits for its picture until
    // 2 seconds after that, whatever is announced meanwhile.
    QMap<int, gint64> notificationShownAt;
    // The document generation each was shown in (notificationGeneration):
    // only an announcement of the same one is given to it.
    QMap<int, int> notificationShownIn;
    QValueList<int> notificationsEvicted;   // dropped by the limit: closed for the host
    // The pictures our script announced (appNotificationBridge), oldest
    // first (at most 20): a notification waits for its picture (see
    // notificationShownAt).
    struct NotificationIcon
    {
        int serial;      // the script's
        QString title;   // the notification it is for
        QString body;
        QString tag;
        int id;          // ... once it is shown (0: not yet)
        bool done;       // the picture came (or none will)
        QImage image;
        gint64 announced;   // g_get_monotonic_time()
        int generation;     // the document's (notificationGeneration)
        // Announced with the same title, body and tag as another one
        // when a notification took one of them: which notification is
        // whose is not known, none of them gets a picture.
        bool ambiguous;
    };
    QValueList<NotificationIcon> notificationIcons;
    // The document whose bridge's messages are taken: its token (its
    // "hello"); a commit without a hello since the load started ends it.
    QString notificationToken;
    bool notificationHello;
    // One more each time a document ends (endNotificationDocument()):
    // notifications and announcements of different ones never match.
    int notificationGeneration;
    // Announcements of this generation dropped before a notification took
    // them (the limit of 20, or 10 seconds unshown): their keys (a hash of
    // title, body and tag) until the document ends, at most 64 different
    // ones.  A notification with such a key may be the dropped one's: it
    // gets no picture.  More than 64: which ones were dropped is no longer
    // known (announcementsLost), no notification of the document gets a
    // picture any more.
    QValueList<Q_UINT64> droppedAnnouncements;
    bool announcementsLost;
    static Q_UINT64 announcementKey( const QString& title, const QString& body, const QString& tag );
    void dropAnnouncement( const NotificationIcon& icon );
    bool wasDropped( Q_UINT64 key ) const;
    void endNotificationDocument();
    int notificationSerial;
    QStringList pendingDownloads;   // new windows of blob: and data: URLs
    bool menuEditable;   // the context menu was asked for over a text field
    QString blockedURL;  // app mode: a document of another site, not shown
    QString appHome;     // app mode: the last page of its sites the host loaded
    gint64 lastReturnToApp;   // returnToApp(): not in a loop
    bool ownBlank;            // ... its empty page is committing
    WebKitWebView* view;
    WPEView* wpeView;
    WPEToplevel* toplevel;
    WebKitUserContentManager* contentManager;   // ours: the ad blocker's filters
    QString url;
    QString title;
    QString deferredURL;   // load() waits for the ad blocker's filters
    bool reloadForFilters; // the ad blocker switched for this page after it was requested
    bool redirected;       // the load was redirected (no reload for the filters)
    bool loading;
    bool failed;           // the current load failed (WebKit still sends LOAD_FINISHED)
    bool ownBackForward;   // goToHistoryToken() asked for this back/forward navigation
    WebKitBackForwardListItem* shownItem;   // back/forward item of the page shown (no reference)

    // history tokens <-> WebKit back/forward items (referenced)
    QMap<int, WebKitBackForwardListItem*> itemOfToken;
    QMap<WebKitBackForwardListItem*, int> tokenOfItem;

    // a URL of a scheme for Konqueror (reported outside WebKit's signal)
    QString externalUrl;
    // the response WebKit cannot show (reported outside its signal)
    QString unsupportedUrl;
    QString unsupportedMimeType;

    // the context menu WebKit asked for (shown outside its signal)
    QString menuLink;
    QString menuImage;
    bool menuSelection;
    bool overEditable;           // the pointer is over a text field (middle click pastes)
    GCancellable* cancellable;   // pending JavaScript calls; cancelled with the view

    bool suppressDialogs;        // "no more dialogs from this page" (until the next page)
    QValueList<WebKitScriptDialog*> quietDialogs;   // answered without asking, after WebKit's signal (ref)
    void answerQuietDialogs();
    void closePageDialogs();
    QGuardedPtr<QWidget> pageDialog;   // file chooser or script dialog (KWPEFileChooser): the page takes no input
    bool insecureContent;        // the page loaded http content (https page)
    WebKitAuthenticationRequest* authRequest;   // waiting for the password dialog (ref)
    unsigned authSerial;         // counts the requests (a new one may reuse the address of an old one)
    gint64 lastCrashReload;      // when the page was reloaded after a crash
    unsigned frameCallback;      // KWPE_STATS frame-displayed callback

    QTimer* statsTimer;
    gint64 loadStart;
    bool firstPaintPending;
    bool statsEnabled;
};

void KWPEWidgetPrivate::setCursorName( const char* name )
{
    static const struct { const char* css; Qt::CursorShape shape; } cursors[] = {
        { "default", Qt::ArrowCursor }, { "auto", Qt::ArrowCursor },
        { "pointer", Qt::PointingHandCursor }, { "context-menu", Qt::ArrowCursor },
        { "text", Qt::IbeamCursor }, { "vertical-text", Qt::IbeamCursor },
        { "wait", Qt::WaitCursor }, { "progress", Qt::BusyCursor },
        { "crosshair", Qt::CrossCursor }, { "cell", Qt::CrossCursor },
        { "move", Qt::SizeAllCursor }, { "all-scroll", Qt::SizeAllCursor },
        { "grab", Qt::SizeAllCursor }, { "grabbing", Qt::SizeAllCursor },
        { "not-allowed", Qt::ForbiddenCursor }, { "no-drop", Qt::ForbiddenCursor },
        { "help", Qt::WhatsThisCursor }, { "copy", Qt::ArrowCursor }, { "alias", Qt::ArrowCursor },
        { "ew-resize", Qt::SizeHorCursor }, { "col-resize", Qt::SplitHCursor },
        { "e-resize", Qt::SizeHorCursor }, { "w-resize", Qt::SizeHorCursor },
        { "ns-resize", Qt::SizeVerCursor }, { "row-resize", Qt::SplitVCursor },
        { "n-resize", Qt::SizeVerCursor }, { "s-resize", Qt::SizeVerCursor },
        { "nesw-resize", Qt::SizeBDiagCursor }, { "ne-resize", Qt::SizeBDiagCursor }, { "sw-resize", Qt::SizeBDiagCursor },
        { "nwse-resize", Qt::SizeFDiagCursor }, { "nw-resize", Qt::SizeFDiagCursor }, { "se-resize", Qt::SizeFDiagCursor },
        { "none", Qt::BlankCursor },
    };
    Qt::CursorShape shape = Qt::ArrowCursor;
    for ( unsigned i = 0; i < sizeof( cursors ) / sizeof( cursors[0] ); ++i )
        if ( name && strcmp( name, cursors[i].css ) == 0 ) {
            shape = cursors[i].shape;
            break;
        }
    if ( q->cursor().shape() != shape )
        q->setCursor( QCursor( shape ) );
}

void KWPEWidgetPrivate::create()
{
    if ( isApp() ) {
        app = ensureAppWebKit( appOptions );
        if ( !app )
            return;
    } else if ( isMail() ? !ensureMailWebKit() : !ensureWebKit() ) {
        return;
    }

    // A content manager per view: the ad blocker's filters are attached
    // to it or not, depending on the site the view shows.  Mail: our
    // script for the message's frames, no ad blocker (nothing remote
    // unless the user wants it for a message).
    contentManager = webkit_user_content_manager_new();
    if ( isMail() ) {
        WebKitUserScript* script = webkit_user_script_new_for_world( mailFrameScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                     WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, mailWorld, 0, 0 );
        webkit_user_content_manager_add_script( contentManager, script );
        webkit_user_script_unref( script );
        WebKitUserStyleSheet* style = webkit_user_style_sheet_new( mailHideStyle, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                   WEBKIT_USER_STYLE_LEVEL_USER, 0, 0 );
        webkit_user_content_manager_add_style_sheet( contentManager, style );
        webkit_user_style_sheet_unref( style );
        g_signal_connect( contentManager, ( QCString( "script-message-received::" ) + mailStateHandler ).data(),
                          G_CALLBACK( onMailState ), this );
        webkit_user_content_manager_register_script_message_handler( contentManager, mailStateHandler, mailWorld );
        // Before anything is loaded (see loadPendingMail()).
        mailFilterAdded = s_mailFilterState == MailFiltersReady;
        if ( mailLayerDisabled( "filter" ) )
            mailFilterAdded = true;   // tests: none
        else if ( mailFilterAdded )
            webkit_user_content_manager_add_filter( contentManager, s_mailFilter[viewExternal ? 1 : 0] );
    }
    WebKitNetworkSession* session = s_session;
    WebKitWebContext* context = s_context;
    WebKitSettings* settings = s_settings;
    if ( isMail() ) {
        session = mailExternalSession ? mailExternalSession : s_mailSession;
        context = s_mailContext;
        settings = s_mailSettings;
    } else if ( isApp() ) {
        session = app->session;
        context = app->context;
        settings = app->settings;
    }
    view = WEBKIT_WEB_VIEW( g_object_new( WEBKIT_TYPE_WEB_VIEW,
                                          "display", s_display,
                                          "web-context", context,
                                          "network-session", session,
                                          "settings", settings,
                                          "user-content-manager", contentManager,
                                          NULL ) );
    if ( isBrowser() ) {
        KWPEAdBlock::self()->addView( contentManager );
    } else if ( isMail() ) {
        // A frame without content yet (the first one, one of a new size)
        // is white like a message, not transparent (black on X).
        WebKitColor white = { 1.0, 1.0, 1.0, 1.0 };
        webkit_web_view_set_background_color( view, &white );
    }
    wpeView = webkit_web_view_get_wpe_view( view );
    g_object_set_data( G_OBJECT( view ), widgetKey, q );

    // One WPE toplevel per widget: the size and activity WebKit sees.
    toplevel = wpe_view_get_toplevel( wpeView );
    if ( toplevel ) {
        g_object_ref( toplevel );
    } else {
        toplevel = wpe_display_create_toplevel( s_display, 1 );
        wpe_view_set_toplevel( wpeView, toplevel );
    }

    g_signal_connect( view, "load-changed", G_CALLBACK( onLoadChanged ), this );
    g_signal_connect( view, "load-failed", G_CALLBACK( onLoadFailed ), this );
    g_signal_connect( view, "notify::uri", G_CALLBACK( onUriChanged ), this );
    g_signal_connect( view, "notify::title", G_CALLBACK( onTitleChanged ), this );
    g_signal_connect( view, "notify::estimated-load-progress", G_CALLBACK( onProgressChanged ), this );
    g_signal_connect( view, "mouse-target-changed", G_CALLBACK( onMouseTargetChanged ), this );
    g_signal_connect( view, "decide-policy", G_CALLBACK( onDecidePolicy ), this );
    g_signal_connect( webkit_web_view_get_back_forward_list( view ), "changed",
                      G_CALLBACK( onBackForwardChanged ), this );
    g_signal_connect( view, "context-menu", G_CALLBACK( onContextMenu ), this );
    g_signal_connect( view, "web-process-terminated", G_CALLBACK( onWebProcessTerminated ), this );
    g_signal_connect( view, "permission-request", G_CALLBACK( onPermissionRequest ), this );
    g_signal_connect( view, "insecure-content-detected", G_CALLBACK( onInsecureContent ), this );
    g_signal_connect( view, "authenticate", G_CALLBACK( onAuthenticate ), this );
    g_signal_connect( view, "submit-form", G_CALLBACK( onSubmitForm ), this );
    g_signal_connect( view, "run-file-chooser", G_CALLBACK( onRunFileChooser ), this );
    g_signal_connect( view, "script-dialog", G_CALLBACK( onScriptDialog ), this );
    KWPENetLog::watch( view );
    if ( isApp() ) {
        g_signal_connect( view, "show-notification", G_CALLBACK( onShowNotification ), this );
        // The notifications' pictures (see appNotificationWrapper): the
        // handler only in the bridge's own world.
        WebKitUserScript* script = webkit_user_script_new( appNotificationWrapper, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                           WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, 0, 0 );
        webkit_user_content_manager_add_script( contentManager, script );
        webkit_user_script_unref( script );
        QCString bridge = QCString( appNotificationBridge ) + "(" + appPictureOrigins( appOptions.pictureOrigins ) + ");\n";
        script = webkit_user_script_new_for_world( bridge.data(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                   WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, appNotificationWorld, 0, 0 );
        webkit_user_content_manager_add_script( contentManager, script );
        webkit_user_script_unref( script );
        g_signal_connect( contentManager, ( QCString( "script-message-received::" ) + appNotificationHandler ).data(),
                          G_CALLBACK( onAppNotification ), this );
        webkit_user_content_manager_register_script_message_handler( contentManager, appNotificationHandler,
                                                                     appNotificationWorld );
        g_signal_connect( view, "query-permission-state", G_CALLBACK( onQueryPermissionState ), this );
    }

    cancellable = g_cancellable_new();
    WebKitFindController* finder = webkit_web_view_get_find_controller( view );
    g_signal_connect( finder, "found-text", G_CALLBACK( onFoundText ), this );
    g_signal_connect( finder, "failed-to-find-text", G_CALLBACK( onFailedToFindText ), this );

    statsEnabled = envFlag( "KWPE_STATS" );
    if ( statsEnabled ) {
        frameCallback = webkit_web_view_add_frame_displayed_callback( view, onFrameDisplayed, this, 0 );
        if ( !statsTimer ) {
            statsTimer = new QTimer( q );
            QObject::connect( statsTimer, SIGNAL( timeout() ), q, SLOT( printFrameStats() ) );
            statsTimer->start( 1000 );
        }
    }

    attach();
    // A view made again for a widget on the screen (mail: a message with
    // external references): what resizeEvent() and showEvent() told the
    // old one.
    if ( q->isVisible() ) {
        wpe_toplevel_resized( toplevel, q->width(), q->height() );
        wpe_view_resized( wpeView, q->width(), q->height() );
        wpe_view_map( wpeView );
        wpe_toplevel_state_changed( toplevel, q->isActiveWindow() ? WPE_TOPLEVEL_STATE_ACTIVE : WPE_TOPLEVEL_STATE_NONE );
        if ( q->hasFocus() )
            wpe_view_focus_in( wpeView );
    }
}

void KWPEWidgetPrivate::destroy()
{
    if ( !view )
        return;
    // A dialog of the page answers WebKit (cancelled) while the view lives.
    closePageDialogs();
    g_signal_handlers_disconnect_by_data( webkit_web_view_get_back_forward_list( view ), this );
    g_signal_handlers_disconnect_by_data( webkit_web_view_get_find_controller( view ), this );
    g_cancellable_cancel( cancellable );
    g_object_unref( cancellable );
    cancellable = 0;
    g_signal_handlers_disconnect_by_data( view, this );
    g_object_set_data( G_OBJECT( view ), widgetKey, 0 );
    KWPE::attachView( wpeView, 0, 0 );
    forgetItems( 0 );
    if ( authRequest ) {
        webkit_authentication_request_cancel( authRequest );
        dropAuthentication();
    }
    if ( frameCallback )
        webkit_web_view_remove_frame_displayed_callback( view, frameCallback );
    frameCallback = 0;
    g_object_unref( view );
    g_object_unref( toplevel );
    if ( isBrowser() )
        KWPEAdBlock::self()->removeView( contentManager );
    forgetNotifications();
    QValueList<PendingPermission> permissions = pendingPermissions;
    pendingPermissions.clear();
    for ( QValueList<PendingPermission>::Iterator it = permissions.begin(); it != permissions.end(); ++it ) {
        if ( !( *it ).request )
            continue;
        webkit_permission_request_deny( ( *it ).request );
        g_object_unref( ( *it ).request );
    }
    g_signal_handlers_disconnect_by_data( contentManager, this );
    g_object_unref( contentManager );
    contentManager = 0;
    view = 0;
    wpeView = 0;
    toplevel = 0;
}

void KWPEWidgetPrivate::attach()
{
    if ( wpeView )
        KWPE::attachView( wpeView, q->winId(), this );
}

int KWPEWidgetPrivate::tokenForItem( WebKitBackForwardListItem* item, bool* isNew )
{
    if ( isNew )
        *isNew = false;
    if ( !item )
        return 0;
    QMap<WebKitBackForwardListItem*, int>::ConstIterator it = tokenOfItem.find( item );
    if ( it != tokenOfItem.end() )
        return it.data();
    // Unique in the process: Konqueror copies history entries (with
    // their tokens) between views, i.e. between part instances.
    static int nextToken = 1;
    int token = nextToken++;
    g_object_ref( item );
    itemOfToken.insert( token, item );
    tokenOfItem.insert( item, token );
    if ( isNew )
        *isNew = true;
    return token;
}

// Drop the tokens of @p items (all of them if 0).
void KWPEWidgetPrivate::forgetItems( GList* items )
{
    if ( !items ) {
        shownItem = 0;
        for ( QMap<int, WebKitBackForwardListItem*>::Iterator it = itemOfToken.begin(); it != itemOfToken.end(); ++it )
            g_object_unref( it.data() );
        itemOfToken.clear();
        tokenOfItem.clear();
        return;
    }
    for ( GList* l = items; l; l = l->next ) {
        WebKitBackForwardListItem* item = WEBKIT_BACK_FORWARD_LIST_ITEM( l->data );
        if ( item == shownItem )
            shownItem = 0;
        QMap<WebKitBackForwardListItem*, int>::Iterator it = tokenOfItem.find( item );
        if ( it == tokenOfItem.end() )
            continue;
        itemOfToken.remove( it.data() );
        tokenOfItem.remove( it );
        g_object_unref( item );
    }
}

void KWPEWidgetPrivate::onLoadChanged( WebKitWebView* view, WebKitLoadEvent event, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    switch ( event ) {
    case WEBKIT_LOAD_STARTED:
        d->notificationHello = false;
        d->reloadForFilters = false;
        d->redirected = false;
        d->loading = true;
        d->failed = false;
        d->loadStart = g_get_monotonic_time();
        emit d->q->loadStarted();
        break;
    case WEBKIT_LOAD_REDIRECTED:
        d->redirected = true;
        break;
    case WEBKIT_LOAD_COMMITTED: {
        bool isNew = false;
        // App mode, the second line of the boundary (the first: the final
        // response, decideAppPolicy()): whatever the main frame committed
        // without a response of an app site (about:blank a frame of
        // another site sent the top to, a page from the back/forward
        // list, ...) is not the application's: the application's page
        // is loaded again, nothing reaches the page meanwhile (drops and
        // permission requests check the URL too).
        // The previous document's announcements are gone with it (unless
        // the new one's bridge said hello already: see onAppNotification()).
        if ( d->isApp() && !d->notificationHello ) {
            d->notificationToken = QString::null;
            d->endNotificationDocument();
        }
        if ( d->isApp() && d->ownBlank ) {
            d->ownBlank = false;
        } else if ( d->isApp() && !d->isAppDocument( webkit_web_view_get_uri( view ) ) ) {
            const char* uri = webkit_web_view_get_uri( view );
            fprintf( stderr, "kwpe: app: the main frame committed %s, not one of the application's sites: "
                     "back to the application\n", uri ? uri : "(none)" );
            d->blockedURL = QString::fromUtf8( uri );
            webkit_web_view_stop_loading( view );
            QTimer::singleShot( 0, d->q, SLOT( reportBlocked() ) );
            QTimer::singleShot( 0, d->q, SLOT( returnToApp() ) );
            break;
        }
        // Usually set already (a page from the back/forward cache has no
        // response).  WebKit decides which elements to hide when it
        // requests the page: if the filters were switched later, the
        // page is loaded once more, with the right ones.
        if ( d->isBrowser() )
            KWPEAdBlock::self()->setViewHost( d->contentManager, KURL( QString::fromUtf8( webkit_web_view_get_uri( view ) ) ).host() );
        else if ( d->isMail() && KURL( QString::fromUtf8( webkit_web_view_get_uri( view ) ) ).host() == d->mailHost )
            d->forgetMailHosts( false );   // the previous message is gone from the view
        if ( d->reloadForFilters ) {
            d->reloadForFilters = false;
            QTimer::singleShot( 0, d->q, SLOT( reloadForFilters() ) );
        }
        d->ownBackForward = false;
        d->insecureContent = false;
        d->suppressDialogs = false;
        // Another page (also after a web process swap): the old page's
        // dialogs must not stay over it (WebKit closes them only when the
        // view goes).
        d->closePageDialogs();
        d->shownItem = webkit_back_forward_list_get_current_item( webkit_web_view_get_back_forward_list( view ) );
        d->tokenForItem( d->shownItem, &isNew );
        d->url = QString::fromUtf8( webkit_web_view_get_uri( view ) );
        d->firstPaintPending = true;
        if ( d->statsEnabled )
            fprintf( stderr, "kwpe: committed %s after %.0f ms\n", d->url.utf8().data(),
                     ( g_get_monotonic_time() - d->loadStart ) / 1000.0 );
        emit d->q->urlChanged( d->url, isNew );
        break;
    }
    case WEBKIT_LOAD_FINISHED:
        // The end of a load replaced by a newer one (still loading): not
        // the end of the newer one.
        if ( webkit_web_view_is_loading( view ) )
            break;
        d->loading = false;
        if ( d->failed )
            break;
        if ( d->statsEnabled )
            fprintf( stderr, "kwpe: finished after %.0f ms\n", ( g_get_monotonic_time() - d->loadStart ) / 1000.0 );
        emit d->q->loadFinished();
        d->findIcon();
        break;
    }
}

gboolean KWPEWidgetPrivate::onLoadFailed( WebKitWebView* view, WebKitLoadEvent, gchar*, GError* error, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // A load cancelled by a newer one (openURL(), a click while loading):
    // nothing ended for Konqueror, the newer load reports.
    if ( g_error_matches( error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED ) && webkit_web_view_is_loading( view ) )
        return TRUE;
    // Stopped by the user, or a response given to somebody else (see
    // onDecidePolicy): no error page, no message.
    bool quiet = g_error_matches( error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED )
        || g_error_matches( error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE );
    // WebKit sends LOAD_FINISHED after the failure; loadFinished() is
    // only for successful loads.
    d->loading = false;
    d->failed = true;
    if ( g_error_matches( error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE ) )
        emit d->q->loadHandedOff();
    else
        emit d->q->loadFailed( quiet ? QString::null : QString::fromUtf8( error->message ) );
    return quiet;   // otherwise WebKit shows its error page
}

void KWPEWidgetPrivate::onUriChanged( GObject*, GParamSpec*, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    QString uri = QString::fromUtf8( webkit_web_view_get_uri( d->view ) );
    // While loading, the URI is the provisional one: the commit reports
    // it.  Otherwise it is a navigation inside the document.
    // webkit_web_view_load_uri() changes the URI at once (before
    // LOAD_STARTED): is_loading() counts that pending request.
    if ( d->loading || webkit_web_view_is_loading( d->view ) || uri == d->url )
        return;
    d->ownBackForward = false;
    bool isNew = false;
    d->shownItem = webkit_back_forward_list_get_current_item( webkit_web_view_get_back_forward_list( d->view ) );
    d->tokenForItem( d->shownItem, &isNew );
    d->url = uri;
    emit d->q->urlChanged( uri, isNew );
}

void KWPEWidgetPrivate::onTitleChanged( GObject*, GParamSpec*, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    d->title = QString::fromUtf8( webkit_web_view_get_title( d->view ) );
    emit d->q->titleChanged( d->title );
}

void KWPEWidgetPrivate::onProgressChanged( GObject*, GParamSpec*, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    emit d->q->loadProgress( int( webkit_web_view_get_estimated_load_progress( d->view ) * 100.0 + 0.5 ) );
}

void KWPEWidgetPrivate::onMouseTargetChanged( WebKitWebView*, WebKitHitTestResult* hit, guint, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    d->overEditable = webkit_hit_test_result_context_is_editable( hit );
    QString link;
    if ( webkit_hit_test_result_context_is_link( hit ) )
        link = QString::fromUtf8( webkit_hit_test_result_get_link_uri( hit ) );
    // Mail: an anchor of the host's document as the host wrote it.
    if ( d->isMail() && link.startsWith( d->mailMainURL + "#" ) )
        link = link.mid( d->mailMainURL.length() );
    emit d->q->linkHovered( link );
}

// How many steps a back/forward navigation to @p uri goes from @p shown,
// the item on screen.  WebKit moved its current item to the target
// already (and moves it back if the navigation is ignored); if it did
// not, the nearest item with @p uri.
static int historyStepsTo( WebKitBackForwardList* list, WebKitBackForwardListItem* shown, const char* uri )
{
    WebKitBackForwardListItem* current = webkit_back_forward_list_get_current_item( list );
    if ( shown && current && shown != current ) {
        for ( int i = 1; i <= 100; ++i ) {
            if ( webkit_back_forward_list_get_nth_item( list, i ) == shown )
                return -i;
            if ( webkit_back_forward_list_get_nth_item( list, -i ) == shown )
                return i;
        }
        return 0;
    }

    int back = 0, forward = 0;
    GList* items = webkit_back_forward_list_get_back_list( list );   // nearest first
    int i = 1;
    for ( GList* l = items; l && !back; l = l->next, ++i )
        if ( g_strcmp0( webkit_back_forward_list_item_get_uri( WEBKIT_BACK_FORWARD_LIST_ITEM( l->data ) ), uri ) == 0 )
            back = -i;
    g_list_free( items );
    items = webkit_back_forward_list_get_forward_list( list );
    i = 1;
    for ( GList* l = items; l && !forward; l = l->next, ++i )
        if ( g_strcmp0( webkit_back_forward_list_item_get_uri( WEBKIT_BACK_FORWARD_LIST_ITEM( l->data ) ), uri ) == 0 )
            forward = i;
    g_list_free( items );
    if ( back && forward )
        return -back < forward ? back : forward;
    return back ? back : forward;
}

// The schemes WebKit loads itself; others go to Konqueror.
static bool isWebKitScheme( const char* uri )
{
    static const char* const schemes[] = { "http:", "https:", "file:", "data:", "about:", "blob:", "javascript:" };
    for ( unsigned i = 0; i < sizeof( schemes ) / sizeof( schemes[0] ); ++i )
        if ( g_ascii_strncasecmp( uri, schemes[i], strlen( schemes[i] ) ) == 0 )
            return true;
    return false;
}

gboolean KWPEWidgetPrivate::onDecidePolicy( WebKitWebView* view, WebKitPolicyDecision* decision,
                                            WebKitPolicyDecisionType type, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( d->isMail() )
        return d->decideMailPolicy( decision, type );
    if ( d->isApp() )
        return d->decideAppPolicy( decision, type );

    if ( type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION ) {
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION( decision ) );
        QString uri = QString::fromUtf8( webkit_uri_request_get_uri( webkit_navigation_action_get_request( action ) ) );
        webkit_policy_decision_ignore( decision );
        emit d->q->newWindowRequested( uri );
        return TRUE;
    }

    if ( type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ) {
        WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
            WEBKIT_NAVIGATION_POLICY_DECISION( decision ) );
        const char* uri = webkit_uri_request_get_uri( webkit_navigation_action_get_request( action ) );
        WebKitNavigationType navigation = webkit_navigation_action_get_navigation_type( action );

        // A middle click on a link: a new window (or tab), as in KHTML.
        if ( navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED && webkit_navigation_action_get_mouse_button( action ) == 2 ) {
            webkit_policy_decision_ignore( decision );
            emit d->q->newWindowRequested( QString::fromUtf8( uri ) );
            return TRUE;
        }

        // mailto:, ftp:, smb:, ...: WebKit would show an error page.  For
        // Konqueror (a mail program, KIO), on a click or another user
        // gesture only: a page must not start programs by itself.
        if ( !isWebKitScheme( uri ) ) {
            webkit_policy_decision_ignore( decision );
            if ( webkit_navigation_action_is_user_gesture( action ) || navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED ) {
                d->externalUrl = QString::fromUtf8( uri );
                QTimer::singleShot( 0, d->q, SLOT( emitExternalURL() ) );
            }
            return TRUE;
        }

        if ( navigation == WEBKIT_NAVIGATION_TYPE_BACK_FORWARD ) {
            if ( d->ownBackForward ) {
                d->ownBackForward = false;
                return FALSE;
            }
            // history.back() and friends: Konqueror owns the history, let
            // it go there (it comes back through goToHistoryToken()).
            int steps = historyStepsTo( webkit_web_view_get_back_forward_list( view ), d->shownItem, uri );
            if ( steps ) {
                webkit_policy_decision_ignore( decision );
                emit d->q->historyNavigationRequested( steps );
                return TRUE;
            }
        }
        // The scriptlets of the site, in the frame (main frame or not)
        // that is about to load it, before its request.
        KWPEAdBlock::self()->addFrameHost( d->contentManager, KURL( QString::fromUtf8( uri ) ).host() );
        return FALSE;
    }

    if ( type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE ) {
        WebKitResponsePolicyDecision* response = WEBKIT_RESPONSE_POLICY_DECISION( decision );
        // A link (or a redirect) from a site where ad blocking is off to
        // one where it is on, or the other way round: the filters switch
        // before WebKit commits the page and loads what it needs.  Its
        // element hiding was decided with the previous ones: reloaded
        // after the commit, unless a second request could do harm: an
        // answer to POST, the end of a redirect (logins, payments: one-
        // time codes).  (Pages Konqueror opens, typed or from another
        // program, get the right filters before they are requested.)
        if ( webkit_response_policy_decision_is_main_frame_main_resource( response ) ) {
            QString uri = QString::fromUtf8( webkit_uri_response_get_uri( webkit_response_policy_decision_get_response( response ) ) );
            KURL responseURL( uri );
            bool switched = KWPEAdBlock::self()->setViewHost( d->contentManager, responseURL.host() );
            const char* method = webkit_uri_request_get_http_method( webkit_response_policy_decision_get_request( response ) );
            if ( switched && ( !method || g_ascii_strcasecmp( method, "GET" ) == 0 ) && !d->redirected )
                d->reloadForFilters = true;
        } else {
            // a subframe, perhaps redirected to a site not seen before
            KWPEAdBlock::self()->addFrameHost( d->contentManager, KURL( QString::fromUtf8(
                webkit_uri_response_get_uri( webkit_response_policy_decision_get_response( response ) ) ) ).host() );
        }
        // PDF: WebKit has a viewer (PDF.js), Konqueror's choice unless asked.
        bool pdfToKonqueror = g_strcmp0( webkit_uri_response_get_mime_type( webkit_response_policy_decision_get_response( response ) ),
                                         "application/pdf" ) == 0 && !KWPESettings::pdfInPage();
        if ( webkit_response_policy_decision_is_main_frame_main_resource( response )
             && ( pdfToKonqueror || !webkit_response_policy_decision_is_mime_type_supported( response ) ) ) {
            // The answer to a POST cannot be fetched again by Konqueror:
            // WebKit downloads it.
            const char* method = webkit_uri_request_get_http_method( webkit_response_policy_decision_get_request( response ) );
            if ( method && g_ascii_strcasecmp( method, "GET" ) != 0 ) {
                webkit_policy_decision_download( decision );
                return TRUE;
            }
            WebKitURIResponse* r = webkit_response_policy_decision_get_response( response );
            d->unsupportedUrl = QString::fromUtf8( webkit_uri_response_get_uri( r ) );
            d->unsupportedMimeType = QString::fromLatin1( webkit_uri_response_get_mime_type( r ) );
            webkit_policy_decision_ignore( decision );
            // Not inside WebKit's signal: Konqueror may ask the user
            // (an event loop) and replace this part (delete the view).
            QTimer::singleShot( 0, d->q, SLOT( emitUnsupportedContent() ) );
            return TRUE;
        }
    }
    return FALSE;
}

// Mail mode: WebKit loads the message's own documents (the host's one,
// the frames of its HTML parts, anchors in them) and nothing else.  What
// the user starts (a click, also on target=_blank and with the middle
// button) goes to the host; what the message would start by itself
// (refresh, redirects, forms) is dropped.
gboolean KWPEWidgetPrivate::decideMailPolicy( WebKitPolicyDecision* decision, WebKitPolicyDecisionType type )
{
    if ( type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE )
        return FALSE;   // only our own documents get this far

    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION( decision ) );
    const char* uri = webkit_uri_request_get_uri( webkit_navigation_action_get_request( action ) );
    WebKitNavigationType navigation = webkit_navigation_action_get_navigation_type( action );
    bool byUser = navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED || webkit_navigation_action_is_user_gesture( action );

    if ( type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && !mailHost.isEmpty() ) {
        KURL target( QString::fromUtf8( uri ) );
        if ( target.protocol() == mailScheme && target.host() == mailHost ) {
            QString file = target.path();
            bool own = file.endsWith( ".html" );
            // An anchor (the attachment list, a message's table of contents).
            if ( own && target.hasRef() && navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED )
                return FALSE;
            // Our load of the host's document, the frames it has.
            if ( own && !byUser && !target.hasRef()
                 && ( navigation == WEBKIT_NAVIGATION_TYPE_OTHER || navigation == WEBKIT_NAVIGATION_TYPE_RELOAD ) )
                return FALSE;
        }
    }
    webkit_policy_decision_ignore( decision );
    if ( byUser && navigation != WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED
         && navigation != WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED && g_ascii_strncasecmp( uri, "kwpemail:", 9 ) != 0 )
        activateLink( uri );
    else if ( getenv( "KWPE_DEBUG" ) )
        fprintf( stderr, "kwpe: mail: navigation to %s dropped (type %d)\n", uri, int( navigation ) );
    return TRUE;
}

void KWPEWidgetPrivate::activateLink( const char* uri )
{
    // The host may run an event loop (a dialog) or delete us: not inside
    // WebKit's signal.
    activatedLinks.append( QString::fromUtf8( uri ) );
    QTimer::singleShot( 0, q, SLOT( emitExternalURL() ) );
}

// App mode: the application's own sites stay in the view; a link the
// user follows to another site, a new window (target=_blank,
// window.open() after a click), a scheme WebKit does not load (mailto:,
// tel:, ...) go to the host (its browser, its mail program).  A new
// window of a blob: or data: URL (a file the page made) is downloaded:
// nothing outside the page can read it.  What the view cannot show is
// downloaded (KDE's save dialog), PDF too: the application's page stays.
gboolean KWPEWidgetPrivate::decideAppPolicy( WebKitPolicyDecision* decision, WebKitPolicyDecisionType type )
{
    if ( type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE ) {
        WebKitResponsePolicyDecision* response = WEBKIT_RESPONSE_POLICY_DECISION( decision );
        if ( !webkit_response_policy_decision_is_main_frame_main_resource( response ) )
            return FALSE;
        // The boundary, whatever led here (a script, a form, a refresh, a
        // redirect, no user at all): the main frame shows the
        // application's sites and nothing else.  This is the final
        // response, after every redirect.
        const char* responseURI = webkit_uri_response_get_uri( webkit_response_policy_decision_get_response( response ) );
        if ( !isAppDocument( responseURI ) ) {
            webkit_policy_decision_ignore( decision );
            fprintf( stderr, "kwpe: app: %s is not one of the application's sites: not shown\n", responseURI );
            blockedURL = QString::fromUtf8( responseURI );
            QTimer::singleShot( 0, q, SLOT( reportBlocked() ) );
            return TRUE;
        }
        const char* mimeType = webkit_uri_response_get_mime_type( webkit_response_policy_decision_get_response( response ) );
        if ( g_strcmp0( mimeType, "application/pdf" ) == 0 || !webkit_response_policy_decision_is_mime_type_supported( response ) ) {
            webkit_policy_decision_download( decision );
            return TRUE;
        }
        return FALSE;
    }

    WebKitNavigationAction* action = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION( decision ) );
    const char* uri = webkit_uri_request_get_uri( webkit_navigation_action_get_request( action ) );
    WebKitNavigationType navigation = webkit_navigation_action_get_navigation_type( action );
    bool byUser = navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED || webkit_navigation_action_is_user_gesture( action );
    bool local = g_ascii_strncasecmp( uri, "blob:", 5 ) == 0 || g_ascii_strncasecmp( uri, "data:", 5 ) == 0;

    if ( type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION
         || ( navigation == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED && webkit_navigation_action_get_mouse_button( action ) == 2 ) ) {
        webkit_policy_decision_ignore( decision );
        if ( local ) {
            pendingDownloads.append( QString::fromUtf8( uri ) );
            QTimer::singleShot( 0, q, SLOT( startDownloads() ) );
        } else if ( g_ascii_strncasecmp( uri, "javascript:", 11 ) != 0 && g_ascii_strncasecmp( uri, "about:", 6 ) != 0 ) {
            activateLink( uri );
        }
        return TRUE;
    }

    if ( !isWebKitScheme( uri ) ) {
        webkit_policy_decision_ignore( decision );
        if ( byUser )
            activateLink( uri );
        return TRUE;
    }

    KURL target( QString::fromUtf8( uri ) );
    bool web = target.protocol() == "http" || target.protocol() == "https";
    if ( web && byUser && navigation != WEBKIT_NAVIGATION_TYPE_FORM_SUBMITTED
         && navigation != WEBKIT_NAVIGATION_TYPE_FORM_RESUBMITTED && !isAppSite( target.host() ) ) {
        webkit_policy_decision_ignore( decision );
        activateLink( uri );
        return TRUE;
    }
    return FALSE;
}

bool KWPEWidgetPrivate::isAppSite( const QString& host ) const
{
    return appOptions.sites.contains( host.lower() );
}

// A document the main frame may show in app mode: http(s) of the
// application's sites, a blob: of theirs (its URL carries its origin).
// No about:blank (its URL does not say whose it is: another site can
// make one), data:, file:, other blob: or other schemes.
bool KWPEWidgetPrivate::isAppDocument( const char* uri ) const
{
    if ( !uri )
        return false;
    QString text = QString::fromUtf8( uri );
    if ( text.startsWith( "blob:", false ) )
        text = text.mid( 5 );
    KURL url( text );
    return ( url.protocol() == "http" || url.protocol() == "https" ) && isAppSite( url.host() );
}

// The page's notification: for the host (KNotify), outside WebKit's
// signal.  Kept (referenced) until it is closed, for its click.
gboolean KWPEWidgetPrivate::onShowNotification( WebKitWebView*, WebKitNotification* notification, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // A long-running page: the oldest go, closed in WebKit too (its
    // provider keeps every notification until it is closed; the page gets
    // its close event), reported closed to the host once (outside
    // WebKit's signal: our "closed" handler is off first).
    while ( d->notifications.count() >= 100 ) {
        QMap<int, WebKitNotification*>::Iterator oldest = d->notifications.begin();
        int old = oldest.key();
        WebKitNotification* evicted = oldest.data();
        g_signal_handlers_disconnect_by_data( evicted, d );
        webkit_notification_close( evicted );
        g_object_unref( evicted );
        d->notificationShownAt.remove( old );
        d->notificationShownIn.remove( old );
        d->notificationsToShow.remove( old );
        for ( QValueList<NotificationIcon>::Iterator icon = d->notificationIcons.begin(); icon != d->notificationIcons.end(); ++icon )
            if ( ( *icon ).id == old ) {
                d->notificationIcons.remove( icon );
                break;
            }
        d->notifications.remove( oldest );
        d->notificationsEvicted.append( old );
    }
    int id = ++d->notificationSerial;
    g_object_ref( notification );
    d->notifications.insert( id, notification );
    d->notificationShownAt.insert( id, g_get_monotonic_time() );
    d->notificationShownIn.insert( id, d->notificationGeneration );
    g_signal_connect( notification, "closed", G_CALLBACK( onNotificationClosed ), d );
    d->notificationsToShow.append( id );
    QTimer::singleShot( 0, d->q, SLOT( emitNotifications() ) );
    return TRUE;
}

void KWPEWidgetPrivate::onNotificationClosed( WebKitNotification* notification, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    for ( QMap<int, WebKitNotification*>::Iterator it = d->notifications.begin(); it != d->notifications.end(); ++it ) {
        if ( it.data() != notification )
            continue;
        int id = it.key();
        g_signal_handlers_disconnect_by_data( notification, d );
        d->notifications.remove( it );
        g_object_unref( notification );
        d->notificationsToShow.remove( id );
        d->notificationShownAt.remove( id );
        d->notificationShownIn.remove( id );
        for ( QValueList<NotificationIcon>::Iterator icon = d->notificationIcons.begin(); icon != d->notificationIcons.end(); ++icon )
            if ( ( *icon ).id == id ) {
                d->notificationIcons.remove( icon );
                break;
            }
        emit d->q->notificationClosed( id );
        return;
    }
}

void KWPEWidgetPrivate::forgetNotifications()
{
    for ( QMap<int, WebKitNotification*>::Iterator it = notifications.begin(); it != notifications.end(); ++it ) {
        g_signal_handlers_disconnect_by_data( it.data(), this );
        g_object_unref( it.data() );
    }
    notifications.clear();
    notificationsToShow.clear();
    notificationShownAt.clear();
    notificationShownIn.clear();
    notificationsEvicted.clear();
    notificationIcons.clear();
    droppedAnnouncements.clear();
    announcementsLost = false;
    notificationToken = QString::null;
}

// FNV-1a (64 bits) of title, body and tag (each with its length).
Q_UINT64 KWPEWidgetPrivate::announcementKey( const QString& title, const QString& body, const QString& tag )
{
    Q_UINT64 hash = Q_UINT64( 14695981039346656037ULL );
    const QString* parts[3] = { &title, &body, &tag };
    for ( int p = 0; p < 3; ++p ) {
        uint length = parts[p]->length();
        for ( uint i = 0; i <= length; ++i ) {
            uint c = i < length ? ( *parts[p] )[i].unicode() : 0x10000 + length;
            for ( int b = 0; b < 3; ++b ) {
                hash ^= ( c >> ( 8 * b ) ) & 0xff;
                hash *= Q_UINT64( 1099511628211ULL );
            }
        }
    }
    return hash;
}

void KWPEWidgetPrivate::dropAnnouncement( const NotificationIcon& icon )
{
    if ( icon.generation != notificationGeneration )
        return;
    if ( announcementsLost )
        return;
    Q_UINT64 key = announcementKey( icon.title, icon.body, icon.tag );
    if ( droppedAnnouncements.contains( key ) )
        return;
    if ( droppedAnnouncements.count() >= 64 ) {
        // (fail closed: none forgotten, none matched from now on)
        announcementsLost = true;
        droppedAnnouncements.clear();
        return;
    }
    droppedAnnouncements.append( key );
}

// A dropped announcement may have had this key (or which ones were
// dropped is not known any more).
bool KWPEWidgetPrivate::wasDropped( Q_UINT64 key ) const
{
    return announcementsLost || droppedAnnouncements.contains( key );
}

// The bridge's document is gone: its announcements too; a notification
// waiting for its picture waits no more; the notifications still queued
// are of the old generation: no announcement of the new document is
// given to them.
void KWPEWidgetPrivate::endNotificationDocument()
{
    ++notificationGeneration;
    droppedAnnouncements.clear();
    announcementsLost = false;
    bool waiting = false;
    for ( QValueList<NotificationIcon>::Iterator it = notificationIcons.begin(); it != notificationIcons.end(); ) {
        if ( ( *it ).id ) {
            waiting = waiting || !( *it ).done;
            ( *it ).done = true;
            ++it;
        } else {
            it = notificationIcons.remove( it );
        }
    }
    if ( waiting )
        QTimer::singleShot( 0, q, SLOT( emitNotifications() ) );
}

static QString jsString( JSCValue* object, const char* name )
{
    JSCValue* value = jsc_value_object_get_property( object, name );
    QString result;
    if ( jsc_value_is_string( value ) ) {
        char* text = jsc_value_to_string( value );
        result = QString::fromUtf8( text );
        g_free( text );
    }
    g_object_unref( value );
    return result;
}

// The bridge's message (appNotificationBridge): a new document
// ("hello"), a picture announced ("pending") or ready ("icon"; its
// pixels or none).  Only the bridge's world reaches the handler (of the
// top frame: no other frame runs it); what it says of a notification is
// still the page's word: only for the application's site, the current
// document, a bounded list, a small picture of exactly its bytes or
// nothing.
void KWPEWidgetPrivate::onAppNotification( WebKitUserContentManager*, JSCValue* value, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( !jsc_value_is_object( value ) || !d->view || !d->isAppDocument( webkit_web_view_get_uri( d->view ) ) )
        return;
    QString kind = jsString( value, "kind" );
    QString token = jsString( value, "token" );
    if ( token.length() != 32 )
        return;
    if ( kind == "hello" ) {
        // A new document (or one back from the page cache): the
        // previous one's announcements are gone, its late messages too.
        d->notificationHello = true;
        if ( token != d->notificationToken ) {
            d->notificationToken = token;
            d->endNotificationDocument();
        }
        return;
    }
    if ( token != d->notificationToken )
        return;
    if ( kind == "debug" ) {
        // only the scheme and host (the bridge's where()), checked again
        QString what = jsString( value, "what" ), origin = jsString( value, "origin" );
        if ( !d->appOptions.debug || ( what != "refused" && what != "failed" && what != "timed out" )
             || origin.length() > 300 || !QRegExp( "^[a-z][a-z0-9+.-]*:(//[a-z0-9.:\\[\\]-]*)?$" ).exactMatch( origin ) )
            return;
        fprintf( stderr, "kwpe: app: a notification's picture %s: %s\n", what.latin1(), origin.latin1() );
        return;
    }
    JSCValue* serialValue = jsc_value_object_get_property( value, "serial" );
    int serial = jsc_value_is_number( serialValue ) ? jsc_value_to_int32( serialValue ) : 0;
    g_object_unref( serialValue );
    if ( kind == "pending" ) {
        NotificationIcon icon;
        icon.serial = serial;
        icon.title = jsString( value, "title" );
        icon.body = jsString( value, "body" );
        icon.tag = jsString( value, "tag" );
        icon.id = 0;
        // (a notification without a picture: nothing to wait for)
        JSCValue* picture = jsc_value_object_get_property( value, "picture" );
        icon.done = !( jsc_value_is_boolean( picture ) && jsc_value_to_boolean( picture ) );
        g_object_unref( picture );
        icon.ambiguous = false;
        icon.announced = g_get_monotonic_time();
        icon.generation = d->notificationGeneration;
        if ( icon.title.length() > 4096 || icon.body.length() > 4096 || icon.tag.length() > 4096 )
            return;
        // At most 20: the oldest not yet given to a notification goes
        // (the one a notification waits for stays: no announcement makes
        // it wait longer, see emitNotifications()).
        if ( d->notificationIcons.count() >= 20 ) {
            QValueList<NotificationIcon>::Iterator it = d->notificationIcons.begin();
            while ( it != d->notificationIcons.end() && ( *it ).id )
                ++it;
            if ( it == d->notificationIcons.end() )
                return;
            d->dropAnnouncement( *it );
            d->notificationIcons.remove( it );
        }
        d->notificationIcons.append( icon );
    } else if ( kind == "icon" ) {
        for ( QValueList<NotificationIcon>::Iterator it = d->notificationIcons.begin(); it != d->notificationIcons.end(); ++it ) {
            if ( ( *it ).serial != serial || ( *it ).done )
                continue;
            ( *it ).done = true;
            ( *it ).image = notificationImage( value );
            if ( ( *it ).id )
                QTimer::singleShot( 0, d->q, SLOT( emitNotifications() ) );
            break;
        }
    }
}

static int jsInt( JSCValue* object, const char* name )
{
    JSCValue* value = jsc_value_object_get_property( object, name );
    int result = -1;
    if ( jsc_value_is_number( value ) ) {
        double number = jsc_value_to_double( value );
        if ( number >= 0 && number <= 65536 && number == int( number ) )
            result = int( number );
    }
    g_object_unref( value );
    return result;
}

// The picture of an "icon" message: width x height (1 to
// appNotificationIconMax each) RGBA pixels, not premultiplied (canvas'
// getImageData()), base64; exactly that many bytes, or a null image.
// No image decoder: the host builds the image from the bytes itself.
QImage KWPEWidgetPrivate::notificationImage( JSCValue* value )
{
    int width = jsInt( value, "width" );
    int height = jsInt( value, "height" );
    if ( width < 1 || height < 1 || width > appNotificationIconMax || height > appNotificationIconMax )
        return QImage();
    uint bytes = uint( width ) * uint( height ) * 4;
    QString encoded = jsString( value, "pixels" );
    if ( encoded.length() != ( bytes + 2 ) / 3 * 4 )
        return QImage();
    // base64 (decoded here: the length is known, nothing else is taken)
    QByteArray pixels( bytes );
    uint out = 0, bits = 0, count = 0;
    for ( uint i = 0; i < encoded.length(); ++i ) {
        ushort c = encoded[i].unicode();
        uint v;
        if ( c >= 'A' && c <= 'Z' )
            v = c - 'A';
        else if ( c >= 'a' && c <= 'z' )
            v = c - 'a' + 26;
        else if ( c >= '0' && c <= '9' )
            v = c - '0' + 52;
        else if ( c == '+' )
            v = 62;
        else if ( c == '/' )
            v = 63;
        else if ( c == '=' && i + 2 >= encoded.length() && out == bytes )
            continue;   // the padding, after all the bytes
        else
            return QImage();
        bits = ( ( bits << 6 ) | v ) & 0xffff;
        count += 6;
        if ( count >= 8 ) {
            count -= 8;
            if ( out == bytes )
                return QImage();
            pixels[out++] = char( ( bits >> count ) & 0xff );
        }
    }
    if ( out != bytes )
        return QImage();
    QImage image( width, height, 32 );
    if ( image.isNull() )
        return QImage();
    image.setAlphaBuffer( true );
    const unsigned char* p = reinterpret_cast<const unsigned char*>( pixels.data() );
    for ( int y = 0; y < height; ++y ) {
        QRgb* line = reinterpret_cast<QRgb*>( image.scanLine( y ) );
        for ( int x = 0; x < width; ++x, p += 4 )
            line[x] = qRgba( p[0], p[1], p[2], p[3] );
    }
    return image;
}

// navigator.permissions.query(): what the user answered before, for
// notifications; WebKit's default for the rest.
gboolean KWPEWidgetPrivate::onQueryPermissionState( WebKitWebView*, WebKitPermissionStateQuery* query, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( g_strcmp0( webkit_permission_state_query_get_name( query ), "notifications" ) != 0 )
        return FALSE;
    char* text = webkit_security_origin_to_string( webkit_permission_state_query_get_security_origin( query ) );
    QString origin = text ? QString::fromUtf8( text ) : QString::null;
    g_free( text );
    WebKitPermissionState state = WEBKIT_PERMISSION_STATE_PROMPT;
    if ( d->app->deniedNotifications.contains( origin ) )
        state = WEBKIT_PERMISSION_STATE_DENIED;
    else if ( d->app->allowedNotifications.contains( origin ) )
        state = WEBKIT_PERMISSION_STATE_GRANTED;
    webkit_permission_state_query_finish( query, state );
    return TRUE;
}

void KWPEWidgetPrivate::onMailState( WebKitUserContentManager*, JSCValue* value, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( !jsc_value_is_object( value ) )
        return;
    JSCValue* bottom = jsc_value_object_get_property( value, "b" );
    JSCValue* selection = jsc_value_object_get_property( value, "s" );
    d->mailAtBottom = jsc_value_to_boolean( bottom );
    char* text = jsc_value_is_string( selection ) ? jsc_value_to_string( selection ) : 0;
    d->mailSelection = text ? QString::fromUtf8( text ) : QString::null;
    g_free( text );
    g_object_unref( bottom );
    g_object_unref( selection );
}

// The messages of this widget WebKit may still read: the one shown (the
// last one) or all of them gone (the view went).
void KWPEWidgetPrivate::forgetMailHosts( bool all )
{
    if ( !s_mailDocuments )
        return;
    QStringList keep;
    for ( QStringList::ConstIterator it = mailHosts.begin(); it != mailHosts.end(); ++it ) {
        if ( !all && *it == mailHost )
            keep.append( *it );
        else
            s_mailDocuments->remove( *it );
    }
    mailHosts = keep;
}

void KWPEWidgetPrivate::onBackForwardChanged( WebKitBackForwardList*, WebKitBackForwardListItem*, GList* removed, gpointer data )
{
    if ( removed )
        static_cast<KWPEWidgetPrivate*>( data )->forgetItems( removed );
}

gboolean KWPEWidgetPrivate::onContextMenu( WebKitWebView*, WebKitContextMenu*, WebKitHitTestResult* hit, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    d->menuLink = webkit_hit_test_result_context_is_link( hit )
        ? QString::fromUtf8( webkit_hit_test_result_get_link_uri( hit ) ) : QString::null;
    d->menuImage = webkit_hit_test_result_context_is_image( hit )
        ? QString::fromUtf8( webkit_hit_test_result_get_image_uri( hit ) ) : QString::null;
    d->menuSelection = webkit_hit_test_result_context_is_selection( hit );
    d->menuEditable = webkit_hit_test_result_context_is_editable( hit );
    // Konqueror's popup menu runs an event loop: not inside WebKit's signal.
    QTimer::singleShot( 0, d->q, SLOT( showContextMenu() ) );
    return TRUE;   // WPE has no menu of its own
}

void KWPEWidgetPrivate::onSelectionText( GObject* view, GAsyncResult* result, gpointer )
{
    GError* error = 0;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish( WEBKIT_WEB_VIEW( view ), result, &error );
    if ( !value ) {
        // Cancelled: the widget is gone.
        g_error_free( error );
        return;
    }
    if ( jsc_value_is_string( value ) ) {
        char* text = jsc_value_to_string( value );
        QString selection = QString::fromUtf8( text );
        g_free( text );
        if ( !selection.isEmpty() )
            QApplication::clipboard()->setText( selection, QClipboard::Clipboard );
    }
    g_object_unref( value );
}

// The page's icon for Konqueror (its location bar, tabs, bookmarks go
// through KDE's favicon cache, which downloads it): of the page's
// <link rel=icon> elements the best one for 16-32 pixels: a PNG or ICO
// of about that size, one without sizes (an ICO has several), an SVG,
// a large one, Apple's touch icon last; media queries are honoured.  In
// our script world (the page cannot fake the DOM functions).  No link:
// Konqueror asks for /favicon.ico itself.
void KWPEWidgetPrivate::findIcon()
{
    static const char script[] =
        "(function() {"
        "  var best = '', bestScore = -1, links = document.querySelectorAll('link[rel]');"
        "  for (var i = 0; i < links.length; i++) {"
        "    var l = links[i], rel = (l.getAttribute('rel') || '').toLowerCase().split(/\\s+/);"
        "    var icon = rel.indexOf('icon') >= 0;"
        "    var apple = rel.indexOf('apple-touch-icon') >= 0 || rel.indexOf('apple-touch-icon-precomposed') >= 0;"
        "    if (!icon && !apple) continue;"
        "    var href = l.href;"
        "    if (!/^https?:/i.test(href)) continue;"
        "    var media = l.getAttribute('media');"
        "    if (media) { try { if (!matchMedia(media).matches) continue; } catch (e) { continue; } }"
        "    var type = (l.getAttribute('type') || '').toLowerCase();"
        "    var sizes = (l.getAttribute('sizes') || '').toLowerCase();"
        "    var m = sizes.match(/(\\d+)x\\d+/), size = m ? +m[1] : 0, score;"
        "    if (apple) score = 1;"
        "    else if (sizes == 'any' || type == 'image/svg+xml' || /\\.svgz?([?#]|$)/i.test(href)) score = 6;"
        "    else if (!size) score = 7;"
        "    else if (size <= 64) score = 9 - Math.abs(size - 32) / 100;"
        "    else score = 5 - size / 10000;"
        "    if (score > bestScore) { best = href; bestScore = score; }"
        "  }"
        "  return best;"
        "})()";
    // KDE's favicon cache is on disk and fetched by kded: not for a
    // session that keeps nothing.
    if ( view && !envFlag( "KWPE_EPHEMERAL" ) && !isMail() )
        webkit_web_view_evaluate_javascript( view, script, -1, "kwpepart", 0, cancellable, onIconURL, this );
}

void KWPEWidgetPrivate::onIconURL( GObject* view, GAsyncResult* result, gpointer data )
{
    GError* error = 0;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish( WEBKIT_WEB_VIEW( view ), result, &error );
    if ( !value ) {
        // Cancelled: the widget is gone (data is too).
        g_error_free( error );
        return;
    }
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( jsc_value_is_string( value ) ) {
        char* text = jsc_value_to_string( value );
        QString url = QString::fromUtf8( text );
        g_free( text );
        if ( !url.isEmpty() )
            emit d->q->iconURLChanged( url );
    }
    g_object_unref( value );
}

void KWPEWidgetPrivate::onSourceData( GObject* resource, GAsyncResult* result, gpointer data )
{
    GError* error = 0;
    gsize length = 0;
    guchar* bytes = webkit_web_resource_get_data_finish( WEBKIT_WEB_RESOURCE( resource ), result, &length, &error );
    if ( !bytes ) {
        // Cancelled (the widget is gone) or no data.
        g_clear_error( &error );
        return;
    }
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    QByteArray source;
    source.duplicate( reinterpret_cast<const char*>( bytes ), length );
    g_free( bytes );
    emit d->q->sourceReady( QString::fromUtf8( webkit_web_resource_get_uri( WEBKIT_WEB_RESOURCE( resource ) ) ), source );
}

void KWPEWidgetPrivate::onFoundText( WebKitFindController*, guint matchCount, gpointer data )
{
    emit static_cast<KWPEWidgetPrivate*>( data )->q->findResult( true, matchCount );
}

void KWPEWidgetPrivate::onFailedToFindText( WebKitFindController*, gpointer data )
{
    emit static_cast<KWPEWidgetPrivate*>( data )->q->findResult( false, 0 );
}

void KWPEWidgetPrivate::onWebProcessTerminated( WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // Its dialogs died with it.
    d->closePageDialogs();
    bool wasLoading = d->loading;
    d->loading = false;
    d->failed = true;
    if ( reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API )
        return;
    fprintf( stderr, "kwpe: the web process of %s %s\n", d->url.utf8().data(),
             reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed" : "exceeded its memory limit" );
    // Konqueror must not keep spinning.
    if ( wasLoading )
        emit d->q->loadFailed( QString::null );
    // A crash: reload once (a new web process), not in a loop.  Out of
    // memory: the same page would do it again; reload by hand.
    gint64 now = g_get_monotonic_time();
    if ( reason == WEBKIT_WEB_PROCESS_CRASHED && now - d->lastCrashReload > 30 * G_USEC_PER_SEC ) {
        d->lastCrashReload = now;
        QTimer::singleShot( 0, d->q, SLOT( reloadAfterCrash() ) );
    } else {
        emit d->q->webProcessFailed( reason == WEBKIT_WEB_PROCESS_CRASHED );
    }
}

void KWPEWidgetPrivate::onInsecureContent( WebKitWebView*, WebKitInsecureContentEvent, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( d->insecureContent )
        return;
    d->insecureContent = true;
    emit d->q->insecureContentDetected();
}

// HTTP (and proxy) authentication: KDE's password dialog.  Nothing is
// stored yet: KWallet comes with the password manager (see onSubmitForm).
gboolean KWPEWidgetPrivate::onAuthenticate( WebKitWebView*, WebKitAuthenticationRequest* request, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // A message must not ask for passwords (a remote image behind HTTP
    // authentication, a phishing attempt).
    if ( d->isMail() ) {
        webkit_authentication_request_cancel( request );
        return TRUE;
    }
    switch ( webkit_authentication_request_get_scheme( request ) ) {
    case WEBKIT_AUTHENTICATION_SCHEME_CLIENT_CERTIFICATE_REQUESTED:
    case WEBKIT_AUTHENTICATION_SCHEME_SERVER_TRUST_EVALUATION_REQUESTED:
    case WEBKIT_AUTHENTICATION_SCHEME_CLIENT_CERTIFICATE_PIN_REQUESTED:
        return FALSE;   // WebKit's default: cancelled
    default:
        break;
    }
    if ( d->authRequest )
        return FALSE;   // one dialog at a time
    d->authRequest = WEBKIT_AUTHENTICATION_REQUEST( g_object_ref( request ) );
    d->authSerial++;
    g_signal_connect( request, "cancelled", G_CALLBACK( onAuthenticationCancelled ), d );
    // The dialog runs an event loop: not inside WebKit's signal.
    QTimer::singleShot( 0, d->q, SLOT( askAuthentication() ) );
    return TRUE;
}

void KWPEWidgetPrivate::onAuthenticationCancelled( WebKitAuthenticationRequest*, gpointer data )
{
    static_cast<KWPEWidgetPrivate*>( data )->dropAuthentication();
}

void KWPEWidgetPrivate::dropAuthentication()
{
    if ( !authRequest )
        return;
    g_signal_handlers_disconnect_by_data( authRequest, this );
    g_object_unref( authRequest );
    authRequest = 0;
}

// The password manager's hook: a form is submitted, with its text fields
// (names and values, password fields included).  Stub: KWallet storage
// and filling come later; for now the form is just sent.
void KWPEWidgetPrivate::onSubmitForm( WebKitWebView*, WebKitFormSubmissionRequest* request, gpointer )
{
    webkit_form_submission_request_submit( request );
}

// alert(), confirm(), prompt(), onbeforeunload: modal to the page only
// (KWPEScriptDialog), with the page's origin as the caption.
gboolean KWPEWidgetPrivate::onScriptDialog( WebKitWebView* view, WebKitScriptDialog* dialog, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // The user said no more; or a file chooser is open (one dialog at a
    // time): answered without asking.
    if ( d->suppressDialogs || d->pageDialog || d->isMail() ) {
        d->quietDialogs.append( webkit_script_dialog_ref( dialog ) );
        QTimer::singleShot( 0, d->q, SLOT( answerQuietDialogs() ) );
        return TRUE;
    }
    KURL url( QString::fromUtf8( webkit_web_view_get_uri( view ) ) );
    QString origin = url.protocol();
    if ( !url.host().isEmpty() ) {
        origin += "://" + url.host();
        if ( url.port() )
            origin += ":" + QString::number( url.port() );
    }
    KWPEScriptDialog* box = new KWPEScriptDialog( dialog, origin, d->q );
    QObject::connect( box, SIGNAL( suppressRequested() ), d->q, SLOT( suppressDialogs() ) );
    // A navigation Konqueror asked for (openURL()) does not happen: the
    // part shows the page's URL again.
    QObject::connect( box, SIGNAL( stayedOnPage() ), d->q, SIGNAL( loadHandedOff() ) );
    d->showPageDialog( box );
    return TRUE;
}

// Answer (cancel) the page's open dialog and the queued ones.
void KWPEWidgetPrivate::closePageDialogs()
{
    delete static_cast<QWidget*>( pageDialog );
    answerQuietDialogs();
}

void KWPEWidgetPrivate::answerQuietDialogs()
{
    QValueList<WebKitScriptDialog*> dialogs = quietDialogs;
    quietDialogs.clear();
    for ( QValueList<WebKitScriptDialog*>::Iterator it = dialogs.begin(); it != dialogs.end(); ++it ) {
        KWPEScriptDialog::answerQuietly( *it );
        webkit_script_dialog_unref( *it );
    }
}

// <input type=file>: KDE's file dialog, modal to the page only.
gboolean KWPEWidgetPrivate::onRunFileChooser( WebKitWebView*, WebKitFileChooserRequest* request, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( d->pageDialog || d->isMail() )
        webkit_file_chooser_request_cancel( request );   // one at a time; none for a message
    else
        d->showPageDialog( new KWPEFileChooser( request, d->q ) );
    return TRUE;
}

// A dialog of the page: shown while the page is (a background tab shows
// it when it comes to the front), no input to the page meanwhile.
void KWPEWidgetPrivate::showPageDialog( QWidget* dialog )
{
    pageDialog = dialog;
    if ( q->isVisible() ) {
        dialog->show();
        dialog->raise();
        dialog->setActiveWindow();
    }
}

gboolean KWPEWidgetPrivate::onPermissionRequest( WebKitWebView* view, WebKitPermissionRequest* request, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    // Geolocation, camera, microphone, notifications, ...: no UI to ask
    // the user yet, so no.  App mode: the host decides.
    if ( !d->isApp() ) {
        webkit_permission_request_deny( request );
        return TRUE;
    }
    PendingPermission pending;
    pending.origin = originOf( webkit_web_view_get_uri( view ) );
    // Only the application's own sites ask (the host is not asked for
    // another page, which the main frame does not show anyway).
    if ( !d->isAppDocument( webkit_web_view_get_uri( view ) ) ) {
        webkit_permission_request_deny( request );
        return TRUE;
    }
    if ( WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST( request ) ) {
        pending.kind = QString::fromLatin1( "notifications" );
        // Answered before (WebKit asks again in a new web process).
        if ( d->app->deniedNotifications.contains( pending.origin ) ) {
            webkit_permission_request_deny( request );
            return TRUE;
        }
        if ( d->app->allowedNotifications.contains( pending.origin ) ) {
            webkit_permission_request_allow( request );
            return TRUE;
        }
    } else if ( WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST( request ) ) {
        WebKitUserMediaPermissionRequest* media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST( request );
        if ( webkit_user_media_permission_is_for_display_device( media ) )
            pending.kind = QString::fromLatin1( "screen" );
        else if ( webkit_user_media_permission_is_for_video_device( media ) )
            pending.kind = QString::fromLatin1( "camera" );
        else
            pending.kind = QString::fromLatin1( "microphone" );
    } else if ( WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST( request ) ) {
        pending.kind = QString::fromLatin1( "geolocation" );
    } else {
        pending.kind = QString::fromLatin1( "other" );
    }
    if ( pending.kind != "notifications" ) {
        // WebKit does not say which frame asks (a frame of another site
        // the application's page shows, with the permission delegated):
        // the top level's origin is not the requester's.  Refused; the
        // host only learns of it (no origin, its answer is not used).
        // Notifications: WebKit refuses a frame of another origin
        // itself, the requester is the main frame's origin.
        webkit_permission_request_deny( request );
        pending.origin = QString::null;
        pending.request = 0;
    } else {
        pending.request = WEBKIT_PERMISSION_REQUEST( g_object_ref( request ) );
    }
    d->pendingPermissions.append( pending );
    // The host may ask the user (an event loop): not inside WebKit's signal.
    QTimer::singleShot( 0, d->q, SLOT( askPermissions() ) );
    return TRUE;
}

void KWPEWidgetPrivate::onFrameDisplayed( WebKitWebView*, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( d->firstPaintPending ) {
        d->firstPaintPending = false;
        fprintf( stderr, "kwpe: first paint %.0f ms after the load started (%s)\n",
                 ( g_get_monotonic_time() - d->loadStart ) / 1000.0, d->url.utf8().data() );
    }
}

// ---------------------------------------------------------------------
// KWPEWidget

KWPEWidget::KWPEWidget( QWidget* parent, const char* name, Mode mode )
    : QWidget( parent, name, WRepaintNoErase | WResizeNoErase )
{
    d = new KWPEWidgetPrivate( this, mode );
    // WebKit paints everything; the X server must not clear the window.
    setBackgroundMode( NoBackground );
    setFocusPolicy( StrongFocus );
    setMouseTracking( true );
    setAcceptDrops( false );
    d->create();
}

KWPEWidget::KWPEWidget( QWidget* parent, const char* name, const KWPEAppOptions& options )
    : QWidget( parent, name, WRepaintNoErase | WResizeNoErase )
{
    d = new KWPEWidgetPrivate( this, App );
    d->appOptions = options;
    for ( QStringList::Iterator it = d->appOptions.sites.begin(); it != d->appOptions.sites.end(); ++it )
        *it = ( *it ).lower();
    setBackgroundMode( NoBackground );
    setFocusPolicy( StrongFocus );
    setMouseTracking( true );
    // WPE has no drag and drop: local files dropped here go to the page
    // as a drop of its own (dropEvent()).
    setAcceptDrops( true );
    d->create();
}

KWPEWidget::~KWPEWidget()
{
    d->destroy();
    if ( d->isMail() ) {
        d->forgetMailHosts( true );
        if ( d->mailExternalSession )
            g_object_unref( d->mailExternalSession );
    }
    delete d;
}

bool KWPEWidget::isValid() const
{
    return d->view != 0;
}

void KWPEWidget::load( const QString& url )
{
    // A response of the previous page not reported yet: superseded.
    d->unsupportedUrl = QString::null;
    if ( !d->view )
        return;
    if ( d->isMail() ) {
        // The host gives files (loadMailFile()); nothing else is loaded.
        return;
    }
    if ( d->isApp() ) {
        if ( d->appOptions.sites.isEmpty() )
            d->appOptions.sites.append( KURL( url ).host().lower() );
        if ( d->isAppDocument( url.utf8().data() ) )
            d->appHome = url;
        webkit_web_view_load_uri( d->view, url.utf8().data() );
        return;
    }
    // The filters for the site, before WebKit requests the page.
    KWPEAdBlock::self()->setViewHost( d->contentManager, KURL( url ).host() );
    // A process just started: the filters are read from their store in
    // the background (milliseconds).  The first page waits for them, at
    // most a second, or its ads would come through.
    if ( !KWPEAdBlock::self()->isReady() ) {
        if ( d->deferredURL.isNull() ) {
            connect( KWPEAdBlock::self(), SIGNAL( changed() ), SLOT( adBlockChanged() ) );
            QTimer::singleShot( 1000, this, SLOT( loadDeferred() ) );
        }
        d->deferredURL = url;
        return;
    }
    d->deferredURL = QString::null;
    webkit_web_view_load_uri( d->view, url.utf8().data() );
}

KWPEWidget::Mode KWPEWidget::mode() const
{
    return d->mode;
}

void KWPEWidget::loadMailFile( const QString& path, bool loadExternal )
{
    if ( !d->view || !d->isMail() )
        return;
    d->pendingMailPath = path;
    d->pendingMailExternal = loadExternal;
    loadPendingMail();
}

bool KWPEWidget::mailAtBottom() const
{
    return d->mailAtBottom;
}

QString KWPEWidget::mailSelectedText() const
{
    return d->mailSelection;
}

void KWPEWidget::loadPendingMail()
{
    if ( d->pendingMailPath.isEmpty() || !d->view )
        return;
    if ( s_mailFilterState == MailFiltersPending ) {
        // Nothing is loaded without the content filter.
        if ( !s_mailWaiting->contains( this ) )
            s_mailWaiting->append( this );
        return;
    }
    QString path = d->pendingMailPath;
    d->pendingMailPath = QString::null;
    if ( s_mailFilterState == MailFiltersFailed ) {
        emit loadFailed( i18n( "The message cannot be shown safely (WebKit's content filter is missing)." ) );
        return;
    }
    QString file = realFilePath( path );
    if ( file.isEmpty() ) {
        emit loadFailed( i18n( "Cannot read %1" ).arg( path ) );
        return;
    }
    KWPEMailDocument doc;
    doc.dir = file.left( file.findRev( '/' ) );
    doc.mainFile = file;
    doc.external = d->pendingMailExternal;
    doc.owner = this;
    if ( doc.dir.isEmpty() )
        return;
    if ( doc.external || d->viewExternal ) {
        // A message with external references gets a new view with a new
        // ephemeral session (cookies, caches, HSTS, connections of its
        // own); the next message gets a view of the render-only session
        // again.  The old view and what it stored go.
        d->destroy();
        d->forgetMailHosts( true );
        if ( d->mailExternalSession )
            g_object_unref( d->mailExternalSession );
        d->mailExternalSession = 0;
        if ( doc.external ) {
            d->mailExternalSession = webkit_network_session_new_ephemeral();
            g_signal_connect( d->mailExternalSession, "download-started", G_CALLBACK( onMailDownloadStarted ), 0 );
            webkit_network_session_set_tls_errors_policy( d->mailExternalSession, WEBKIT_TLS_ERRORS_POLICY_FAIL );
        }
        d->viewExternal = doc.external;
        d->create();
        if ( !d->view )
            return;
    }
    if ( !d->mailFilterAdded ) {
        webkit_user_content_manager_add_filter( d->contentManager, s_mailFilter[d->viewExternal ? 1 : 0] );
        d->mailFilterAdded = true;
    }
    // A new origin for every message; this widget's view alone reads it,
    // until the view shows another one (or goes).
    QString host = QString::fromLatin1( "m%1" ).arg( ++s_mailSerial );
    s_mailDocuments->insert( host, doc );
    d->mailHosts.append( host );
    KURL url;
    url.setProtocol( mailScheme );
    url.setHost( host );
    url.setPath( file );
    d->mailHost = host;
    d->mailMainURL = url.url();
    d->mailAtBottom = true;
    d->mailSelection = QString::null;
    if ( d->statsEnabled )
        fprintf( stderr, "kwpe: mail: loading %s\n", d->mailMainURL.utf8().data() );
    webkit_web_view_load_uri( d->view, d->mailMainURL.utf8().data() );
}

void KWPEWidget::scrollBy( int dx, int dy )
{
    if ( !d->view )
        return;
    QCString script;
    script.sprintf( "window.scrollBy(%d, %d)", dx, dy );
    webkit_web_view_evaluate_javascript( d->view, script.data(), -1, d->isMail() ? mailWorld : "kwpepart", 0,
                                         d->cancellable, 0, 0 );
}

void KWPEWidget::selectAll()
{
    if ( d->view )
        webkit_web_view_execute_editing_command( d->view, WEBKIT_EDITING_COMMAND_SELECT_ALL );
}

void KWPEWidget::adBlockChanged()
{
    if ( KWPEAdBlock::self()->isReady() )
        loadDeferred();
}

void KWPEWidget::loadDeferred()
{
    disconnect( KWPEAdBlock::self(), SIGNAL( changed() ), this, SLOT( adBlockChanged() ) );
    if ( d->deferredURL.isNull() || !d->view )
        return;
    QString url = d->deferredURL;
    d->deferredURL = QString::null;
    webkit_web_view_load_uri( d->view, url.utf8().data() );
}

void KWPEWidget::downloadURL( const QString& url )
{
    // WebKit's "download-started" takes it from here (onDownloadStarted).
    if ( d->view )
        g_object_unref( webkit_web_view_download_uri( d->view, url.utf8().data() ) );
}

// Zoom steps in percent, as in other browsers.
static const int zoomSteps[] = { 30, 50, 67, 80, 90, 100, 110, 120, 133, 150, 170, 200, 240, 300 };
static const int zoomStepCount = sizeof( zoomSteps ) / sizeof( zoomSteps[0] );

int KWPEWidget::zoom() const
{
    return d->view ? int( webkit_web_view_get_zoom_level( d->view ) * 100.0 + 0.5 ) : 100;
}

void KWPEWidget::setZoom( int percent )
{
    if ( !d->view )
        return;
    percent = QMAX( zoomSteps[0], QMIN( percent, zoomSteps[zoomStepCount - 1] ) );
    if ( percent == zoom() )
        return;
    webkit_web_view_set_zoom_level( d->view, percent / 100.0 );
    emit zoomChanged( percent );
}

void KWPEWidget::zoomIn()
{
    int current = zoom();
    for ( int i = 0; i < zoomStepCount; ++i )
        if ( zoomSteps[i] > current ) {
            setZoom( zoomSteps[i] );
            return;
        }
}

void KWPEWidget::zoomOut()
{
    int current = zoom();
    for ( int i = zoomStepCount - 1; i >= 0; --i )
        if ( zoomSteps[i] < current ) {
            setZoom( zoomSteps[i] );
            return;
        }
}

bool KWPEWidget::canZoomIn() const
{
    return zoom() < zoomSteps[zoomStepCount - 1];
}

bool KWPEWidget::canZoomOut() const
{
    return zoom() > zoomSteps[0];
}

void KWPEWidget::find( const QString& text, bool caseSensitive, bool wholeWords, bool backwards )
{
    if ( !d->view )
        return;
    guint32 options = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if ( !caseSensitive )
        options |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
    if ( wholeWords )
        options |= WEBKIT_FIND_OPTIONS_AT_WORD_STARTS;
    if ( backwards )
        options |= WEBKIT_FIND_OPTIONS_BACKWARDS;
    // All matches highlighted, up to 1000 counted.
    webkit_find_controller_search( webkit_web_view_get_find_controller( d->view ), text.utf8().data(), options, 1000 );
}

void KWPEWidget::findNext( bool forward )
{
    if ( !d->view )
        return;
    WebKitFindController* finder = webkit_web_view_get_find_controller( d->view );
    if ( forward )
        webkit_find_controller_search_next( finder );
    else
        webkit_find_controller_search_previous( finder );
}

void KWPEWidget::copySelection()
{
    // The selected text into the X clipboard (Konqueror's Edit/Copy and
    // Ctrl+C, which Konqueror takes before WebKit; WebKit's own copies,
    // e.g. a page's execCommand("copy"), arrive through KWPEClipboard).
    // In a script world of our own: the page cannot replace
    // getSelection() or toString() to choose what is copied.  Selections in text fields are not in
    // getSelection(); password fields are not copied.
    static const char script[] =
        "(function() {"
        "  var e = document.activeElement;"
        "  if (e && (e.tagName == 'INPUT' || e.tagName == 'TEXTAREA')) {"
        "    if (e.type == 'password') return '';"
        "    try { return e.value.substring(e.selectionStart, e.selectionEnd); } catch (x) { return ''; }"
        "  }"
        "  return window.getSelection().toString();"
        "})()";
    // Mail: the selection may be in a frame of the message (an HTML part).
    static const char mailScript[] =
        "(function() {"
        "  function selected(w) {"
        "    var t = w.getSelection().toString();"
        "    if (t) return t;"
        "    var frames = w.document.querySelectorAll('iframe');"
        "    for (var i = 0; i < frames.length; i++) {"
        "      try { t = selected(frames[i].contentWindow); if (t) return t; } catch (e) {}"
        "    }"
        "    return '';"
        "  }"
        "  return selected(window);"
        "})()";
    if ( d->view )
        webkit_web_view_evaluate_javascript( d->view, d->isMail() ? mailScript : script, -1,
                                             d->isMail() ? mailWorld : "kwpepart", 0, d->cancellable,
                                             KWPEWidgetPrivate::onSelectionText, 0 );
}

void KWPEWidget::cut()
{
    if ( d->view )
        webkit_web_view_execute_editing_command( d->view, WEBKIT_EDITING_COMMAND_CUT );
}

void KWPEWidget::paste()
{
    if ( !d->view )
        return;
    // The X CLIPBOARD for WebKit's paste (as for Ctrl+V).
    KWPEClipboard::preparePaste( QClipboard::Clipboard );
    webkit_web_view_execute_editing_command( d->view, WEBKIT_EDITING_COMMAND_PASTE );
}

bool KWPEWidget::contextMenuOnEditable() const
{
    return d->menuEditable;
}

void KWPEWidget::activateNotification( int id )
{
    QMap<int, WebKitNotification*>::ConstIterator it = d->notifications.find( id );
    if ( it != d->notifications.end() )
        webkit_notification_clicked( it.data() );
}

// The same text of a notification and of its announcement: WebKit gives
// no tag (or body) as a null string, the bridge as an empty one (Qt's ==
// tells them apart).
static bool sameNotificationText( const QString& a, const QString& b )
{
    return a.isEmpty() ? b.isEmpty() : a == b;
}

// In the page's order; one waiting for its picture holds the later ones.
void KWPEWidget::emitNotifications()
{
    // Pictures of notifications WebKit did not show (denied): gone.
    gint64 now = g_get_monotonic_time();
    for ( QValueList<KWPEWidgetPrivate::NotificationIcon>::Iterator icon = d->notificationIcons.begin();
          icon != d->notificationIcons.end(); ) {
        if ( ( *icon ).id == 0 && now - ( *icon ).announced > 10 * G_USEC_PER_SEC ) {
            d->dropAnnouncement( *icon );
            icon = d->notificationIcons.remove( icon );
        }
        else
            ++icon;
    }
    QGuardedPtr<KWPEWidget> self( this );
    while ( self && !d->notificationsEvicted.isEmpty() ) {
        int id = d->notificationsEvicted.first();
        d->notificationsEvicted.remove( d->notificationsEvicted.begin() );
        emit notificationClosed( id );
    }
    while ( self && !d->notificationsToShow.isEmpty() ) {
        int id = d->notificationsToShow.first();
        QMap<int, WebKitNotification*>::ConstIterator it = d->notifications.find( id );
        if ( it == d->notifications.end() ) {
            d->notificationsToShow.remove( d->notificationsToShow.begin() );
            continue;   // closed meanwhile
        }
        QString title = QString::fromUtf8( webkit_notification_get_title( it.data() ) );
        QString body = QString::fromUtf8( webkit_notification_get_body( it.data() ) );
        QString tag = QString::fromUtf8( webkit_notification_get_tag( it.data() ) );
        int generation = d->notificationShownIn.contains( id ) ? d->notificationShownIn[id] : -1;
        // Its announcement: the one given to it already, else the first of
        // its document with its title, body and tag.  Several of these:
        // WebKit shows a notification once its icon is loaded, so two
        // alike may come in either order; none of them gets a picture
        // (never another one's).
        QValueList<KWPEWidgetPrivate::NotificationIcon>::Iterator icon = d->notificationIcons.begin();
        for ( ; icon != d->notificationIcons.end(); ++icon )
            if ( ( *icon ).id == id )
                break;
        if ( icon == d->notificationIcons.end() ) {
            int alike = 0;
            for ( QValueList<KWPEWidgetPrivate::NotificationIcon>::Iterator it = d->notificationIcons.begin();
                  it != d->notificationIcons.end(); ++it )
                if ( ( *it ).id == 0 && ( *it ).generation == generation
                     && sameNotificationText( ( *it ).title, title ) && sameNotificationText( ( *it ).body, body )
                     && sameNotificationText( ( *it ).tag, tag ) ) {
                    if ( !alike++ )
                        icon = it;
                    else
                        ( *it ).ambiguous = true;
                }
            // (several alike, or one alike dropped, or the dropped ones no
            // longer known: whose is not known)
            if ( alike > 1 || ( alike == 1 && d->wasDropped( KWPEWidgetPrivate::announcementKey( title, body, tag ) ) ) )
                ( *icon ).ambiguous = true;
        }
        QImage image;
        if ( icon != d->notificationIcons.end() ) {
            ( *icon ).id = id;
            if ( !( *icon ).done && !( *icon ).ambiguous ) {
                // 2 seconds after WebKit showed it, fixed then.
                gint64 shown = d->notificationShownAt.contains( id ) ? d->notificationShownAt[id] : now;
                gint64 left = shown + 2 * G_USEC_PER_SEC - now;
                if ( left > 0 ) {
                    QTimer::singleShot( int( left / 1000 ) + 10, this, SLOT( emitNotifications() ) );
                    return;
                }
            }
            if ( !( *icon ).ambiguous )
                image = ( *icon ).image;
            d->notificationIcons.remove( icon );
        }
        d->notificationsToShow.remove( d->notificationsToShow.begin() );
        d->notificationShownAt.remove( id );
        d->notificationShownIn.remove( id );
        emit notificationShown( id, title, body );
        if ( self )
            emit notificationShown( id, title, body, image );
    }
}

void KWPEWidget::askPermissions()
{
    QGuardedPtr<KWPEWidget> self( this );
    while ( self && !d->pendingPermissions.isEmpty() ) {
        KWPEWidgetPrivate::PendingPermission pending = d->pendingPermissions.first();
        d->pendingPermissions.remove( d->pendingPermissions.begin() );
        bool allow = false;
        emit permissionRequested( pending.kind, pending.origin, allow );
        if ( !pending.request )
            continue;   // refused already (no requester known)
        // Answered even if the widget went meanwhile (destroy() denied
        // only those still in the list).
        if ( allow )
            webkit_permission_request_allow( pending.request );
        else
            webkit_permission_request_deny( pending.request );
        g_object_unref( pending.request );
        if ( self && pending.kind == "notifications" && !pending.origin.isEmpty() ) {
            KWPEAppWebKit* app = d->app;
            app->allowedNotifications.remove( pending.origin );
            app->deniedNotifications.remove( pending.origin );
            ( allow ? app->allowedNotifications : app->deniedNotifications ).append( pending.origin );
            saveAppPermissions( app );
        }
    }
}

void KWPEWidget::reportBlocked()
{
    QString url = d->blockedURL;
    d->blockedURL = QString::null;
    if ( !url.isEmpty() )
        emit statusMessage( i18n( "%1 is not part of this application and was not opened." )
                            .arg( KStringHandler::csqueeze( url, 80 ) ) );
}

void KWPEWidget::returnToApp()
{
    if ( !d->view || d->appHome.isEmpty() )
        return;
    // Once every 5 seconds at most (a home page that leaves again).
    gint64 now = g_get_monotonic_time();
    if ( now - d->lastReturnToApp < 5 * G_USEC_PER_SEC ) {
        // An empty page of our own instead (no app privileges either).
        d->ownBlank = true;
        webkit_web_view_load_html( d->view, "", 0 );
        emit statusMessage( i18n( "The application's page keeps leaving its site; reload it to try again." ) );
        return;
    }
    d->lastReturnToApp = now;
    webkit_web_view_load_uri( d->view, d->appHome.utf8().data() );
}

void KWPEWidget::startDownloads()
{
    QStringList urls = d->pendingDownloads;
    d->pendingDownloads.clear();
    for ( QStringList::ConstIterator it = urls.begin(); it != urls.end(); ++it )
        downloadURL( *it );
}

// ---------------------------------------------------------------------
// App mode: files dropped on the view.  WPE has no drag and drop of its
// own; the page gets the files as a drop at that point: dragenter,
// dragover and drop events with a DataTransfer of File objects, made in
// our script world (the page's listeners see them as any drop).  The
// files' bytes travel as base64 in the script's arguments: up to
// maxDropBytes together (the page's own file chooser has no such limit).

static const unsigned maxDropBytes = 64 * 1024 * 1024;

enum DropRead { DropReadOk, DropReadTooLarge, DropReadFailed };

// The whole regular file @p path, if it has at most @p limit bytes: the
// size of the opened file (64 bits), at most @p limit + 1 bytes read, a
// short or failed read is an error.
static DropRead readDroppedFile( const QString& path, unsigned long long limit, QByteArray* data, time_t* modified )
{
    int fd = ::open( QFile::encodeName( path ).data(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK );
    if ( fd < 0 )
        return DropReadFailed;
    struct stat st;
    if ( fstat( fd, &st ) != 0 || !S_ISREG( st.st_mode ) || st.st_size < 0 ) {
        ::close( fd );
        return DropReadFailed;
    }
    if ( (unsigned long long)st.st_size > limit ) {
        ::close( fd );
        return DropReadTooLarge;
    }
    size_t size = size_t( st.st_size );
    if ( !data->resize( size ) ) {
        ::close( fd );
        return DropReadFailed;
    }
    size_t done = 0;
    while ( done < size ) {
        ssize_t got = ::read( fd, data->data() + done, size - done );
        if ( got < 0 && errno == EINTR )
            continue;
        if ( got <= 0 )
            break;
        done += size_t( got );
    }
    // The file grew since fstat(): one more byte must not be there.
    char extra;
    ssize_t more;
    do
        more = ::read( fd, &extra, 1 );
    while ( more < 0 && errno == EINTR );
    ::close( fd );
    if ( done != size || more != 0 )
        return more > 0 ? DropReadTooLarge : DropReadFailed;
    *modified = st.st_mtime;
    return DropReadOk;
}

static bool droppedFiles( QMimeSource* source, QStringList* files )
{
    QStringList list;
    if ( !QUriDrag::canDecode( source ) || !QUriDrag::decodeLocalFiles( source, list ) || list.isEmpty() )
        return false;
    for ( QStringList::ConstIterator it = list.begin(); it != list.end(); ++it ) {
        QFileInfo info( *it );
        if ( !info.isFile() || !info.isReadable() )
            return false;
    }
    if ( files )
        *files = list;
    return true;
}

void KWPEWidget::dragEnterEvent( QDragEnterEvent* e )
{
    e->accept( d->isApp() && d->view && droppedFiles( e, 0 ) );
}

void KWPEWidget::dragMoveEvent( QDragMoveEvent* e )
{
    e->accept( d->isApp() && d->view && droppedFiles( e, 0 ) );
}

void KWPEWidgetPrivate::onDropDone( GObject* view, GAsyncResult* result, gpointer data )
{
    GError* error = 0;
    JSCValue* value = webkit_web_view_call_async_javascript_function_finish( WEBKIT_WEB_VIEW( view ), result, &error );
    if ( !value ) {
        // Cancelled: the widget is gone (data is too).
        if ( !g_error_matches( error, G_IO_ERROR, G_IO_ERROR_CANCELLED ) ) {
            fprintf( stderr, "kwpe: drop: %s\n", error ? error->message : "?" );
            emit static_cast<KWPEWidgetPrivate*>( data )->q->statusMessage( i18n( "The files could not be dropped on the page." ) );
        }
        g_clear_error( &error );
        return;
    }
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    if ( !jsc_value_to_boolean( value ) )
        emit d->q->statusMessage( i18n( "The page did not take the dropped files." ) );
    g_object_unref( value );
}

void KWPEWidget::dropEvent( QDropEvent* e )
{
    QStringList files;
    // Only the application's own page gets the user's files.
    if ( !d->isApp() || !d->view || !droppedFiles( e, &files ) || !d->isAppDocument( webkit_web_view_get_uri( d->view ) ) ) {
        e->ignore();
        return;
    }
    e->accept();
    static const char script[] =
        "var dt = new DataTransfer(), keys = Object.keys(files).sort(function(a, b) { return a - b; });"
        "for (var i = 0; i < keys.length; i++) {"
        "  var f = files[keys[i]], bytes;"
        "  if (Uint8Array.fromBase64) {"
        "    bytes = Uint8Array.fromBase64(f.data);"
        "  } else {"
        "    var bin = atob(f.data);"
        "    bytes = new Uint8Array(bin.length);"
        "    for (var j = 0; j < bin.length; j++) bytes[j] = bin.charCodeAt(j);"
        "  }"
        "  dt.items.add(new File([bytes], f.name, { type: f.type, lastModified: f.modified }));"
        "}"
        "var target = document.elementFromPoint(x, y) || document.body || document.documentElement;"
        "function fire(type) {"
        "  var e = new DragEvent(type, { bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y,"
        "                               dataTransfer: dt });"
        "  return target.dispatchEvent(e);"
        "}"
        "fire('dragenter');"
        "fire('dragover');"
        // Taken: the page called preventDefault() on the drop.
        "return !fire('drop');";

    GVariantBuilder list;
    g_variant_builder_init( &list, G_VARIANT_TYPE( "a{sv}" ) );
    unsigned long long total = 0;
    int n = 0;
    for ( QStringList::ConstIterator it = files.begin(); it != files.end(); ++it ) {
        QFileInfo info( *it );
        QByteArray data;
        time_t modified = 0;
        // What is read is bounded, not what the name said a moment ago.
        DropRead result = readDroppedFile( *it, maxDropBytes - total, &data, &modified );
        if ( result != DropReadOk ) {
            if ( result == DropReadTooLarge )
                emit statusMessage( i18n( "Files of more than %1 MB cannot be dropped here; use the page's own button "
                                          "to attach them." ).arg( maxDropBytes / 1024 / 1024 ) );
            else
                emit statusMessage( i18n( "Cannot read %1" ).arg( *it ) );
            g_variant_builder_clear( &list );
            return;
        }
        total += data.size();
        QCString base64 = KCodecs::base64Encode( data, false );
        QString mimeType = KMimeType::findByPath( *it, 0, false )->name();
        if ( mimeType == KMimeType::defaultMimeType() )
            mimeType = QString::null;
        GVariantBuilder entry;
        g_variant_builder_init( &entry, G_VARIANT_TYPE( "a{sv}" ) );
        g_variant_builder_add( &entry, "{sv}", "name", g_variant_new_string( info.fileName().utf8().data() ) );
        g_variant_builder_add( &entry, "{sv}", "type", g_variant_new_string( mimeType.utf8().data() ) );
        g_variant_builder_add( &entry, "{sv}", "modified", g_variant_new_double( double( modified ) * 1000.0 ) );
        g_variant_builder_add( &entry, "{sv}", "data", g_variant_new_string( base64.isNull() ? "" : base64.data() ) );
        g_variant_builder_add( &list, "{sv}", QString::number( n++ ).latin1(), g_variant_builder_end( &entry ) );
    }
    // CSS pixels of the page's viewport
    double zoomLevel = webkit_web_view_get_zoom_level( d->view );
    if ( zoomLevel <= 0 )
        zoomLevel = 1.0;
    GVariantBuilder arguments;
    g_variant_builder_init( &arguments, G_VARIANT_TYPE( "a{sv}" ) );
    g_variant_builder_add( &arguments, "{sv}", "files", g_variant_builder_end( &list ) );
    g_variant_builder_add( &arguments, "{sv}", "x", g_variant_new_double( e->pos().x() / zoomLevel ) );
    g_variant_builder_add( &arguments, "{sv}", "y", g_variant_new_double( e->pos().y() / zoomLevel ) );
    // WebKit reads the arguments and does not take them: ours to free.
    GVariant* args = g_variant_ref_sink( g_variant_builder_end( &arguments ) );
    webkit_web_view_call_async_javascript_function( d->view, script, -1, args, "kwpepart", 0,
                                                    d->cancellable, KWPEWidgetPrivate::onDropDone, d );
    g_variant_unref( args );
    setActiveWindow();
    setFocus();
}

void KWPEWidget::requestSource()
{
    WebKitWebResource* resource = d->view ? webkit_web_view_get_main_resource( d->view ) : 0;
    if ( resource )
        webkit_web_resource_get_data( resource, d->cancellable, KWPEWidgetPrivate::onSourceData, d );
}

void KWPEWidget::askAuthentication()
{
    WebKitAuthenticationRequest* request = d->authRequest;
    if ( !request )
        return;
    QString realm = QString::fromUtf8( webkit_authentication_request_get_realm( request ) );
    QString host = QString::fromUtf8( webkit_authentication_request_get_host( request ) );
    QString user, password;
    WebKitCredential* proposed = webkit_authentication_request_get_proposed_credential( request );
    if ( proposed ) {
        user = QString::fromUtf8( webkit_credential_get_username( proposed ) );
        webkit_credential_free( proposed );
    }
    QString prompt = webkit_authentication_request_is_for_proxy( request )
        ? i18n( "The proxy %1 needs a user name and a password." ).arg( host )
        : i18n( "%1 needs a user name and a password." ).arg( host );
    if ( webkit_authentication_request_is_retry( request ) )
        prompt = i18n( "Authentication failed." ) + " " + prompt;

    QGuardedPtr<KWPEWidget> self( this );
    unsigned serial = d->authSerial;
    int result = KIO::PasswordDialog::getNameAndPassword( user, password, 0, prompt, false,
                                                         i18n( "Authorization Dialog" ),
                                                         realm, i18n( "Site:" ) );
    // Gone meanwhile: destroy() cancelled the request; or WebKit did
    // (and another request, maybe of another site, may have come since:
    // these credentials are not for it).
    if ( !self || d->authRequest != request || d->authSerial != serial )
        return;
    if ( result == QDialog::Accepted ) {
        WebKitCredential* credential = webkit_credential_new( user.utf8().data(), password.utf8().data(),
                                                              WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION );
        webkit_authentication_request_authenticate( request, credential );
        webkit_credential_free( credential );
    } else {
        webkit_authentication_request_cancel( request );
    }
    d->dropAuthentication();
}

void KWPEWidget::reloadForFilters()
{
    if ( d->view )
        webkit_web_view_reload( d->view );
}

void KWPEWidget::reloadAfterCrash()
{
    if ( d->view )
        webkit_web_view_reload( d->view );
}

void KWPEWidget::emitUnsupportedContent()
{
    QString url = d->unsupportedUrl;
    QString mimeType = d->unsupportedMimeType;
    d->unsupportedUrl = d->unsupportedMimeType = QString::null;
    if ( !url.isEmpty() )
        emit unsupportedContent( url, mimeType );
}

void KWPEWidget::suppressDialogs()
{
    d->suppressDialogs = true;
}

void KWPEWidget::answerQuietDialogs()
{
    d->answerQuietDialogs();
}

void KWPEWidget::emitExternalURL()
{
    if ( d->isMail() || d->isApp() ) {
        // One link per click; the host may delete us while it handles it.
        QStringList links = d->activatedLinks;
        d->activatedLinks.clear();
        if ( !links.isEmpty() )
            emit linkActivated( links.first() );
        return;
    }
    QString url = d->externalUrl;
    d->externalUrl = QString::null;
    if ( !url.isEmpty() )
        emit externalURLRequested( url );
}

void KWPEWidget::showContextMenu()
{
    emit contextMenuRequested( d->menuLink, d->menuImage, d->menuSelection, QCursor::pos() );
}

void KWPEWidget::reload( bool bypassCache )
{
    if ( !d->view )
        return;
    if ( bypassCache )
        webkit_web_view_reload_bypass_cache( d->view );
    else
        webkit_web_view_reload( d->view );
}

void KWPEWidget::stop()
{
    d->deferredURL = QString::null;
    if ( d->view )
        webkit_web_view_stop_loading( d->view );
}

QString KWPEWidget::url() const
{
    return d->url;
}

QString KWPEWidget::title() const
{
    return d->title;
}

bool KWPEWidget::isLoading() const
{
    return d->loading || ( d->view && webkit_web_view_is_loading( d->view ) );
}

bool KWPEWidget::hasInsecureContent() const
{
    return d->insecureContent;
}

bool KWPEWidget::isSecure() const
{
    GTlsCertificate* certificate = 0;
    GTlsCertificateFlags errors = GTlsCertificateFlags( 0 );
    return d->view && webkit_web_view_get_tls_info( d->view, &certificate, &errors ) && certificate && !errors;
}

int KWPEWidget::currentHistoryToken()
{
    if ( !d->view )
        return 0;
    return d->tokenForItem( webkit_back_forward_list_get_current_item( webkit_web_view_get_back_forward_list( d->view ) ), 0 );
}

KWPEWidget::HistoryResult KWPEWidget::goToHistoryToken( int token, const QString& url )
{
    QMap<int, WebKitBackForwardListItem*>::ConstIterator it = d->itemOfToken.find( token );
    if ( !d->view || it == d->itemOfToken.end() )
        return HistoryUnknown;
    // A token of another page (should not happen: tokens are unique in
    // the process): load the URL instead.
    if ( !urlcmp( QString::fromUtf8( webkit_back_forward_list_item_get_uri( it.data() ) ), url, true, false ) )
        return HistoryUnknown;
    WebKitBackForwardList* list = webkit_web_view_get_back_forward_list( d->view );
    if ( it.data() == webkit_back_forward_list_get_current_item( list ) )
        return HistoryCurrent;
    d->ownBackForward = true;
    if ( d->isBrowser() )
        KWPEAdBlock::self()->setViewHost( d->contentManager, KURL( url ).host() );
    webkit_web_view_go_to_back_forward_list_item( d->view, it.data() );
    return HistoryNavigating;
}

void KWPEWidget::applySettings()
{
    if ( !s_settings && !s_mailSettings && !s_apps )
        return;
    // Animated scrolling lags behind a fast wheel (every notch extends the
    // animation): off unless asked for.  "When efficient": with the GPU
    // path, where frames cost little.
    KWPESettings::SmoothScrolling smooth = KWPESettings::smoothScrolling();
    bool on = smooth == KWPESettings::SmoothAlways
              || ( smooth == KWPESettings::SmoothWhenEfficient && KWPE::bufferPath() == KWPE::PathDMABuf );
    if ( s_settings )
        webkit_settings_set_enable_smooth_scrolling( s_settings, on );
    if ( s_mailSettings )
        webkit_settings_set_enable_smooth_scrolling( s_mailSettings, on );
    if ( s_apps )
        for ( KWPEAppWebKits::ConstIterator it = s_apps->begin(); it != s_apps->end(); ++it )
            webkit_settings_set_enable_smooth_scrolling( it.data()->settings, on );
}

QString KWPEWidget::bufferPathName()
{
    return KWPE::bufferPath() == KWPE::PathDMABuf ? QString::fromLatin1( "DMA-BUF" ) : QString::fromLatin1( "SHM" );
}

bool KWPEWidget::event( QEvent* e )
{
    // A dialog of the page is open: the page takes no input.
    if ( d->pageDialog ) {
        switch ( e->type() ) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            return true;
        default:
            break;
        }
    }
    switch ( e->type() ) {
    case QEvent::Reparent:
        // Qt 3 made a new X window.
        d->attach();
        break;
    case QEvent::AccelOverride: {
        // Keys without Ctrl/Alt/Meta are for the page (typing in forms,
        // scrolling), not for single-key shortcuts; F-keys and Escape stay
        // with Konqueror (reload, stop).
        QKeyEvent* k = static_cast<QKeyEvent*>( e );
        int key = k->key();
        bool modified = k->state() & ( ControlButton | AltButton | MetaButton );
        bool functionKey = ( key >= Key_F1 && key <= Key_F35 ) || key == Key_Escape;
        // Mail: a message has no fields to type into; KMail's single-key
        // shortcuts (next message, delete, scrolling) stay KMail's.
        if ( !modified && !functionKey && !d->isMail() ) {
            k->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QWidget::event( e );
}

void KWPEWidget::paintEvent( QPaintEvent* e )
{
    if ( !d->wpeView )
        return;
    QRect r = e->rect();
    KWPE::exposeView( d->wpeView, r.x(), r.y(), r.width(), r.height() );
}

void KWPEWidget::resizeEvent( QResizeEvent* e )
{
    if ( !d->wpeView )
        return;
    wpe_toplevel_resized( d->toplevel, e->size().width(), e->size().height() );
    wpe_view_resized( d->wpeView, e->size().width(), e->size().height() );
}

// KWPE_NETLOG: what the page's view went through (shown, hidden, active)
static void netNote( WebKitWebView* view, const char* what )
{
    if ( !KWPENetLog::enabled() || !view )
        return;
    // the host only, as sent (encoded), nothing unprintable
    const char* uri = webkit_web_view_get_uri( view );
    GUri* parsed = uri ? g_uri_parse( uri, G_URI_FLAGS_ENCODED, 0 ) : 0;
    QCString host( parsed && g_uri_get_host( parsed ) ? g_uri_get_host( parsed ) : "-" );
    if ( parsed )
        g_uri_unref( parsed );
    host.truncate( 100 );
    for ( uint i = 0; i < host.length(); ++i )
        if ( (unsigned char) host[i] < 0x20 || (unsigned char) host[i] >= 0x7f )
            host[i] = '?';
    KWPENetLog::note( "view %p (%s): %s", static_cast<void*>( view ), host.data(), what );
}

void KWPEWidget::showEvent( QShowEvent* )
{
    if ( !d->wpeView )
        return;
    d->attach();
    wpe_view_map( d->wpeView );
    netNote( d->view, isActiveWindow() ? "shown, window active" : "shown, window inactive" );
    if ( d->pageDialog && !d->pageDialog->isVisible() )
        d->showPageDialog( d->pageDialog );
    wpe_toplevel_state_changed( d->toplevel, isActiveWindow() ? WPE_TOPLEVEL_STATE_ACTIVE : WPE_TOPLEVEL_STATE_NONE );
    // WebKit takes the pointer as inside until it is told otherwise (the
    // overlay scrollbars stay shown and keep repainting).
    if ( !rect().contains( mapFromGlobal( QCursor::pos() ) ) )
        leaveEvent( 0 );
}

void KWPEWidget::hideEvent( QHideEvent* )
{
    // Another tab: the page's dialog waits for this one to come back.
    if ( d->pageDialog )
        d->pageDialog->hide();
    if ( d->wpeView )
        wpe_view_unmap( d->wpeView );
    netNote( d->view, "hidden (the page is told it is not visible)" );
}

void KWPEWidget::windowActivationChange( bool )
{
    netNote( d->view, isActiveWindow() ? "window active" : "window inactive" );
    if ( d->toplevel )
        wpe_toplevel_state_changed( d->toplevel, isActiveWindow() ? WPE_TOPLEVEL_STATE_ACTIVE : WPE_TOPLEVEL_STATE_NONE );
}

void KWPEWidget::focusInEvent( QFocusEvent* )
{
    if ( d->wpeView )
        wpe_view_focus_in( d->wpeView );
}

void KWPEWidget::focusOutEvent( QFocusEvent* )
{
    if ( d->wpeView )
        wpe_view_focus_out( d->wpeView );
}

bool KWPEWidget::focusNextPrevChild( bool )
{
    // Tab moves between the page's links and fields.
    return false;
}

static guint wpeModifiers( int state )
{
    guint modifiers = 0;
    if ( state & Qt::ShiftButton )
        modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if ( state & Qt::ControlButton )
        modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if ( state & Qt::AltButton )
        modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
    if ( state & Qt::MetaButton )
        modifiers |= WPE_MODIFIER_KEYBOARD_META;
    if ( state & Qt::LeftButton )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON1;
    if ( state & Qt::MidButton )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON2;
    if ( state & Qt::RightButton )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON3;
    return modifiers;
}

// A key event without an X event behind it (an input method's text):
// the keysym from the Qt key or the text.
static guint keyvalFromQt( const QKeyEvent* e )
{
    static const struct { int qt; guint keyval; } keys[] = {
        { Qt::Key_Escape, WPE_KEY_Escape }, { Qt::Key_Tab, WPE_KEY_Tab }, { Qt::Key_Backtab, WPE_KEY_ISO_Left_Tab },
        { Qt::Key_Backspace, WPE_KEY_BackSpace }, { Qt::Key_Return, WPE_KEY_Return }, { Qt::Key_Enter, WPE_KEY_KP_Enter },
        { Qt::Key_Insert, WPE_KEY_Insert }, { Qt::Key_Delete, WPE_KEY_Delete }, { Qt::Key_Home, WPE_KEY_Home },
        { Qt::Key_End, WPE_KEY_End }, { Qt::Key_Left, WPE_KEY_Left }, { Qt::Key_Up, WPE_KEY_Up },
        { Qt::Key_Right, WPE_KEY_Right }, { Qt::Key_Down, WPE_KEY_Down }, { Qt::Key_Prior, WPE_KEY_Page_Up },
        { Qt::Key_Next, WPE_KEY_Page_Down },
    };
    for ( unsigned i = 0; i < sizeof( keys ) / sizeof( keys[0] ); ++i )
        if ( e->key() == keys[i].qt )
            return keys[i].keyval;
    if ( e->text().length() == 1 ) {
        uint ucs = e->text()[0].unicode();
        return ucs < 0x100 ? ucs : 0x01000000 | ucs;
    }
    return 0;
}

static void sendQtKey( WPEView* view, QKeyEvent* e, bool press )
{
    guint keyval = keyvalFromQt( e );
    if ( !keyval )
        return;
    WPEEvent* event = wpe_event_keyboard_new( press ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, view,
                                              WPE_INPUT_SOURCE_KEYBOARD, guint32( qt_x_time ),
                                              WPEModifiers( wpeModifiers( e->state() ) ), 0, keyval );
    wpe_view_event( view, event );
    wpe_event_unref( event );
}

void KWPEWidget::keyPressEvent( QKeyEvent* e )
{
    if ( !d->wpeView )
        return;
    // WebKit's paste keys (Ctrl+V, Ctrl+Shift+V, Shift+Insert): the X
    // CLIPBOARD may be read now.
    int mods = e->state() & ( ShiftButton | ControlButton | AltButton | MetaButton );
    if ( ( e->key() == Key_V && ( mods == ControlButton || mods == ( ControlButton | ShiftButton ) ) )
         || ( e->key() == Key_Insert && mods == ShiftButton ) )
        KWPEClipboard::preparePaste( QClipboard::Clipboard );
    if ( !KWPE::sendKeyEvent( d->wpeView, true ) )
        sendQtKey( d->wpeView, e, true );
    e->accept();
}

void KWPEWidget::keyReleaseEvent( QKeyEvent* e )
{
    if ( !d->wpeView )
        return;
    if ( !KWPE::sendKeyEvent( d->wpeView, false ) )
        sendQtKey( d->wpeView, e, false );
    e->accept();
}

static guint wpeButton( int button )
{
    switch ( button ) {
    case Qt::LeftButton: return WPE_BUTTON_PRIMARY;
    case Qt::MidButton: return WPE_BUTTON_MIDDLE;
    case Qt::RightButton: return WPE_BUTTON_SECONDARY;
    default: return 0;
    }
}

void KWPEWidget::mousePressEvent( QMouseEvent* e )
{
    guint button = wpeButton( e->button() );
    if ( !d->wpeView || !button )
        return;
    if ( !hasFocus() )
        setFocus();
    guint32 time = d->eventTime();
    guint count = wpe_view_compute_press_count( d->wpeView, e->x(), e->y(), button, time );
    WPEEvent* event = wpe_event_pointer_button_new( WPE_EVENT_POINTER_DOWN, d->wpeView, WPE_INPUT_SOURCE_MOUSE, time,
                                                    WPEModifiers( wpeModifiers( e->stateAfter() ) ), button,
                                                    e->x(), e->y(), count );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
}

void KWPEWidget::mouseDoubleClickEvent( QMouseEvent* e )
{
    // Qt 3 turns the second press into a double click; WebKit counts itself.
    mousePressEvent( e );
}

void KWPEWidget::mouseReleaseEvent( QMouseEvent* e )
{
    guint button = wpeButton( e->button() );
    if ( !d->wpeView || !button )
        return;
    WPEEvent* event = wpe_event_pointer_button_new( WPE_EVENT_POINTER_UP, d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                                    WPEModifiers( wpeModifiers( e->stateAfter() ) ), button,
                                                    e->x(), e->y(), 0 );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
    // X's middle click paste into a text field (WebKit does it for GTK
    // only): PRIMARY, at the caret the click placed.
    if ( e->button() == MidButton && d->overEditable && KWPEClipboard::preparePaste( QClipboard::Selection ) )
        webkit_web_view_execute_editing_command( d->view, WEBKIT_EDITING_COMMAND_PASTE );
}

static QPoint s_lastPointer;

void KWPEWidget::mouseMoveEvent( QMouseEvent* e )
{
    if ( !d->wpeView )
        return;
    QPoint delta = e->pos() - s_lastPointer;
    s_lastPointer = e->pos();
    WPEEvent* event = wpe_event_pointer_move_new( WPE_EVENT_POINTER_MOVE, d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                                  WPEModifiers( wpeModifiers( e->state() ) ),
                                                  e->x(), e->y(), delta.x(), delta.y() );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
}

void KWPEWidget::wheelEvent( QWheelEvent* e )
{
    if ( !d->wpeView )
        return;
    // Ctrl+wheel zooms, as in KHTML.
    if ( e->state() & ControlButton ) {
        if ( e->delta() > 0 )
            zoomIn();
        else if ( e->delta() < 0 )
            zoomOut();
        e->accept();
        return;
    }
    // Wheel notches (120 per notch, positive away from the user) as
    // WebKit's discrete steps (positive: scroll up/left), which it scrolls
    // by a "line step" of view height^(2/3) pixels (100 for a 1000 pixel
    // high view) and animates if smooth scrolling is on.  KDE's "scroll by
    // N lines" (3 by default) scales them.  Not precise deltas: WebKit
    // would report 120 * delta / 2.5 as the DOM's wheelDelta.  Pixel-
    // precise XI2.1 scrolling: later.
    double steps = e->delta() / 120.0 * QApplication::wheelScrollLines() / 3.0;
    bool horizontal = e->orientation() == Qt::Horizontal;
    WPEEvent* event = wpe_event_scroll_new( d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                            WPEModifiers( wpeModifiers( e->state() ) ),
                                            horizontal ? steps : 0.0, horizontal ? 0.0 : steps,
                                            FALSE, FALSE, e->x(), e->y() );
    if ( d->statsEnabled )
        KWPE::noteInput( d->wpeView );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
    e->accept();
}

void KWPEWidget::enterEvent( QEvent* )
{
    if ( !d->wpeView )
        return;
    QPoint pos = mapFromGlobal( QCursor::pos() );
    s_lastPointer = pos;
    WPEEvent* event = wpe_event_pointer_move_new( WPE_EVENT_POINTER_ENTER, d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                                  WPEModifiers( 0 ), pos.x(), pos.y(), 0, 0 );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
}

void KWPEWidget::leaveEvent( QEvent* )
{
    if ( !d->wpeView )
        return;
    WPEEvent* event = wpe_event_pointer_move_new( WPE_EVENT_POINTER_LEAVE, d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                                  WPEModifiers( 0 ), -1, -1, 0, 0 );
    wpe_view_event( d->wpeView, event );
    wpe_event_unref( event );
}

void KWPEWidget::printFrameStats()
{
    if ( !d->wpeView )
        return;
    KWPE::FrameStats s = KWPE::takeFrameStats( d->wpeView );
    if ( s.frames )
        fprintf( stderr, "kwpe: %s %u fps (%u DMA-BUF), latency %.1f ms, max gap %.1f ms, copy %.2f ms, damage %.2f Mpx\n",
                 bufferPathName().latin1(), s.frames, s.dmabufFrames, s.latencyMs, s.maxIntervalMs, s.copyMs,
                 s.damagePixels / 1e6 );
    if ( s.inputFrames )
        fprintf( stderr, "kwpe: wheel -> frame on screen: %.1f ms average, %.1f ms max (%u frames)\n",
                 s.inputLatencyMs, s.inputLatencyMaxMs, s.inputFrames );
    if ( s.bursts )
        fprintf( stderr, "kwpe: last wheel event -> page at rest: %.1f ms average, %.1f ms max (%u bursts)\n",
                 s.settleMs, s.settleMaxMs, s.bursts );
}

#include "kwpewidget.moc"
