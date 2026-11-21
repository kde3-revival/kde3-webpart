/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// WPE (GLib) first, then Xlib; no Qt header in this file.
#include <wpe/wpe-platform.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/Xrandr.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include "wpex11platform.h"

// ---------------------------------------------------------------------
// helpers

// KWPE_DEBUG=1: what the platform decides; 2: every Present event too
static bool s_debug = false;
static int s_debugLevel = 0;

static void debugLog( const char* format, ... ) G_GNUC_PRINTF( 1, 2 );
static void debugLog( const char* format, ... )
{
    if ( !s_debug )
        return;
    va_list args;
    va_start( args, format );
    fputs( "kwpe: ", stderr );
    vfprintf( stderr, format, args );
    fputc( '\n', stderr );
    va_end( args );
}

static bool envFlag( const char* name )
{
    const char* value = getenv( name );
    return value && *value && strcmp( value, "0" ) != 0;
}

// How long a frame may wait for PresentCompleteNotify before it is taken
// as shown anyway (a window that is not viewable, a server that drops
// the event): WebKit renders nothing until the frame is done.
static const guint presentTimeoutMs = 250;

// The frame clock without Present: about 60 Hz.
static const guint fallbackFrameMs = 16;

// ---------------------------------------------------------------------
// KWPEScreenX11: the X screen (one, the default one)

struct KWPEScreenX11
{
    WPEScreen parent;
};

struct KWPEScreenX11Class
{
    WPEScreenClass parent_class;
};

G_DEFINE_TYPE( KWPEScreenX11, kwpe_screen_x11, WPE_TYPE_SCREEN )

static void kwpe_screen_x11_init( KWPEScreenX11* )
{
}

static void kwpe_screen_x11_class_init( KWPEScreenX11Class* )
{
}

// ---------------------------------------------------------------------
// KWPEDisplayX11

struct KWPEDisplayX11
{
    WPEDisplay parent;

    Display* dpy;
    xcb_connection_t* xcb;
    int screenNumber;
    Window root;
    int depth;
    Visual* visual;

    bool hasShm;
    bool hasPresent;
    int presentOpcode;
    bool hasDri3;
    bool dri3Modifiers;   // DRI3 >= 1.2: PixmapFromBuffers, GetSupportedModifiers
    KWPE::BufferPath path;

    WPEDRMDevice* drmDevice;
    WPEScreen* screen;
};

struct KWPEDisplayX11Class
{
    WPEDisplayClass parent_class;
};

G_DEFINE_TYPE( KWPEDisplayX11, kwpe_display_x11, WPE_TYPE_DISPLAY )

static GType kwpe_view_x11_get_type();
static GType kwpe_toplevel_x11_get_type();

static KWPEDisplayX11* s_display = 0;

// The DRM device of the X server's screen, from DRI3Open.
static WPEDRMDevice* drmDeviceFromDri3( KWPEDisplayX11* d )
{
    xcb_dri3_open_cookie_t cookie = xcb_dri3_open( d->xcb, d->root, 0 );
    xcb_dri3_open_reply_t* reply = xcb_dri3_open_reply( d->xcb, cookie, 0 );
    if ( !reply )
        return 0;

    WPEDRMDevice* device = 0;
    if ( reply->nfd == 1 ) {
        int fd = xcb_dri3_open_reply_fds( d->xcb, reply )[0];
        drmDevicePtr drm = 0;
        if ( drmGetDevice2( fd, 0, &drm ) == 0 ) {
            const char* primary = ( drm->available_nodes & ( 1 << DRM_NODE_PRIMARY ) ) ? drm->nodes[DRM_NODE_PRIMARY] : 0;
            const char* render = ( drm->available_nodes & ( 1 << DRM_NODE_RENDER ) ) ? drm->nodes[DRM_NODE_RENDER] : 0;
            if ( primary || render ) {
                device = wpe_drm_device_new( primary, render );
                debugLog( "DRI3 device: primary %s, render %s", primary ? primary : "-", render ? render : "-" );
            }
            drmFreeDevice( &drm );
        }
        close( fd );
    }
    free( reply );
    return device;
}

static int refreshRateMilliHz( KWPEDisplayX11* d )
{
    int rate = 0;
    XRRScreenResources* res = XRRGetScreenResourcesCurrent( d->dpy, d->root );
    if ( !res )
        return 60000;

    RROutput primary = XRRGetOutputPrimary( d->dpy, d->root );
    for ( int i = 0; i < res->ncrtc && !rate; ++i ) {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo( d->dpy, res, res->crtcs[i] );
        if ( !crtc )
            continue;
        bool isPrimary = !primary;
        for ( int o = 0; o < crtc->noutput; ++o )
            if ( crtc->outputs[o] == primary )
                isPrimary = true;
        if ( crtc->mode && isPrimary ) {
            for ( int m = 0; m < res->nmode; ++m ) {
                const XRRModeInfo& mode = res->modes[m];
                if ( mode.id == crtc->mode && mode.hTotal && mode.vTotal ) {
                    rate = int( double( mode.dotClock ) * 1000.0 / ( double( mode.hTotal ) * mode.vTotal ) + 0.5 );
                    break;
                }
            }
        }
        XRRFreeCrtcInfo( crtc );
    }
    XRRFreeScreenResources( res );
    return rate > 0 ? rate : 60000;
}

