/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPE_X11PLATFORM_H
#define KWPE_X11PLATFORM_H

/*
 * A WPEPlatform implementation for an X11 window owned by somebody else
 * (a Qt 3 widget): WebKit renders into buffers and hands them to the
 * WPEView of this platform, which shows them in the window.
 *
 *  - DMA-BUF (GPU memory) when the X server has DRI3 and Present: every
 *    buffer becomes an X pixmap once (DRI3 PixmapFromBuffers) and each
 *    frame is a PresentPixmap of it, limited to the damaged region; the
 *    copy happens on the GPU at the next vblank, no pixel crosses the CPU.
 *  - Shared memory otherwise (Xephyr, remote X, KWPE_FORCE_SHM=1): the
 *    damaged rectangles are copied into a MIT-SHM image, put into a
 *    backing pixmap and presented from there.
 *
 * Present's CompleteNotify tells WebKit that the frame is on screen
 * (wpe_view_buffer_rendered), its IdleNotify that a buffer may be reused
 * (wpe_view_buffer_released), like a Wayland compositor would.
 *
 * The interface is deliberately free of GLib, WPE, Xlib and Qt headers:
 * Qt 3 defines "signals", "slots" and "emit" as macros (GLib uses those
 * words), Xlib defines "None", "Bool", "Status" and friends (Qt uses
 * those words).  The implementation includes WPE and Xlib only, the Qt
 * side (kwpewidget.cpp) includes WebKit before Qt and never Xlib's
 * extension headers.
 */

typedef struct _XDisplay Display;
typedef union _XEvent XEvent;
typedef struct _WPEDisplay WPEDisplay;
typedef struct _WPEView WPEView;

namespace KWPE
{

/** What the view needs from the widget it is shown in. */
class ViewHost
{
public:
    /** A CSS cursor name ("default", "pointer", "text", ...). */
    virtual void setCursorName( const char* name ) = 0;

protected:
    ~ViewHost() {}
};

enum BufferPath
{
    PathSHM,     /// pixels copied through MIT-SHM (or plain XPutImage)
    PathDMABuf   /// GPU buffers imported with DRI3, presented with Present
};

/** Counters since the previous call of takeFrameStats(). */
struct FrameStats
{
    unsigned frames;        /// frames shown
    unsigned dmabufFrames;  /// ... of them from DMA-BUF buffers
    double copyMs;          /// CPU time spent copying SHM pixels (total)
    double latencyMs;       /// average render_buffer -> PresentCompleteNotify
    double maxIntervalMs;   /// longest gap between two shown frames
    unsigned damagePixels;  /// damaged pixels (total)
};

/**
 * The process-wide WPEDisplay on the X connection @p dpy (Qt's), created
 * and connected on first use.  Returns 0 if WPE refused it.
 */
WPEDisplay* x11Display( Display* dpy );

/** The buffer path the display offers to WebKit (decided once). */
BufferPath bufferPath();

/**
 * Show @p view (created by WebKit on the display above) in the X window
 * @p window, an InputOutput window of the default visual.  Called again
 * when the widget got a new window (Qt 3 recreates it on reparent).
 * @p host gets the cursor changes.  window = 0 detaches.
 */
void attachView( WPEView* view, unsigned long window, ViewHost* host );

/** Repaint a part of the window from the last frame (an Expose). */
void exposeView( WPEView* view, int x, int y, int w, int h );

/**
 * X events for the platform (Present notifications); true if consumed.
 * The widget code calls this from its Qt X11 event filter, for every
 * event: key events are remembered (not consumed) for sendKeyEvent().
 */
bool handleXEvent( XEvent* ev );

/**
 * Send the X key event Qt is delivering right now (KeyPress if @p press,
 * else KeyRelease) to @p view, with its X keycode, the keysym of the
 * current keyboard group and level as keyval (so a Romanian layout gives
 * WebKit "scommabelow", not "semicolon") and the modifiers.  Returns
 * false if there is no such X event (a key event synthesized by Qt or an
 * input method): the caller translates the Qt event then.
 */
bool sendKeyEvent( WPEView* view, bool press );

FrameStats takeFrameStats( WPEView* view );

}

#endif
