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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qapplication.h>
#include <qclipboard.h>
#include <qcursor.h>
#include <qfile.h>
#include <qmap.h>
#include <qtimer.h>
#include <qevent.h>

#include <kglobal.h>
#include <kurl.h>
#include <kstandarddirs.h>

#include "kwpedownload.h"
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

// Once per process.  The module is never unloaded (__kde_do_not_unload
// in the part: GObject types and the X event filter cannot go away).
static bool ensureWebKit()
{
    if ( s_context )
        return true;

    // The display first: WebKit uses the WPEPlatform API as soon as the
    // WPEDisplay type exists.
    s_display = KWPE::x11Display( qt_xdisplay() );
    if ( !s_display )
        return false;

    s_previousFilter = qt_set_x11_event_filter( kwpeX11EventFilter );

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

    s_context = webkit_web_context_new();
    webkit_web_context_set_cache_model( s_context, WEBKIT_CACHE_MODEL_WEB_BROWSER );

    s_settings = webkit_settings_new();
    webkit_settings_set_enable_smooth_scrolling( s_settings, TRUE );
    webkit_settings_set_enable_developer_extras( s_settings, envFlag( "KWPE_DEVELOPER_EXTRAS" ) );
    // WebKit 2.54 drops the contents of GPU canvases of pages kept in the
    // back/forward cache: going back shows them empty.  Skia's CPU canvas
    // keeps them (KWPE_CANVAS_ACCELERATION=1 to compare).
    webkit_settings_set_enable_2d_canvas_acceleration( s_settings, envFlag( "KWPE_CANVAS_ACCELERATION" ) );
    return true;
}

// ---------------------------------------------------------------------
// KWPEWidgetPrivate

class KWPEWidgetPrivate : public KWPE::ViewHost
{
public:
    KWPEWidgetPrivate( KWPEWidget* widget )
        : q( widget ), view( 0 ), wpeView( 0 ), toplevel( 0 ), loading( false ), failed( false ), ownBackForward( false ), shownItem( 0 ),
          menuSelection( false ), canCopy( false ), cancellable( 0 ), statsTimer( 0 ), loadStart( 0 ), firstPaintPending( false ), statsEnabled( false )
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
    static void onBackForwardChanged( WebKitBackForwardList*, WebKitBackForwardListItem*, GList* removed, gpointer data );
    static void onFrameDisplayed( WebKitWebView*, gpointer data );
    static gboolean onContextMenu( WebKitWebView*, WebKitContextMenu*, WebKitHitTestResult* hit, gpointer data );
    static void onEditorStateChanged( WebKitEditorState* state, gpointer data );
    static void onSelectionText( GObject* view, GAsyncResult* result, gpointer data );
    static void onFoundText( WebKitFindController*, guint matchCount, gpointer data );
    static void onFailedToFindText( WebKitFindController*, gpointer data );

    KWPEWidget* q;
    WebKitWebView* view;
    WPEView* wpeView;
    WPEToplevel* toplevel;
    QString url;
    QString title;
    bool loading;
    bool failed;           // the current load failed (WebKit still sends LOAD_FINISHED)
    bool ownBackForward;   // goToHistoryToken() asked for this back/forward navigation
    WebKitBackForwardListItem* shownItem;   // back/forward item of the page shown (no reference)

    // history tokens <-> WebKit back/forward items (referenced)
    QMap<int, WebKitBackForwardListItem*> itemOfToken;
    QMap<WebKitBackForwardListItem*, int> tokenOfItem;

    // the context menu WebKit asked for (shown outside its signal)
    QString menuLink;
    QString menuImage;
    bool menuSelection;
    bool canCopy;
    GCancellable* cancellable;   // pending JavaScript calls; cancelled with the view

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
    if ( !ensureWebKit() )
        return;

