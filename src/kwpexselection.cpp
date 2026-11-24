/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include "kwpexselection.h"

#include <X11/Xatom.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>

namespace
{

long long nowMs()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

// The window that receives the data.  After a read that was given up a
// late answer could still arrive on it, so it is replaced then (the owner
// of an abandoned INCR transfer gets BadWindow and stops).
Display* s_dpy = 0;
Window s_requestor = 0;
Atom s_property = 0;

Window requestor( Display* dpy )
{
    if ( s_dpy != dpy ) {
        s_dpy = dpy;
        s_requestor = 0;
        s_property = XInternAtom( dpy, "KWPE_SELECTION", False );
    }
    if ( !s_requestor ) {
        XSetWindowAttributes attributes;
        attributes.event_mask = PropertyChangeMask;
        s_requestor = XCreateWindow( dpy, DefaultRootWindow( dpy ), -10, -10, 1, 1, 0, 0, InputOnly,
                                     CopyFromParent, CWEventMask, &attributes );
    }
    return s_requestor;
}

void dropRequestor( Display* dpy )
{
    if ( s_requestor && s_dpy == dpy )
        XDestroyWindow( dpy, s_requestor );
    s_requestor = 0;
}

struct Match
{
    Window window;
    int type;
    Atom property;   // PropertyNotify: this property, PropertyNewValue only
};

Bool matches( Display*, XEvent* e, XPointer arg )
{
    const Match* m = reinterpret_cast<const Match*>( arg );
    if ( e->type != m->type || e->xany.window != m->window )
        return False;
    if ( e->type == PropertyNotify )
        return e->xproperty.atom == m->property && e->xproperty.state == PropertyNewValue;
    return True;
}

Bool isSelectionTraffic( Display*, XEvent* e, XPointer arg )
{
    Window own = *reinterpret_cast<Window*>( arg );
    return ( e->type == SelectionRequest || e->type == SelectionClear ) && e->xany.window != own;
}

// Waits for an event of m; false at the deadline.
bool waitFor( Display* dpy, Match& m, XEvent* event, long long deadline, KWPEXSelection::ServiceFunction service )
{
    XFlush( dpy );
    for ( ;; ) {
        if ( XCheckIfEvent( dpy, event, matches, reinterpret_cast<XPointer>( &m ) ) )
            return true;
        if ( service ) {
            XEvent other;
            if ( XCheckIfEvent( dpy, &other, isSelectionTraffic, reinterpret_cast<XPointer>( &m.window ) ) ) {
                service( &other );
                XFlush( dpy );
                continue;
            }
        }
        long long left = deadline - nowMs();
        if ( left <= 0 )
            return false;
        // Everything read is in Xlib's queue now: wait for more on the socket.
        struct pollfd fd;
        fd.fd = ConnectionNumber( dpy );
        fd.events = POLLIN;
        fd.revents = 0;
        int n = poll( &fd, 1, int( left ) );
        if ( n < 0 && errno != EINTR )
            return false;
        if ( n > 0 && ( fd.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) )
            return false;
        if ( n > 0 )
            XEventsQueued( dpy, QueuedAfterReading );
    }
}

// Reads (and deletes) the whole property; appends its items to data.
bool takeProperty( Display* dpy, Window w, Atom property, std::string& data, Atom* type, int* format,
                   unsigned long* items, size_t maxBytes, bool* tooLarge )
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0, after = 0;
    unsigned char* value = 0;
    *items = 0;
    *tooLarge = false;
    // size first
    if ( XGetWindowProperty( dpy, w, property, 0, 0, False, AnyPropertyType, &actualType, &actualFormat,
                             &count, &after, &value ) != Success )
        return false;
    if ( value )
        XFree( value );
    if ( actualType == None )
        return false;
    if ( data.size() + after > maxBytes ) {
        *tooLarge = true;
        XDeleteProperty( dpy, w, property );
        return false;
    }
    long length = long( ( after + 3 ) / 4 );
    value = 0;
    if ( XGetWindowProperty( dpy, w, property, 0, length, True, AnyPropertyType, &actualType, &actualFormat,
                             &count, &after, &value ) != Success )
        return false;
    if ( type )
        *type = actualType;
    if ( format )
        *format = actualFormat;
    if ( value ) {
        if ( actualFormat == 8 ) {
            data.append( reinterpret_cast<const char*>( value ), count );
        } else if ( actualFormat == 16 ) {
            data.append( reinterpret_cast<const char*>( value ), count * sizeof( short ) );
        } else if ( actualFormat == 32 ) {
            // Xlib hands format 32 items as longs
            const long* longs = reinterpret_cast<const long*>( value );
            for ( unsigned long i = 0; i < count; ++i ) {
                uint32_t item = uint32_t( longs[i] );
                data.append( reinterpret_cast<const char*>( &item ), sizeof( item ) );
            }
        }
        XFree( value );
    }
    *items = count;
    return true;
}

}

