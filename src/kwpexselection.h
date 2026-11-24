/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEXSELECTION_H
#define KWPEXSELECTION_H

#include <X11/Xlib.h>

#include <stddef.h>
#include <string>

/**
 * Reading another program's X selection (ICCCM), with the INCR protocol for
 * large data (a screenshot PNG is several MB, the owner sends it in pieces
 * of about 256 KB), bounded in time.
 *
 * WebKit asks for clipboard data synchronously, so the caller waits; this
 * waits on the X connection itself (no polling with sleeps, as Qt 3's
 * reader does: 50 ms per piece) and gives up after a time without
 * progress and after a total time, instead of 5 s per step.  Plain Xlib,
 * no Qt (tests/clipboard runs it alone).
 */
namespace KWPEXSelection
{

enum Result
{
    Ok,
    Refused,      // the owner cannot convert to that target, or no owner
    Timeout,      // no answer / no progress in time
    TooLarge,     // more than maxBytes
    Failed        // a protocol error (the property vanished, ...)
};

/** Events that must not wait while we do (requests to our own selections). */
typedef void ( *ServiceFunction )( XEvent* event );

struct Limits
{
    int idleMs;        // longest wait for the next answer or piece
    int totalMs;       // longest read in all
    size_t maxBytes;
};

/**
 * Converts @p selection to @p target and reads it into @p data (format 8
 * data as bytes; format 32 items as 32-bit values in host order, e.g.
 * TARGETS).  @p time is the time of the user's request (ICCCM).
 * @p service, if given, gets the SelectionRequest/SelectionClear events of
 * other windows of this connection that arrive meanwhile.
 */
Result read( Display* dpy, Atom selection, Atom target, Time time, const Limits& limits,
             std::string& data, Atom* type = 0, int* format = 0, ServiceFunction service = 0 );

/** Text for logs: "ok", "refused", ... */
const char* resultName( Result result );

}

#endif
