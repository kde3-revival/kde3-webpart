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
#include <string.h>
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

// Our own deletions (one per piece) and new values of the property seen
// while not waiting for them: nobody else wants them, and left in the queue
// every later scan walks them.
Bool isNoise( Display*, XEvent* e, XPointer arg )
{
    const Match* m = reinterpret_cast<const Match*>( arg );
    return e->type == PropertyNotify && e->xany.window == m->window
           && ( e->xproperty.state == PropertyDelete || m->type != PropertyNotify );
}

struct ServiceMatch
{
    const KWPEXSelection::Service* service;
    Window requestor;
};

Bool isServiced( Display*, XEvent* e, XPointer arg )
{
    const ServiceMatch* m = reinterpret_cast<const ServiceMatch*>( arg );
    return e->xany.window != m->requestor && m->service->wants( e, m->service->data );
}

// Waits for an event of m; false at the deadline (checked every round, so
// a stream of other work cannot hold it off).
bool waitFor( Display* dpy, Match& m, XEvent* event, long long deadline, const KWPEXSelection::Service* service )
{
    XFlush( dpy );
    ServiceMatch serviced = { service, m.window };
    for ( ;; ) {
        if ( nowMs() >= deadline )
            return false;
        // (the deadline is looked at every 64: an owner that floods our
        // window cannot keep us here past it)
        XEvent other;
        bool late = false;
        for ( int i = 1; XCheckIfEvent( dpy, &other, isNoise, reinterpret_cast<XPointer>( &m ) ); ++i )
            if ( i % 64 == 0 && nowMs() >= deadline ) {
                late = true;
                break;
            }
        if ( late )
            return false;
        if ( XCheckIfEvent( dpy, event, matches, reinterpret_cast<XPointer>( &m ) ) )
            return true;
        if ( service && XCheckIfEvent( dpy, &other, isServiced, reinterpret_cast<XPointer>( &serviced ) ) ) {
            service->handle( &other, service->data );
            XFlush( dpy );
            continue;
        }
        long long left = deadline - nowMs();
        if ( left <= 0 )
            return false;
        // Wait for more on the socket.  The checks above end with a flush,
        // during which XCB may read events into its own queue: poll() does
        // not see those (3 s stalls with xclip), so look again when the
        // queue grew, and never wait longer than 10 ms at a time.
        // (Events of other windows stay queued for Qt: no busy loop on them.)
        int queued = XEventsQueued( dpy, QueuedAlready );
        if ( XEventsQueued( dpy, QueuedAfterReading ) > queued )
            continue;
        struct pollfd fd;
        fd.fd = ConnectionNumber( dpy );
        fd.events = POLLIN;
        fd.revents = 0;
        int n = poll( &fd, 1, int( left < 10 ? left : 10 ) );
        if ( n < 0 && errno != EINTR )
            return false;
        if ( n > 0 && ( fd.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) )
            return false;
        if ( n > 0 )
            XEventsQueued( dpy, QueuedAfterReading );
    }
}

// Reads (and deletes) the whole property; appends its items to data.  One
// round trip per piece: INCR owners that send small pieces (xclip) make
// many of them.
bool takeProperty( Display* dpy, Window w, Atom property, std::string& data, Atom* type, int* format,
                   unsigned long* items, size_t maxBytes, bool* tooLarge )
{
    *items = 0;
    *tooLarge = false;
    long offset = 0;   // in 32-bit units
    for ( ;; ) {
        Atom actualType = None;
        int actualFormat = 0;
        unsigned long count = 0, after = 0;
        unsigned char* value = 0;
        // At most what may still come (one long more, to see "more than
        // that" in after without fetching it); delete with the last part
        // (Xlib deletes only when nothing is left).
        size_t room = maxBytes > data.size() ? maxBytes - data.size() : 0;
        long length = long( room / 4 + 1 < 0x400000 ? room / 4 + 1 : 0x400000 );
        if ( XGetWindowProperty( dpy, w, property, offset, length, True, AnyPropertyType, &actualType, &actualFormat,
                                 &count, &after, &value ) != Success )
            return false;
        if ( actualType == None ) {
            if ( value )
                XFree( value );
            return false;
        }
        if ( type )
            *type = actualType;
        if ( format )
            *format = actualFormat;
        size_t bytes = actualFormat == 8 ? count : actualFormat == 16 ? count * sizeof( short ) : actualFormat == 32 ? count * 4 : 0;
        bool fits = data.size() + bytes + after <= maxBytes;
        if ( value ) {
            if ( fits && actualFormat == 32 ) {
                // Xlib hands format 32 items as longs
                const long* longs = reinterpret_cast<const long*>( value );
                for ( unsigned long i = 0; i < count; ++i ) {
                    uint32_t item = uint32_t( longs[i] );
                    data.append( reinterpret_cast<const char*>( &item ), sizeof( item ) );
                }
            } else if ( fits ) {
                data.append( reinterpret_cast<const char*>( value ), bytes );
            }
            XFree( value );
        }
        *items += count;
        if ( !fits ) {
            *tooLarge = true;
            if ( after )
                XDeleteProperty( dpy, w, property );
            return false;
        }
        if ( !after )
            return true;
        offset += long( bytes / 4 );
    }
}

}