static gboolean kwpeDisplayConnect( WPEDisplay* display, GError** )
{
    KWPEDisplayX11* d = reinterpret_cast<KWPEDisplayX11*>( display );

    d->xcb = XGetXCBConnection( d->dpy );
    d->screenNumber = DefaultScreen( d->dpy );
    d->root = RootWindow( d->dpy, d->screenNumber );
    d->depth = DefaultDepth( d->dpy, d->screenNumber );
    d->visual = DefaultVisual( d->dpy, d->screenNumber );

    d->hasShm = XShmQueryExtension( d->dpy );
    // Not across the network: the segment would be useless.
    if ( d->hasShm ) {
        const char* name = DisplayString( d->dpy );
        if ( name && name[0] != ':' && strncmp( name, "unix:", 5 ) != 0 )
            d->hasShm = false;
    }

    int eventBase, errorBase, major = 1, minor = 0;
    d->hasPresent = XPresentQueryExtension( d->dpy, &d->presentOpcode, &eventBase, &errorBase )
        && XPresentQueryVersion( d->dpy, &major, &minor );

    const xcb_query_extension_reply_t* dri3 = xcb_get_extension_data( d->xcb, &xcb_dri3_id );
    if ( dri3 && dri3->present ) {
        xcb_dri3_query_version_reply_t* version =
            xcb_dri3_query_version_reply( d->xcb, xcb_dri3_query_version( d->xcb, 1, 2 ), 0 );
        if ( version ) {
            d->hasDri3 = true;
            d->dri3Modifiers = version->major_version > 1 || version->minor_version >= 2;
            free( version );
        }
    }

    d->path = KWPE::PathSHM;
    if ( d->hasDri3 && d->hasPresent && d->depth == 24 && !envFlag( "KWPE_FORCE_SHM" ) ) {
        d->drmDevice = drmDeviceFromDri3( d );
        if ( d->drmDevice )
            d->path = KWPE::PathDMABuf;
    }

    // One screen: the X screen.  Its refresh rate paces WebKit's frames
    // (it has no vblank source from us yet).
    d->screen = WPE_SCREEN( g_object_new( kwpe_screen_x11_get_type(), "id", 1, NULL ) );
    wpe_screen_set_position( d->screen, 0, 0 );
    wpe_screen_set_size( d->screen, DisplayWidth( d->dpy, d->screenNumber ), DisplayHeight( d->dpy, d->screenNumber ) );
    wpe_screen_set_physical_size( d->screen, DisplayWidthMM( d->dpy, d->screenNumber ), DisplayHeightMM( d->dpy, d->screenNumber ) );
    wpe_screen_set_scale( d->screen, 1.0 );
    wpe_screen_set_refresh_rate( d->screen, refreshRateMilliHz( d ) );

    wpe_display_set_available_input_devices( display,
        WPEAvailableInputDevices( WPE_AVAILABLE_INPUT_DEVICE_MOUSE | WPE_AVAILABLE_INPUT_DEVICE_KEYBOARD ) );

    debugLog( "display: MIT-SHM %d, Present %d.%d (%d), DRI3 %d (modifiers %d), depth %d, refresh %d mHz -> %s",
              d->hasShm, major, minor, d->hasPresent, d->hasDri3, d->dri3Modifiers, d->depth,
              wpe_screen_get_refresh_rate( d->screen ),
              d->path == KWPE::PathDMABuf ? "DMA-BUF" : "SHM" );
    return TRUE;
}

static WPEView* kwpeDisplayCreateView( WPEDisplay* display )
{
    return WPE_VIEW( g_object_new( kwpe_view_x11_get_type(), "display", display, NULL ) );
}

static WPEToplevel* kwpeDisplayCreateToplevel( WPEDisplay* display, guint )
{
    return WPE_TOPLEVEL( g_object_new( kwpe_toplevel_x11_get_type(), "display", display, "max-views", 1u, NULL ) );
}

static WPEDRMDevice* kwpeDisplayGetDrmDevice( WPEDisplay* display )
{
    return reinterpret_cast<KWPEDisplayX11*>( display )->drmDevice;
}