KWPEXSelection::Result KWPEXSelection::read( Display* dpy, Atom selection, Atom target, Time time, const Limits& limits,
                                             std::string& data, Atom* type, int* format, ServiceFunction service )
{
    data.clear();
    if ( type )
        *type = None;
    if ( format )
        *format = 0;
    if ( XGetSelectionOwner( dpy, selection ) == None )
        return Refused;

    long long start = nowMs();
    long long end = start + limits.totalMs;
    Window w = requestor( dpy );
    Atom property = s_property;
    XDeleteProperty( dpy, w, property );
    XConvertSelection( dpy, selection, target, property, w, time );

    Match notify = { w, SelectionNotify, None };
    XEvent event;
    for ( ;; ) {
        long long deadline = nowMs() + limits.idleMs;
        if ( deadline > end )
            deadline = end;
        if ( !waitFor( dpy, notify, &event, deadline, service ) ) {
            dropRequestor( dpy );
            return Timeout;
        }
        // An answer to an earlier, abandoned request (same window, other
        // selection or target) is not ours.
        if ( event.xselection.selection == selection && event.xselection.target == target )
            break;
    }
    if ( event.xselection.property == None )
        return Refused;

    Atom incr = XInternAtom( dpy, "INCR", False );
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long items = 0;
    bool tooLarge = false;
    // the INCR property is small: read without the limit, then check the announced size
    std::string first;
    if ( !takeProperty( dpy, w, property, first, &actualType, &actualFormat, &items, ~size_t( 0 ), &tooLarge ) )
        return Failed;
    if ( actualType != incr ) {
        if ( first.size() > limits.maxBytes )
            return TooLarge;
        data.swap( first );
        if ( type )
            *type = actualType;
        if ( format )
            *format = actualFormat;
        return Ok;
    }

    // INCR: deleting the property (done by takeProperty) asks for the first
    // piece; every piece is a new value, an empty one ends the transfer.
    if ( first.size() >= 4 ) {
        uint32_t announced = *reinterpret_cast<const uint32_t*>( first.data() );
        if ( announced > limits.maxBytes ) {
            dropRequestor( dpy );
            return TooLarge;
        }
        data.reserve( announced );
    }
    Match piece = { w, PropertyNotify, property };
    for ( ;; ) {
        long long deadline = nowMs() + limits.idleMs;
        if ( deadline > end )
            deadline = end;
        if ( !waitFor( dpy, piece, &event, deadline, service ) ) {
            dropRequestor( dpy );
            data.clear();
            return Timeout;
        }
        size_t before = data.size();
        if ( !takeProperty( dpy, w, property, data, &actualType, &actualFormat, &items, limits.maxBytes, &tooLarge ) ) {
            // a PropertyNotify for a value already taken: nothing there
            if ( !tooLarge )
                continue;
            dropRequestor( dpy );
            data.clear();
            return TooLarge;
        }
        if ( data.size() == before ) {
            if ( type )
                *type = actualType;
            if ( format )
                *format = actualFormat;
            return Ok;
        }
    }
}

const char* KWPEXSelection::resultName( Result result )
{
    switch ( result ) {
    case Ok: return "ok";
    case Refused: return "refused";
    case Timeout: return "timeout";
    case TooLarge: return "too large";
    case Failed: return "failed";
    }
    return "?";
}