    view = WEBKIT_WEB_VIEW( g_object_new( WEBKIT_TYPE_WEB_VIEW,
                                          "display", s_display,
                                          "web-context", s_context,
                                          "network-session", s_session,
                                          "settings", s_settings,
                                          NULL ) );
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
    g_signal_connect( webkit_web_view_get_editor_state( view ), "changed", G_CALLBACK( onEditorStateChanged ), this );
    cancellable = g_cancellable_new();
    WebKitFindController* finder = webkit_web_view_get_find_controller( view );
    g_signal_connect( finder, "found-text", G_CALLBACK( onFoundText ), this );
    g_signal_connect( finder, "failed-to-find-text", G_CALLBACK( onFailedToFindText ), this );

    statsEnabled = envFlag( "KWPE_STATS" );
    if ( statsEnabled ) {
        webkit_web_view_add_frame_displayed_callback( view, onFrameDisplayed, this, 0 );
        statsTimer = new QTimer( q );
        QObject::connect( statsTimer, SIGNAL( timeout() ), q, SLOT( printFrameStats() ) );
        statsTimer->start( 1000 );
    }

    attach();
}

void KWPEWidgetPrivate::destroy()
{
    if ( !view )
        return;
    g_signal_handlers_disconnect_by_data( webkit_web_view_get_back_forward_list( view ), this );
    g_signal_handlers_disconnect_by_data( webkit_web_view_get_editor_state( view ), this );
    g_signal_handlers_disconnect_by_data( webkit_web_view_get_find_controller( view ), this );
    g_cancellable_cancel( cancellable );
    g_object_unref( cancellable );
    cancellable = 0;
    g_signal_handlers_disconnect_by_data( view, this );
    g_object_set_data( G_OBJECT( view ), widgetKey, 0 );
    KWPE::attachView( wpeView, 0, 0 );
    forgetItems( 0 );
    webkit_web_view_try_close( view );
    g_object_unref( view );
    g_object_unref( toplevel );
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
        d->loading = true;
        d->failed = false;
        d->loadStart = g_get_monotonic_time();
        emit d->q->loadStarted();
        break;
    case WEBKIT_LOAD_REDIRECTED:
        break;
    case WEBKIT_LOAD_COMMITTED: {
        bool isNew = false;
        d->ownBackForward = false;
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
    QString link;
    if ( webkit_hit_test_result_context_is_link( hit ) )
        link = QString::fromUtf8( webkit_hit_test_result_get_link_uri( hit ) );
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

gboolean KWPEWidgetPrivate::onDecidePolicy( WebKitWebView* view, WebKitPolicyDecision* decision,
                                            WebKitPolicyDecisionType type, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );

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
        return FALSE;
    }

    if ( type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE ) {
        WebKitResponsePolicyDecision* response = WEBKIT_RESPONSE_POLICY_DECISION( decision );
        if ( webkit_response_policy_decision_is_main_frame_main_resource( response )
             && !webkit_response_policy_decision_is_mime_type_supported( response ) ) {
            // The answer to a POST cannot be fetched again by Konqueror:
            // WebKit downloads it.
            const char* method = webkit_uri_request_get_http_method( webkit_response_policy_decision_get_request( response ) );
            if ( method && g_ascii_strcasecmp( method, "GET" ) != 0 ) {
                webkit_policy_decision_download( decision );
                return TRUE;
            }
            WebKitURIResponse* r = webkit_response_policy_decision_get_response( response );
            QString uri = QString::fromUtf8( webkit_uri_response_get_uri( r ) );
            QString mimeType = QString::fromLatin1( webkit_uri_response_get_mime_type( r ) );
            webkit_policy_decision_ignore( decision );
            emit d->q->unsupportedContent( uri, mimeType );
            return TRUE;
        }
    }
    return FALSE;
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
    // Konqueror's popup menu runs an event loop: not inside WebKit's signal.
    QTimer::singleShot( 0, d->q, SLOT( showContextMenu() ) );
    return TRUE;   // WPE has no menu of its own
}