// What the X server can import: 32 bpp BGRA/BGRX (depth 24) with the
// modifiers DRI3 reports for that depth; implicit modifier without DRI3 1.2.
static WPEBufferFormats* kwpeDisplayGetPreferredBufferFormats( WPEDisplay* display )
{
    KWPEDisplayX11* d = reinterpret_cast<KWPEDisplayX11*>( display );
    if ( d->path != KWPE::PathDMABuf )
        return 0;

    std::vector<guint64> modifiers;
    if ( d->dri3Modifiers ) {
        xcb_dri3_get_supported_modifiers_reply_t* reply = xcb_dri3_get_supported_modifiers_reply(
            d->xcb, xcb_dri3_get_supported_modifiers( d->xcb, d->root, 24, 32 ), 0 );
        if ( reply ) {
            const uint64_t* list = xcb_dri3_get_supported_modifiers_window_modifiers( reply );
            int n = xcb_dri3_get_supported_modifiers_window_modifiers_length( reply );
            for ( int i = 0; i < n; ++i )
                modifiers.push_back( list[i] );
            list = xcb_dri3_get_supported_modifiers_screen_modifiers( reply );
            n = xcb_dri3_get_supported_modifiers_screen_modifiers_length( reply );
            for ( int i = 0; i < n; ++i ) {
                bool known = false;
                for ( size_t k = 0; k < modifiers.size(); ++k )
                    known = known || modifiers[k] == list[i];
                if ( !known )
                    modifiers.push_back( list[i] );
            }
            free( reply );
        }
    }
    if ( modifiers.empty() )
        modifiers.push_back( DRM_FORMAT_MOD_INVALID );

    WPEBufferFormatsBuilder* builder = wpe_buffer_formats_builder_new( d->drmDevice );
    wpe_buffer_formats_builder_append_group( builder, 0, WPE_BUFFER_FORMAT_USAGE_RENDERING );
    static const guint32 fourccs[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
    for ( size_t f = 0; f < G_N_ELEMENTS( fourccs ); ++f )
        for ( size_t m = 0; m < modifiers.size(); ++m )
            wpe_buffer_formats_builder_append_format( builder, fourccs[f], modifiers[m] );
    debugLog( "buffer formats: XRGB8888/ARGB8888 with %u modifier(s)", unsigned( modifiers.size() ) );
    return wpe_buffer_formats_builder_end( builder );
}

static guint kwpeDisplayGetNScreens( WPEDisplay* )
{
    return 1;
}

static WPEScreen* kwpeDisplayGetScreen( WPEDisplay* display, guint index )
{
    return index == 0 ? reinterpret_cast<KWPEDisplayX11*>( display )->screen : 0;
}

static void kwpeDisplayDispose( GObject* object )
{
    KWPEDisplayX11* d = reinterpret_cast<KWPEDisplayX11*>( object );
    if ( d->drmDevice ) {
        wpe_drm_device_unref( d->drmDevice );
        d->drmDevice = 0;
    }
    g_clear_object( &d->screen );
    G_OBJECT_CLASS( kwpe_display_x11_parent_class )->dispose( object );
}

static void kwpe_display_x11_init( KWPEDisplayX11* )
{
}

static void kwpe_display_x11_class_init( KWPEDisplayX11Class* klass )
{
    G_OBJECT_CLASS( klass )->dispose = kwpeDisplayDispose;

    WPEDisplayClass* displayClass = WPE_DISPLAY_CLASS( klass );
    displayClass->connect = kwpeDisplayConnect;
    displayClass->create_view = kwpeDisplayCreateView;
    displayClass->create_toplevel = kwpeDisplayCreateToplevel;
    displayClass->get_drm_device = kwpeDisplayGetDrmDevice;
    displayClass->get_preferred_buffer_formats = kwpeDisplayGetPreferredBufferFormats;
    displayClass->get_n_screens = kwpeDisplayGetNScreens;
    displayClass->get_screen = kwpeDisplayGetScreen;
}

// ---------------------------------------------------------------------
// KWPEToplevelX11: the widget's window, as far as WebKit cares (size,
// screen, activity); its size follows the widget, WebKit cannot change it.

struct KWPEToplevelX11
{
    WPEToplevel parent;
};

struct KWPEToplevelX11Class
{
    WPEToplevelClass parent_class;
};

G_DEFINE_TYPE( KWPEToplevelX11, kwpe_toplevel_x11, WPE_TYPE_TOPLEVEL )

static WPEScreen* kwpeToplevelGetScreen( WPEToplevel* toplevel )
{
    return kwpeDisplayGetScreen( wpe_toplevel_get_display( toplevel ), 0 );
}

static gboolean kwpeToplevelResize( WPEToplevel*, int, int )
{
    return FALSE;
}

static void kwpe_toplevel_x11_init( KWPEToplevelX11* )
{
}

static void kwpe_toplevel_x11_class_init( KWPEToplevelX11Class* klass )
{
    WPEToplevelClass* toplevelClass = WPE_TOPLEVEL_CLASS( klass );
    toplevelClass->get_screen = kwpeToplevelGetScreen;
    toplevelClass->resize = kwpeToplevelResize;
}

// ---------------------------------------------------------------------
// KWPEViewX11

namespace
{

// What a WPEBuffer is on the X side (object data of the buffer).
struct BufferX11
{
    Display* dpy;
    Pixmap pixmap;          // the DRI3 import; 0 for SHM buffers
    bool importFailed;
    bool busy;              // presented, no IdleNotify yet
    bool releaseWhenIdle;   // replaced while busy: give it back on IdleNotify
};

const char bufferDataKey[] = "kwpe-x11-buffer";

void freeBufferX11( gpointer data )
{
    BufferX11* bx = static_cast<BufferX11*>( data );
    if ( bx->pixmap )
        XFreePixmap( bx->dpy, bx->pixmap );
    delete bx;
}

BufferX11* bufferX11( WPEBuffer* buffer )
{
    return static_cast<BufferX11*>( g_object_get_data( G_OBJECT( buffer ), bufferDataKey ) );
}

enum FrameSource { SourceNone, SourceBacking, SourceCurrent };

struct ViewState
{
    WPEView* view;
    KWPEDisplayX11* display;
    Window window;
    KWPE::ViewHost* host;
    GC gc;

    guint32 serial;
    bool framePending;          // waiting for PresentCompleteNotify
    guint32 pendingSerial;
    WPEBuffer* pending;         // the buffer of that frame (ref)
    gint64 pendingSince;
    guint frameTimer;           // watchdog, or the frame clock without Present

    WPEBuffer* current;         // DMA-BUF buffer on screen (ref), held
    std::vector<WPEBuffer*> waitingForIdle;   // replaced but still busy (refs)
    FrameSource lastSource;

    // SHM path: our copy of the frame.
    XImage* image;
    XShmSegmentInfo shmInfo;
    bool shmAttached;
    Pixmap backing;
    int backingWidth;
    int backingHeight;

    // statistics
    KWPE::FrameStats stats;
    double latencySum;
    gint64 lastFrameTime;
};

std::vector<ViewState*> s_views;

}

struct KWPEViewX11
{
    WPEView parent;
    ViewState* state;
};

struct KWPEViewX11Class
{
    WPEViewClass parent_class;
};

G_DEFINE_TYPE( KWPEViewX11, kwpe_view_x11, WPE_TYPE_VIEW )

static ViewState* viewState( WPEView* view )
{
    return reinterpret_cast<KWPEViewX11*>( view )->state;
}

static void releaseBuffer( ViewState* st, WPEBuffer* buffer )
{
    BufferX11* bx = bufferX11( buffer );
    if ( bx && bx->busy ) {
        bx->releaseWhenIdle = true;
        st->waitingForIdle.push_back( buffer );   // keeps the reference
        return;
    }
    wpe_view_buffer_released( st->view, buffer );
    g_object_unref( buffer );
}

static void frameDone( ViewState* st, gint64 now )
{
    if ( !st->framePending )
        return;
    st->framePending = false;
    if ( st->frameTimer ) {
        g_source_remove( st->frameTimer );
        st->frameTimer = 0;
    }

    st->stats.frames++;
    st->latencySum += ( now - st->pendingSince ) / 1000.0;
    if ( st->lastFrameTime ) {
        double interval = ( now - st->lastFrameTime ) / 1000.0;
        if ( interval > st->stats.maxIntervalMs )
            st->stats.maxIntervalMs = interval;
    }
    st->lastFrameTime = now;

    WPEBuffer* buffer = st->pending;
    st->pending = 0;
    if ( !buffer )
        return;
    if ( WPE_IS_BUFFER_DMA_BUF( buffer ) ) {
        // Shown: it stays the current one (for Expose) until the next.
        st->stats.dmabufFrames++;
        WPEBuffer* previous = st->current;
        st->current = buffer;
        st->lastSource = SourceCurrent;
        if ( previous && previous != buffer )
            releaseBuffer( st, previous );
        else if ( previous )
            g_object_unref( previous );
        wpe_view_buffer_rendered( st->view, buffer );
    } else {
        // An SHM buffer was given back when it was copied.
        wpe_view_buffer_rendered( st->view, buffer );
        g_object_unref( buffer );
    }
}

static gboolean frameTimeout( gpointer data )
{
    ViewState* st = static_cast<ViewState*>( data );
    st->frameTimer = 0;
    if ( st->framePending && st->display->hasPresent && st->window )
        debugLog( "no PresentCompleteNotify for frame %u after %u ms", st->pendingSerial, presentTimeoutMs );
    frameDone( st, g_get_monotonic_time() );
    return G_SOURCE_REMOVE;
}

static void destroyShmImage( ViewState* st )
{
    Display* dpy = st->display->dpy;
    if ( st->image ) {
        if ( st->shmAttached ) {
            XShmDetach( dpy, &st->shmInfo );
            XDestroyImage( st->image );   // frees the XImage, not the segment
            shmdt( st->shmInfo.shmaddr );
            st->shmAttached = false;
        } else {
            XDestroyImage( st->image );   // frees data (malloc'ed) too
        }
        st->image = 0;
    }
    if ( st->backing ) {
        XFreePixmap( dpy, st->backing );
        st->backing = 0;
    }
    st->backingWidth = st->backingHeight = 0;
}

// XShmAttach() reports failure (BadAccess: another IPC namespace, a
// container) only as an asynchronous X error; this catches it.
static int s_shmMajorOpcode = 0;
static bool s_shmAttachFailed = false;
static int ( *s_previousErrorHandler )( Display*, XErrorEvent* ) = 0;

static int shmAttachErrorHandler( Display* dpy, XErrorEvent* error )
{
    if ( error->request_code == s_shmMajorOpcode && error->minor_code == 1 /* X_ShmAttach */ ) {
        s_shmAttachFailed = true;
        return 0;
    }
    return s_previousErrorHandler ? s_previousErrorHandler( dpy, error ) : 0;
}

static bool shmAttach( Display* dpy, XShmSegmentInfo* info )
{
    int event, error;
    if ( !s_shmMajorOpcode && !XQueryExtension( dpy, "MIT-SHM", &s_shmMajorOpcode, &event, &error ) )
        return false;
    XSync( dpy, False );
    s_shmAttachFailed = false;
    s_previousErrorHandler = XSetErrorHandler( shmAttachErrorHandler );
    bool ok = XShmAttach( dpy, info );
    XSync( dpy, False );
    XSetErrorHandler( s_previousErrorHandler );
    if ( ok && s_shmAttachFailed ) {
        // The server has no attachment, but Xlib's bookkeeping does.
        XShmDetach( dpy, info );
        XSync( dpy, False );
    }
    return ok && !s_shmAttachFailed;
}

// The copy below is a plain memcpy of WebKit's BGRA (little-endian
// ARGB8888) pixels: the X image must have the same layout.
static bool imageMatchesWebKit( const XImage* image )
{
    return image->bits_per_pixel == 32 && image->byte_order == LSBFirst && image->red_mask == 0xff0000
        && image->green_mask == 0xff00 && image->blue_mask == 0xff;
}

static bool ensureShmImage( ViewState* st, int width, int height )
{
    if ( st->image && st->backingWidth == width && st->backingHeight == height )
        return true;

    destroyShmImage( st );
    KWPEDisplayX11* d = st->display;
    Display* dpy = d->dpy;

    if ( d->hasShm ) {
        memset( &st->shmInfo, 0, sizeof( st->shmInfo ) );
        st->image = XShmCreateImage( dpy, d->visual, d->depth, ZPixmap, 0, &st->shmInfo, width, height );
        if ( st->image ) {
            st->shmInfo.shmid = shmget( IPC_PRIVATE, size_t( st->image->bytes_per_line ) * height, IPC_CREAT | 0600 );
            if ( st->shmInfo.shmid >= 0 ) {
                st->shmInfo.shmaddr = st->image->data = static_cast<char*>( shmat( st->shmInfo.shmid, 0, 0 ) );
                st->shmInfo.readOnly = True;
                if ( st->shmInfo.shmaddr != reinterpret_cast<char*>( -1 ) && shmAttach( dpy, &st->shmInfo ) )
                    st->shmAttached = true;
                // Gone as soon as both sides detached.
                shmctl( st->shmInfo.shmid, IPC_RMID, 0 );
            }
            if ( !st->shmAttached ) {
                if ( st->shmInfo.shmaddr && st->shmInfo.shmaddr != reinterpret_cast<char*>( -1 ) )
                    shmdt( st->shmInfo.shmaddr );
                st->image->data = 0;
                XDestroyImage( st->image );
                st->image = 0;
                d->hasShm = false;
                debugLog( "MIT-SHM failed, using XPutImage" );
            }
        }
    }
    if ( !st->image ) {
        st->image = XCreateImage( dpy, d->visual, d->depth, ZPixmap, 0, 0, width, height, 32, 0 );
        if ( !st->image )
            return false;
        st->image->data = static_cast<char*>( malloc( size_t( st->image->bytes_per_line ) * height ) );
    }
    if ( !imageMatchesWebKit( st->image ) ) {
        // Depth 16 or 30, a big-endian server: would need a conversion.
        static bool warned = false;
        if ( !warned )
            fprintf( stderr, "kwpe: the X visual (depth %d, %d bpp, red mask %06lx) is not 32-bit xRGB: cannot show pages\n",
                     d->depth, st->image->bits_per_pixel, st->image->red_mask );
        warned = true;
        destroyShmImage( st );
        return false;
    }

    st->backing = XCreatePixmap( dpy, d->root, width, height, d->depth );
    st->backingWidth = width;
    st->backingHeight = height;
    return true;
}

// The damage as rectangles inside the buffer (all of it if none given).
static std::vector<XRectangle> damageRects( const WPERectangle* rects, guint n, int width, int height )
{
    std::vector<XRectangle> out;
    if ( !n ) {
        XRectangle r = { 0, 0, (unsigned short) width, (unsigned short) height };
        out.push_back( r );
        return out;
    }
    for ( guint i = 0; i < n; ++i ) {
        int x1 = MAX( rects[i].x, 0 ), y1 = MAX( rects[i].y, 0 );
        int x2 = MIN( rects[i].x + rects[i].width, width ), y2 = MIN( rects[i].y + rects[i].height, height );
        if ( x2 <= x1 || y2 <= y1 )
            continue;
        XRectangle r = { (short) x1, (short) y1, (unsigned short) ( x2 - x1 ), (unsigned short) ( y2 - y1 ) };
        out.push_back( r );
    }
    return out;
}

static void presentPixmap( ViewState* st, Pixmap pixmap, const std::vector<XRectangle>& damage )
{
    Display* dpy = st->display->dpy;
    st->serial++;
    XserverRegion region = XFixesCreateRegion( dpy, const_cast<XRectangle*>( damage.data() ), int( damage.size() ) );
    XPresentPixmap( dpy, st->window, pixmap, st->serial, 0 /* valid: all */, region, 0, 0,
                    0 /* any CRTC */, 0, 0, PresentOptionNone, 0, 0, 0, 0, 0 );
    XFixesDestroyRegion( dpy, region );
}

static bool importDmaBuf( ViewState* st, WPEBuffer* buffer, GError** error )
{
    BufferX11* bx = bufferX11( buffer );
    if ( bx )
        return !bx->importFailed;

    KWPEDisplayX11* d = st->display;
    WPEBufferDMABuf* dmabuf = WPE_BUFFER_DMA_BUF( buffer );
    bx = new BufferX11();
    bx->dpy = d->dpy;
    g_object_set_data_full( G_OBJECT( buffer ), bufferDataKey, bx, freeBufferX11 );

    guint32 planes = wpe_buffer_dma_buf_get_n_planes( dmabuf );
    guint64 modifier = wpe_buffer_dma_buf_get_modifier( dmabuf );
    int width = wpe_buffer_get_width( buffer ), height = wpe_buffer_get_height( buffer );
    if ( !d->hasDri3 || planes < 1 || planes > 4 || ( !d->dri3Modifiers && planes != 1 ) ) {
        bx->importFailed = true;
        g_set_error( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "cannot import a DMA-BUF with %u planes", planes );
        return false;
    }

    // DRI3 takes no format: only what the X pixmap (depth 24, 32 bpp)
    // means, the formats offered in the format list.
    guint32 format = wpe_buffer_dma_buf_get_format( dmabuf );
    if ( format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888 ) {
        bx->importFailed = true;
        g_set_error( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "cannot import a DMA-BUF of format %08x", format );
        return false;
    }

    // xcb sends the descriptors and closes them: give it copies.  A failed
    // dup() must not reach xcb: a bad descriptor in SCM_RIGHTS shuts the
    // whole X connection down.
    int32_t fds[4];
    guint32 strides[4] = { 0, 0, 0, 0 }, offsets[4] = { 0, 0, 0, 0 };
    for ( guint32 i = 0; i < planes; ++i ) {
        fds[i] = dup( wpe_buffer_dma_buf_get_fd( dmabuf, i ) );
        if ( fds[i] < 0 ) {
            for ( guint32 k = 0; k < i; ++k )
                close( fds[k] );
            bx->importFailed = true;
            g_set_error( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "cannot duplicate a DMA-BUF descriptor" );
            return false;
        }
        strides[i] = wpe_buffer_dma_buf_get_stride( dmabuf, i );
        offsets[i] = wpe_buffer_dma_buf_get_offset( dmabuf, i );
    }

    xcb_pixmap_t pixmap = xcb_generate_id( d->xcb );
    xcb_void_cookie_t cookie;
    if ( d->dri3Modifiers )
        cookie = xcb_dri3_pixmap_from_buffers_checked( d->xcb, pixmap, d->root, planes, width, height,
                                                       strides[0], offsets[0], strides[1], offsets[1],
                                                       strides[2], offsets[2], strides[3], offsets[3],
                                                       24, 32, modifier, fds );
    else
        cookie = xcb_dri3_pixmap_from_buffer_checked( d->xcb, pixmap, d->root, strides[0] * height,
                                                      width, height, strides[0], 24, 32, fds[0] );
    // Once per buffer (WebKit keeps a handful): a round trip is fine.
    xcb_generic_error_t* xerror = xcb_request_check( d->xcb, cookie );
    if ( xerror ) {
        g_set_error( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED,
                     "DRI3 import of a %dx%d buffer (format %08x, modifier %" G_GINT64_MODIFIER "x) failed: X error %d",
                     width, height, wpe_buffer_dma_buf_get_format( dmabuf ), modifier, xerror->error_code );
        free( xerror );
        bx->importFailed = true;
        return false;
    }
    bx->pixmap = pixmap;
    debugLog( "imported DMA-BUF %dx%d, format %08x, modifier %" G_GINT64_MODIFIER "x, %u plane(s) as pixmap 0x%lx",
              width, height, wpe_buffer_dma_buf_get_format( dmabuf ), modifier, planes, (unsigned long) pixmap );
    return true;
}

static void scheduleFrameTimer( ViewState* st )
{
    if ( st->frameTimer )
        g_source_remove( st->frameTimer );
    bool presenting = st->display->hasPresent && st->window;
    st->frameTimer = g_timeout_add( presenting ? presentTimeoutMs : fallbackFrameMs, frameTimeout, st );
}

static gboolean kwpeViewRenderBuffer( WPEView* view, WPEBuffer* buffer, const WPERectangle* rects,
                                      guint nRects, GError** error )
{
    ViewState* st = viewState( view );
    KWPEDisplayX11* d = st->display;
    Display* dpy = d->dpy;
    int width = wpe_buffer_get_width( buffer ), height = wpe_buffer_get_height( buffer );
    if ( !st->stats.frames && !st->framePending )
        debugLog( "render_buffer: %s %dx%d, %u damage rect(s)", G_OBJECT_TYPE_NAME( buffer ), width, height, nRects );

    if ( st->framePending )   // should not happen: WebKit waits for frames
        frameDone( st, g_get_monotonic_time() );

    std::vector<XRectangle> damage = damageRects( rects, nRects, width, height );
    if ( s_debugLevel > 2 )
        for ( size_t i = 0; i < damage.size(); ++i )
            debugLog( "damage %d,%d %dx%d", damage[i].x, damage[i].y, damage[i].width, damage[i].height );
    for ( size_t i = 0; i < damage.size(); ++i )
        st->stats.damagePixels += unsigned( damage[i].width ) * damage[i].height;
    st->pendingSince = g_get_monotonic_time();

    if ( WPE_IS_BUFFER_DMA_BUF( buffer ) ) {
        if ( !importDmaBuf( st, buffer, error ) )
            return FALSE;
        BufferX11* bx = bufferX11( buffer );
        st->pending = WPE_BUFFER( g_object_ref( buffer ) );
        if ( st->window && d->hasPresent ) {
            bx->busy = true;
            presentPixmap( st, bx->pixmap, damage );
        } else if ( st->window ) {
            for ( size_t i = 0; i < damage.size(); ++i )
                XCopyArea( dpy, bx->pixmap, st->window, st->gc, damage[i].x, damage[i].y,
                           damage[i].width, damage[i].height, damage[i].x, damage[i].y );
        }
    } else if ( WPE_IS_BUFFER_SHM( buffer ) ) {
        WPEBufferSHM* shm = WPE_BUFFER_SHM( buffer );
        gsize size = 0;
        const guchar* pixels = static_cast<const guchar*>( g_bytes_get_data( wpe_buffer_shm_get_data( shm ), &size ) );
        guint stride = wpe_buffer_shm_get_stride( shm );
        // The buffer comes from the web process: a row shorter than the
        // width would make the copy below read past the pixels.
        if ( !pixels || width <= 0 || height <= 0 || stride < guint( width ) * 4 || size < gsize( stride ) * height
             || !ensureShmImage( st, width, height ) ) {
            g_set_error_literal( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "cannot show the SHM buffer" );
            return FALSE;
        }

        // ARGB8888 premultiplied in memory is what a 32 bpp ZPixmap of a
        // depth 24 TrueColor visual is (little endian): copy rows as they are.
        gint64 copyStart = g_get_monotonic_time();
        XImage* image = st->image;
        for ( size_t i = 0; i < damage.size(); ++i ) {
            const XRectangle& r = damage[i];
            for ( int y = r.y; y < r.y + r.height; ++y )
                memcpy( image->data + size_t( y ) * image->bytes_per_line + r.x * 4,
                        pixels + size_t( y ) * stride + r.x * 4, size_t( r.width ) * 4 );
        }
        st->stats.copyMs += ( g_get_monotonic_time() - copyStart ) / 1000.0;
        // Our copy is complete: WebKit may draw into the buffer again.
        wpe_view_buffer_released( view, buffer );
        st->pending = WPE_BUFFER( g_object_ref( buffer ) );

        for ( size_t i = 0; i < damage.size(); ++i ) {
            const XRectangle& r = damage[i];
            if ( st->shmAttached )
                XShmPutImage( dpy, st->backing, st->gc, image, r.x, r.y, r.x, r.y, r.width, r.height, False );
            else
                XPutImage( dpy, st->backing, st->gc, image, r.x, r.y, r.x, r.y, r.width, r.height );
        }
        st->lastSource = SourceBacking;
        if ( st->current ) {   // switched from DMA-BUF to SHM
            releaseBuffer( st, st->current );
            st->current = 0;
        }
        if ( st->window && d->hasPresent ) {
            presentPixmap( st, st->backing, damage );
        } else if ( st->window ) {
            for ( size_t i = 0; i < damage.size(); ++i )
                XCopyArea( dpy, st->backing, st->window, st->gc, damage[i].x, damage[i].y,
                           damage[i].width, damage[i].height, damage[i].x, damage[i].y );
        }
    } else {
        g_set_error_literal( error, WPE_VIEW_ERROR, WPE_VIEW_ERROR_RENDER_FAILED, "unsupported buffer type" );
        return FALSE;
    }

    st->framePending = true;
    st->pendingSerial = st->serial;
    scheduleFrameTimer( st );
    XFlush( dpy );
    return TRUE;
}

static void kwpeViewSetCursorFromName( WPEView* view, const char* name )
{
    ViewState* st = viewState( view );
    if ( st->host )
        st->host->setCursorName( name );
}

static gboolean kwpeViewCanBeMapped( WPEView* )
{
    return TRUE;
}

static void kwpeViewConstructed( GObject* object )
{
    G_OBJECT_CLASS( kwpe_view_x11_parent_class )->constructed( object );

    KWPEViewX11* self = reinterpret_cast<KWPEViewX11*>( object );
    ViewState* st = new ViewState();
    st->view = WPE_VIEW( object );
    st->display = reinterpret_cast<KWPEDisplayX11*>( wpe_view_get_display( st->view ) );
    st->gc = XCreateGC( st->display->dpy, st->display->root, 0, 0 );
    XSetGraphicsExposures( st->display->dpy, st->gc, False );
    self->state = st;
    s_views.push_back( st );
}

static void kwpeViewDispose( GObject* object )
{
    KWPEViewX11* self = reinterpret_cast<KWPEViewX11*>( object );
    ViewState* st = self->state;
    if ( st ) {
        for ( size_t i = 0; i < s_views.size(); ++i )
            if ( s_views[i] == st ) {
                s_views.erase( s_views.begin() + i );
                break;
            }
        if ( st->frameTimer )
            g_source_remove( st->frameTimer );
        if ( st->pending )
            g_object_unref( st->pending );
        if ( st->current )
            g_object_unref( st->current );
        for ( size_t i = 0; i < st->waitingForIdle.size(); ++i )
            g_object_unref( st->waitingForIdle[i] );
        destroyShmImage( st );
        XFreeGC( st->display->dpy, st->gc );
        delete st;
        self->state = 0;
    }
    G_OBJECT_CLASS( kwpe_view_x11_parent_class )->dispose( object );
}

static void kwpe_view_x11_init( KWPEViewX11* )
{
}

static void kwpe_view_x11_class_init( KWPEViewX11Class* klass )
{
    GObjectClass* objectClass = G_OBJECT_CLASS( klass );
    objectClass->constructed = kwpeViewConstructed;
    objectClass->dispose = kwpeViewDispose;

    WPEViewClass* viewClass = WPE_VIEW_CLASS( klass );
    viewClass->render_buffer = kwpeViewRenderBuffer;
    viewClass->set_cursor_from_name = kwpeViewSetCursorFromName;
    viewClass->can_be_mapped = kwpeViewCanBeMapped;
}

// ---------------------------------------------------------------------
// public interface

namespace KWPE
{

WPEDisplay* x11Display( Display* dpy )
{
    if ( s_display )
        return WPE_DISPLAY( s_display );

    s_debug = envFlag( "KWPE_DEBUG" );
    s_debugLevel = s_debug ? atoi( getenv( "KWPE_DEBUG" ) ) : 0;
    s_display = reinterpret_cast<KWPEDisplayX11*>( g_object_new( kwpe_display_x11_get_type(), NULL ) );
    s_display->dpy = dpy;
    GError* error = 0;
    if ( !wpe_display_connect( WPE_DISPLAY( s_display ), &error ) ) {
        fprintf( stderr, "kwpe: cannot connect the WPE display: %s\n", error ? error->message : "?" );
        g_clear_error( &error );
        g_clear_object( &s_display );
        return 0;
    }
    // WebKit takes the DRM device and the screens from the primary display.
    wpe_display_set_primary( WPE_DISPLAY( s_display ) );
    return WPE_DISPLAY( s_display );
}

BufferPath bufferPath()
{
    return s_display ? s_display->path : PathSHM;
}

void attachView( WPEView* view, unsigned long window, ViewHost* host )
{
    ViewState* st = viewState( view );
    if ( !st )
        return;
    st->host = host;
    if ( st->window == window )
        return;

    // A frame in flight belongs to the old window: done with it.
    if ( st->framePending )
        frameDone( st, g_get_monotonic_time() );
    // IdleNotify of the old window never comes (the event selection died
    // with it): its buffers are free, or WebKit runs out of them.
    if ( st->current ) {
        BufferX11* bx = bufferX11( st->current );
        if ( bx )
            bx->busy = false;
    }
    for ( size_t k = 0; k < st->waitingForIdle.size(); ++k ) {
        BufferX11* bx = bufferX11( st->waitingForIdle[k] );
        if ( bx ) {
            bx->busy = false;
            bx->releaseWhenIdle = false;
        }
        wpe_view_buffer_released( st->view, st->waitingForIdle[k] );
        g_object_unref( st->waitingForIdle[k] );
    }
    st->waitingForIdle.clear();

    st->window = window;
    // The selection goes away with the window, no need to free it.
    if ( window && st->display->hasPresent )
        XPresentSelectInput( st->display->dpy, window, PresentCompleteNotifyMask | PresentIdleNotifyMask );
    debugLog( "view %p shown in window 0x%lx", (void*) view, window );
}

void exposeView( WPEView* view, int x, int y, int w, int h )
{
    ViewState* st = viewState( view );
    if ( !st || !st->window || w <= 0 || h <= 0 )
        return;
    Display* dpy = st->display->dpy;

    Drawable source = 0;
    int sw = 0, sh = 0;
    if ( st->lastSource == SourceCurrent && st->current ) {
        BufferX11* bx = bufferX11( st->current );
        if ( bx && bx->pixmap ) {
            source = bx->pixmap;
            sw = wpe_buffer_get_width( st->current );
            sh = wpe_buffer_get_height( st->current );
        }
    } else if ( st->lastSource == SourceBacking && st->backing ) {
        source = st->backing;
        sw = st->backingWidth;
        sh = st->backingHeight;
    }

    // Outside the last frame (none yet, or the widget grew): white,
    // like an empty page, until WebKit sends a frame of the new size.
    XSetForeground( dpy, st->gc, WhitePixel( dpy, st->display->screenNumber ) );
    if ( !source ) {
        XFillRectangle( dpy, st->window, st->gc, x, y, w, h );
    } else {
        int cw = MIN( x + w, sw ) - x, ch = MIN( y + h, sh ) - y;
        if ( cw > 0 && ch > 0 )
            XCopyArea( dpy, source, st->window, st->gc, x, y, cw, ch, x, y );
        if ( x + w > sw )
            XFillRectangle( dpy, st->window, st->gc, MAX( x, sw ), y, x + w - MAX( x, sw ), h );
        if ( y + h > sh )
            XFillRectangle( dpy, st->window, st->gc, x, MAX( y, sh ), w, y + h - MAX( y, sh ) );
    }
    XFlush( dpy );
}

// The last key event from the X server, for sendKeyEvent().
static XKeyEvent s_lastKey;
static bool s_lastKeyValid = false;

static guint wpeModifiers( unsigned int state )
{
    guint modifiers = 0;
    if ( state & ShiftMask )
        modifiers |= WPE_MODIFIER_KEYBOARD_SHIFT;
    if ( state & LockMask )
        modifiers |= WPE_MODIFIER_KEYBOARD_CAPS_LOCK;
    if ( state & ControlMask )
        modifiers |= WPE_MODIFIER_KEYBOARD_CONTROL;
    if ( state & Mod1Mask )
        modifiers |= WPE_MODIFIER_KEYBOARD_ALT;
    if ( state & Mod4Mask )
        modifiers |= WPE_MODIFIER_KEYBOARD_META;
    if ( state & Button1Mask )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON1;
    if ( state & Button2Mask )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON2;
    if ( state & Button3Mask )
        modifiers |= WPE_MODIFIER_POINTER_BUTTON3;
    return modifiers;
}

bool sendKeyEvent( WPEView* view, bool press )
{
    if ( !s_display || !s_lastKeyValid || s_lastKey.type != ( press ? KeyPress : KeyRelease ) )
        return false;
    s_lastKeyValid = false;

    // The keysym of the event's group and shift level (XKB state in the
    // event's state field), like XLookupString but without the text.
    KeySym keysym = NoSymbol;
    unsigned int consumed = 0;
    if ( !XkbLookupKeySym( s_display->dpy, s_lastKey.keycode, s_lastKey.state, &consumed, &keysym ) || keysym == NoSymbol )
        return false;

    // The X state is the one before the event; WebKit wants the one of
    // the event for modifier keys (as WPE's Wayland platform does it).
    guint modifiers = wpeModifiers( s_lastKey.state );
    guint bit = 0;
    switch ( keysym ) {
    case XK_Shift_L: case XK_Shift_R: bit = WPE_MODIFIER_KEYBOARD_SHIFT; break;
    case XK_Control_L: case XK_Control_R: bit = WPE_MODIFIER_KEYBOARD_CONTROL; break;
    case XK_Alt_L: case XK_Alt_R: bit = WPE_MODIFIER_KEYBOARD_ALT; break;
    case XK_Meta_L: case XK_Meta_R: case XK_Super_L: case XK_Super_R: bit = WPE_MODIFIER_KEYBOARD_META; break;
    case XK_Caps_Lock: bit = WPE_MODIFIER_KEYBOARD_CAPS_LOCK; break;
    default: break;
    }
    if ( bit && press )
        modifiers |= bit;
    else if ( bit )
        modifiers &= ~bit;

    // WPE's keysyms are X keysyms, its keycodes evdev + 8 like X's.
    WPEEvent* event = wpe_event_keyboard_new( press ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                                              view, WPE_INPUT_SOURCE_KEYBOARD, guint32( s_lastKey.time ),
                                              WPEModifiers( modifiers ), s_lastKey.keycode, guint( keysym ) );
    wpe_view_event( view, event );
    wpe_event_unref( event );
    return true;
}

bool handleXEvent( XEvent* event )
{
    if ( event->type == KeyPress || event->type == KeyRelease ) {
        s_lastKey = event->xkey;
        s_lastKeyValid = true;
        return false;
    }

    if ( !s_display || !s_display->hasPresent || event->type != GenericEvent
         || event->xgeneric.extension != s_display->presentOpcode )
        return false;

    Display* dpy = s_display->dpy;
    XGenericEventCookie* cookie = &event->xcookie;
    if ( !XGetEventData( dpy, cookie ) )
        return true;

    gint64 now = g_get_monotonic_time();
    if ( cookie->evtype == PresentCompleteNotify ) {
        XPresentCompleteNotifyEvent* ev = static_cast<XPresentCompleteNotifyEvent*>( cookie->data );
        if ( s_debugLevel > 1 )
            debugLog( "complete serial %u kind %d mode %d window 0x%lx msc %llu", ev->serial_number, ev->kind, ev->mode, ev->window, (unsigned long long) ev->msc );
        for ( size_t i = 0; i < s_views.size(); ++i ) {
            ViewState* st = s_views[i];
            if ( st->window == ev->window && ev->kind == PresentCompleteKindPixmap
                 && st->framePending && ev->serial_number == st->pendingSerial ) {
                frameDone( st, now );
                break;
            }
        }
    } else if ( cookie->evtype == PresentIdleNotify ) {
        XPresentIdleNotifyEvent* ev = static_cast<XPresentIdleNotifyEvent*>( cookie->data );
        for ( size_t i = 0; i < s_views.size(); ++i ) {
            ViewState* st = s_views[i];
            if ( st->window != ev->window )
                continue;
            if ( st->current ) {
                BufferX11* bx = bufferX11( st->current );
                if ( bx && bx->pixmap == ev->pixmap )
                    bx->busy = false;
            }
            if ( st->pending ) {
                BufferX11* bx = bufferX11( st->pending );
                if ( bx && bx->pixmap == ev->pixmap )
                    bx->busy = false;
            }
            for ( size_t k = 0; k < st->waitingForIdle.size(); ++k ) {
                WPEBuffer* buffer = st->waitingForIdle[k];
                BufferX11* bx = bufferX11( buffer );
                if ( bx && bx->pixmap == ev->pixmap ) {
                    bx->busy = false;
                    bx->releaseWhenIdle = false;
                    st->waitingForIdle.erase( st->waitingForIdle.begin() + k );
                    wpe_view_buffer_released( st->view, buffer );
                    g_object_unref( buffer );
                    break;
                }
            }
        }
    }
    XFreeEventData( dpy, cookie );
    return true;
}

FrameStats takeFrameStats( WPEView* view )
{
    FrameStats result = FrameStats();
    ViewState* st = viewState( view );
    if ( !st )
        return result;
    result = st->stats;
    if ( result.frames )
        result.latencyMs = st->latencySum / result.frames;
    st->stats = FrameStats();
    st->latencySum = 0;
    return result;
}

}