KWPEXSelection::Result KWPEXSelection::read( Display* dpy, Atom selection, Atom target, Time time, const Limits& limits,
                                             std::string& data, Atom* type, int* format, const Service* service )
{
    data.clear();
    if ( type )
        *type = None;
    if ( format )
        *format = 0;
    // One read at a time: they share the requestor window and property.
    static bool busy = false;
    if ( busy )
        return Failed;
    struct Guard
    {
        Guard() { busy = true; }
        ~Guard() { busy = false; }
    } guard;
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
    // The idle deadline moves only on progress (an answer, a piece).
    long long deadline = start + limits.idleMs < end ? start + limits.idleMs : end;
    for ( ;; ) {
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
    std::string first;
    if ( !takeProperty( dpy, w, property, first, &actualType, &actualFormat, &items, limits.maxBytes, &tooLarge ) )
        return tooLarge ? TooLarge : Failed;
    if ( actualType != incr ) {
        data.swap( first );
        if ( type )
            *type = actualType;
        if ( format )
            *format = actualFormat;
        return Ok;
    }

    // INCR: deleting the property (done by takeProperty) asks for the first
    // piece; every piece is a new value, an empty one ends the transfer.
    if ( actualFormat == 32 && first.size() >= 4 ) {
        uint32_t announced;
        memcpy( &announced, first.data(), sizeof( announced ) );
        if ( announced > limits.maxBytes ) {
            dropRequestor( dpy );
            return TooLarge;
        }
        data.reserve( announced );
    }
    Match piece = { w, PropertyNotify, property };
    deadline = nowMs() + limits.idleMs < end ? nowMs() + limits.idleMs : end;
    for ( ;; ) {
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
        deadline = nowMs() + limits.idleMs < end ? nowMs() + limits.idleMs : end;
    }
}

namespace
{

struct Outgoing
{
    Window requestor;
    Atom property;
    long long last;
};

const int maxOutgoing = 32;
const long long outgoingMs = 10000;
Outgoing s_outgoing[maxOutgoing];

}

void KWPEXSelection::noteRequest( const XEvent* event )
{
    if ( event->type != SelectionRequest )
        return;
    const XSelectionRequestEvent& r = event->xselectionrequest;
    Window requestor = r.requestor;
    Atom property = r.property != None ? r.property : r.target;   // obsolete clients: the target
    long long now = nowMs();
    int slot = 0;
    for ( int i = 0; i < maxOutgoing; ++i ) {
        if ( s_outgoing[i].requestor == requestor && s_outgoing[i].property == property ) {
            slot = i;
            break;
        }
        if ( s_outgoing[i].last < s_outgoing[slot].last )
            slot = i;   // the oldest (or a free one) otherwise
    }
    s_outgoing[slot].requestor = requestor;
    s_outgoing[slot].property = property;
    s_outgoing[slot].last = now;
}

bool KWPEXSelection::isOutgoingStep( const XEvent* event )
{
    if ( event->type != PropertyNotify || event->xproperty.state != PropertyDelete )
        return false;
    long long now = nowMs();
    for ( int i = 0; i < maxOutgoing; ++i ) {
        Outgoing& o = s_outgoing[i];
        if ( o.requestor == event->xproperty.window && o.property == event->xproperty.atom && now - o.last < outgoingMs ) {
            o.last = now;
            return true;
        }
    }
    return false;
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