void KWPEWidgetPrivate::onEditorStateChanged( WebKitEditorState* state, gpointer data )
{
    KWPEWidgetPrivate* d = static_cast<KWPEWidgetPrivate*>( data );
    bool canCopy = webkit_editor_state_is_copy_available( state );
    if ( canCopy != d->canCopy ) {
        d->canCopy = canCopy;
        emit d->q->copyAvailable( canCopy );
    }
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

void KWPEWidgetPrivate::onFoundText( WebKitFindController*, guint matchCount, gpointer data )
{
    emit static_cast<KWPEWidgetPrivate*>( data )->q->findResult( true, matchCount );
}

void KWPEWidgetPrivate::onFailedToFindText( WebKitFindController*, gpointer data )
{
    emit static_cast<KWPEWidgetPrivate*>( data )->q->findResult( false, 0 );
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

KWPEWidget::KWPEWidget( QWidget* parent, const char* name )
    : QWidget( parent, name, WRepaintNoErase | WResizeNoErase )
{
    d = new KWPEWidgetPrivate( this );
    // WebKit paints everything; the X server must not clear the window.
    setBackgroundMode( NoBackground );
    setFocusPolicy( StrongFocus );
    setMouseTracking( true );
    setAcceptDrops( false );
    d->create();
}

KWPEWidget::~KWPEWidget()
{
    d->destroy();
    delete d;
}

bool KWPEWidget::isValid() const
{
    return d->view != 0;
}

void KWPEWidget::load( const QString& url )
{
    if ( d->view )
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
    // The selected text into the X clipboard.  (WebKit's own copy goes
    // to WPE's clipboard, which is not the X one yet.)
    if ( d->view )
        webkit_web_view_evaluate_javascript( d->view, "window.getSelection().toString()", -1, 0, 0, d->cancellable,
                                             KWPEWidgetPrivate::onSelectionText, 0 );
}

bool KWPEWidget::hasSelection() const
{
    return d->canCopy;
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
    webkit_web_view_go_to_back_forward_list_item( d->view, it.data() );
    return HistoryNavigating;
}

QString KWPEWidget::bufferPathName()
{
    return KWPE::bufferPath() == KWPE::PathDMABuf ? QString::fromLatin1( "DMA-BUF" ) : QString::fromLatin1( "SHM" );
}

bool KWPEWidget::event( QEvent* e )
{
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
        if ( !modified && !functionKey ) {
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

void KWPEWidget::showEvent( QShowEvent* )
{
    if ( !d->wpeView )
        return;
    d->attach();
    wpe_view_map( d->wpeView );
    wpe_toplevel_state_changed( d->toplevel, isActiveWindow() ? WPE_TOPLEVEL_STATE_ACTIVE : WPE_TOPLEVEL_STATE_NONE );
    // WebKit takes the pointer as inside until it is told otherwise (the
    // overlay scrollbars stay shown and keep repainting).
    if ( !rect().contains( mapFromGlobal( QCursor::pos() ) ) )
        leaveEvent( 0 );
}

void KWPEWidget::hideEvent( QHideEvent* )
{
    if ( d->wpeView )
        wpe_view_unmap( d->wpeView );
}

void KWPEWidget::windowActivationChange( bool )
{
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
    // WebKit's discrete steps (positive: scroll up/left); WebKit animates
    // them (smooth scrolling).  Pixel-precise XI2.1 scrolling: later.
    double steps = e->delta() / 120.0;
    bool horizontal = e->orientation() == Qt::Horizontal;
    WPEEvent* event = wpe_event_scroll_new( d->wpeView, WPE_INPUT_SOURCE_MOUSE, d->eventTime(),
                                            WPEModifiers( wpeModifiers( e->state() ) ),
                                            horizontal ? steps : 0.0, horizontal ? 0.0 : steps,
                                            FALSE, FALSE, e->x(), e->y() );
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
    if ( !s.frames )
        return;
    fprintf( stderr, "kwpe: %s %u fps (%u DMA-BUF), latency %.1f ms, max gap %.1f ms, copy %.2f ms, damage %.2f Mpx\n",
             bufferPathName().latin1(), s.frames, s.dmabufFrames, s.latencyMs, s.maxIntervalMs, s.copyMs,
             s.damagePixels / 1e6 );
}

#include "kwpewidget.moc"
