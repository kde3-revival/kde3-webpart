/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// xseltest DISPLAY [read TARGET]: KWPEXSelection against owners that behave and owners
// that do not, on a nested X server (never :0/:1).  An owner runs in a
// child process per case; it serves "image/png" (or stalls, refuses, ...).
//
//   ok small       one property
//   ok incr        8 MB in INCR pieces (the owner's piece size: the
//                  server's max request size, as Qt 3 and xclip do)
//   silent         no answer at all: Timeout after the idle time
//   incr stall     INCR, then no more pieces: Timeout, the next read works
//   refused        an unknown target: Refused at once
//   too large      one property / INCR announcing more than the limit
//   slow pieces    INCR pieces 100 ms apart: Ok (progress keeps it going)
//   total          INCR pieces 300 ms apart, total limit 1 s: Timeout
//   flood          property events on our window without end, no answer
//   endless        pieces every 5 ms that never end: the total limit
//   own INCR       while we read PRIMARY (slowly), another client reads our
//                  CLIPBOARD by INCR: served through the Service hook

#include "kwpexselection.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

namespace
{

enum Mode { Serve, Silent, IncrStall, Huge, SlowPieces, Endless, Flood };

long long nowMs()
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

std::string payload( size_t size )
{
    std::string s( size, '\0' );
    uint32_t x = 2463534242u;
    for ( size_t i = 0; i < size; ++i ) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = char( x );
    }
    return s;
}

// The owner: owns CLIPBOARD, writes "1" to the pipe, serves until killed.
const char* s_selection = "CLIPBOARD";   // what the owner children own

void owner( const char* name, Mode mode, size_t size, int pieceDelayMs, int ready )
{
    Display* dpy = XOpenDisplay( name );
    if ( !dpy )
        _exit( 2 );
    Window w = XCreateSimpleWindow( dpy, DefaultRootWindow( dpy ), 0, 0, 1, 1, 0, 0, 0 );
    Atom clipboard = XInternAtom( dpy, s_selection, False );
    Atom png = XInternAtom( dpy, "image/png", False );
    Atom targets = XInternAtom( dpy, "TARGETS", False );
    Atom incr = XInternAtom( dpy, "INCR", False );
    XSetSelectionOwner( dpy, clipboard, w, CurrentTime );
    XSync( dpy, False );
    if ( write( ready, "1", 1 ) != 1 )
        _exit( 2 );
    std::string data = payload( size );
    size_t piece = XMaxRequestSize( dpy ) * 4 - 100;

    // one INCR transfer at a time is enough here
    Window requestor = 0;
    Atom property = None;
    size_t offset = 0;
    bool done = true;
    for ( ;; ) {
        XEvent e;
        XNextEvent( dpy, &e );
        if ( e.type == SelectionRequest ) {
            XSelectionRequestEvent& r = e.xselectionrequest;
            XEvent reply;
            memset( &reply, 0, sizeof( reply ) );
            reply.xselection.type = SelectionNotify;
            reply.xselection.requestor = r.requestor;
            reply.xselection.selection = r.selection;
            reply.xselection.target = r.target;
            reply.xselection.time = r.time;
            reply.xselection.property = None;
            if ( mode == Silent )
                continue;
            if ( mode == Flood && r.target == png ) {
                // changes and deletions of a property on the requestor's
                // window, without end and without an answer
                Atom junk = XInternAtom( dpy, "XSELTEST_JUNK", False );
                long long until = nowMs() + 3000;
                while ( nowMs() < until ) {
                    for ( int k = 0; k < 200; ++k ) {
                        XChangeProperty( dpy, r.requestor, junk, XA_INTEGER, 8, PropModeReplace,
                                         reinterpret_cast<const unsigned char*>( "x" ), 1 );
                        XDeleteProperty( dpy, r.requestor, junk );
                    }
                    XFlush( dpy );
                }
                continue;
            }
            if ( r.target == targets ) {
                long list[2] = { long( targets ), long( png ) };
                XChangeProperty( dpy, r.requestor, r.property, XA_ATOM, 32, PropModeReplace,
                                 reinterpret_cast<unsigned char*>( list ), 2 );
                reply.xselection.property = r.property;
            } else if ( r.target == png ) {
                if ( mode == Serve && data.size() <= piece ) {
                    XChangeProperty( dpy, r.requestor, r.property, png, 8, PropModeReplace,
                                     reinterpret_cast<const unsigned char*>( data.data() ), int( data.size() ) );
                } else {
                    long announced = mode == Huge ? 100L * 1024 * 1024 : long( data.size() );
                    XSelectInput( dpy, r.requestor, PropertyChangeMask );
                    XChangeProperty( dpy, r.requestor, r.property, incr, 32, PropModeReplace,
                                     reinterpret_cast<unsigned char*>( &announced ), 1 );
                    requestor = r.requestor;
                    property = r.property;
                    offset = 0;
                    done = false;
                }
                reply.xselection.property = r.property;
            }
            XSendEvent( dpy, r.requestor, False, 0, &reply );
            XFlush( dpy );
        } else if ( e.type == PropertyNotify && e.xproperty.window == requestor && e.xproperty.atom == property
                    && e.xproperty.state == PropertyDelete && !done ) {
            if ( mode == IncrStall && offset >= 3 * piece )
                continue;   // stop sending
            if ( pieceDelayMs )
                usleep( pieceDelayMs * 1000 );
            if ( mode == Endless && offset >= data.size() )
                offset = 0;   // never ends
            size_t n = offset < data.size() ? std::min( piece, data.size() - offset ) : 0;
            XChangeProperty( dpy, requestor, property, png, 8, PropModeReplace,
                             reinterpret_cast<const unsigned char*>( data.data() + offset ), int( n ) );
            XFlush( dpy );
            offset += n;
            if ( !n )
                done = true;
        }
    }
}


// ---- our own CLIPBOARD served (INCR) while we read PRIMARY ----------------
// The reader's Service hands us the requests and the requestor's
// PropertyDelete events; we answer like Qt 3 does.
struct OwnServer
{
    Display* dpy;
    Window window;
    Atom clipboard, png, targets, incr;
    std::string data;
    size_t piece;
    Window requestor;
    Atom property;
    size_t offset;
    bool active;
    int handled;
};

bool ownWants( const XEvent* e, void* data )
{
    const OwnServer* o = static_cast<const OwnServer*>( data );
    if ( e->type == SelectionRequest )
        return e->xselectionrequest.owner == o->window;
    return KWPEXSelection::isOutgoingStep( e );
}

void ownHandle( XEvent* e, void* data )
{
    OwnServer* o = static_cast<OwnServer*>( data );
    o->handled++;
    if ( e->type == SelectionRequest ) {
        KWPEXSelection::noteRequest( e );
        XSelectionRequestEvent& r = e->xselectionrequest;
        XEvent reply;
        memset( &reply, 0, sizeof( reply ) );
        reply.xselection.type = SelectionNotify;
        reply.xselection.requestor = r.requestor;
        reply.xselection.selection = r.selection;
        reply.xselection.target = r.target;
        reply.xselection.time = r.time;
        reply.xselection.property = None;
        if ( r.target == o->png ) {
            long announced = long( o->data.size() );
            XSelectInput( o->dpy, r.requestor, PropertyChangeMask );
            XChangeProperty( o->dpy, r.requestor, r.property, o->incr, 32, PropModeReplace,
                             reinterpret_cast<unsigned char*>( &announced ), 1 );
            o->requestor = r.requestor;
            o->property = r.property;
            o->offset = 0;
            o->active = true;
            reply.xselection.property = r.property;
        }
        XSendEvent( o->dpy, r.requestor, False, 0, &reply );
    } else if ( o->active && e->xproperty.window == o->requestor && e->xproperty.atom == o->property ) {
        size_t n = o->offset < o->data.size() ? std::min( o->piece, o->data.size() - o->offset ) : 0;
        XChangeProperty( o->dpy, o->requestor, o->property, o->png, 8, PropModeReplace,
                         reinterpret_cast<const unsigned char*>( o->data.data() + o->offset ), int( n ) );
        o->offset += n;
        if ( !n )
            o->active = false;
    }
}

// A client that reads our CLIPBOARD (INCR) and writes "bytes ms-at-end" to fd.
void requester( const char* name, int fd )
{
    Display* dpy = XOpenDisplay( name );
    if ( !dpy )
        _exit( 2 );
    XSetWindowAttributes a;
    a.event_mask = PropertyChangeMask;
    Window w = XCreateWindow( dpy, DefaultRootWindow( dpy ), 0, 0, 1, 1, 0, 0, InputOnly, CopyFromParent, CWEventMask, &a );
    Atom prop = XInternAtom( dpy, "XSELTEST_IN", False );
    // an unrelated property change the reader's owner watches (on the root
    // window): not a step of its transfer, it must stay queued for it
    Atom noise = XInternAtom( dpy, "XSELTEST_NOISE", False );
    long one = 1;
    usleep( 200000 );
    XChangeProperty( dpy, DefaultRootWindow( dpy ), noise, XA_INTEGER, 32, PropModeReplace,
                     reinterpret_cast<unsigned char*>( &one ), 1 );
    XDeleteProperty( dpy, DefaultRootWindow( dpy ), noise );
    XConvertSelection( dpy, XInternAtom( dpy, "CLIPBOARD", False ), XInternAtom( dpy, "image/png", False ), prop, w, CurrentTime );
    XFlush( dpy );
    std::string got;
    bool incr = false;
    long long end = nowMs() + 8000;
    while ( nowMs() < end ) {
        if ( !XPending( dpy ) ) {
            usleep( 1000 );
            continue;
        }
        XEvent e;
        XNextEvent( dpy, &e );
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char* v = 0;
        if ( e.type == SelectionNotify ) {
            XGetWindowProperty( dpy, w, prop, 0, 0x1000000, True, AnyPropertyType, &type, &format, &n, &after, &v );
            if ( v )
                XFree( v );
            incr = true;
        } else if ( incr && e.type == PropertyNotify && e.xproperty.state == PropertyNewValue && e.xproperty.atom == prop ) {
            XGetWindowProperty( dpy, w, prop, 0, 0x1000000, True, AnyPropertyType, &type, &format, &n, &after, &v );
            if ( v ) {
                got.append( reinterpret_cast<char*>( v ), n );
                XFree( v );
            }
            if ( !n ) {
                char line[64];
                int len = snprintf( line, sizeof( line ), "%u %lld\n", unsigned( got.size() ), nowMs() );
                if ( write( fd, line, len ) != len )
                    _exit( 2 );
                _exit( 0 );
            }
        }
    }
    if ( write( fd, "0 0\n", 4 ) != 4 )
        _exit( 2 );
    _exit( 1 );
}

int failures = 0;

void check( bool ok, const char* what, const char* detail )
{
    printf( "%s %s (%s)\n", ok ? "ok  " : "FAIL", what, detail );
    if ( !ok )
        ++failures;
}

pid_t startOwner( const char* name, Mode mode, size_t size, int pieceDelayMs = 0 )
{
    int fds[2];
    if ( pipe( fds ) )
        exit( 2 );
    pid_t pid = fork();
    if ( pid == 0 ) {
        close( fds[0] );
        owner( name, mode, size, pieceDelayMs, fds[1] );
        _exit( 0 );
    }
    close( fds[1] );
    char c;
    if ( read( fds[0], &c, 1 ) != 1 ) {
        fprintf( stderr, "xseltest: the owner did not start\n" );
        exit( 2 );
    }
    close( fds[0] );
    return pid;
}

void stopOwner( pid_t pid )
{
    kill( pid, SIGTERM );
    waitpid( pid, 0, 0 );
}

struct Run
{
    KWPEXSelection::Result result;
    std::string data;
    long long ms;
};

Run readPng( Display* dpy, const KWPEXSelection::Limits& limits, const char* target = "image/png",
             const KWPEXSelection::Service* service = 0, const char* selection = "CLIPBOARD" )
{
    Run run;
    long long start = nowMs();
    run.result = KWPEXSelection::read( dpy, XInternAtom( dpy, selection, False ), XInternAtom( dpy, target, False ),
                                       CurrentTime, limits, run.data, 0, 0, service );
    run.ms = nowMs() - start;
    return run;
}

}

int main( int argc, char** argv )
{
    if ( argc < 2 || argv[1][0] != ':' || !strcmp( argv[1], ":0" ) || !strcmp( argv[1], ":1" )
         || !strncmp( argv[1], ":0.", 3 ) || !strncmp( argv[1], ":1.", 3 ) ) {
        fprintf( stderr, "usage: xseltest :DISPLAY (a nested X server, never :0 or :1)\n" );
        return 2;
    }
    Display* dpy = XOpenDisplay( argv[1] );
    if ( !dpy ) {
        fprintf( stderr, "xseltest: cannot open %s\n", argv[1] );
        return 2;
    }
    const KWPEXSelection::Limits limits = { 1000, 5000, 32 * 1024 * 1024 };
    char detail[200];

    // xseltest :D read TARGET: read the CLIPBOARD of another program (timing)
    if ( argc > 3 && !strcmp( argv[2], "read" ) ) {
        const KWPEXSelection::Limits generous = { 3000, 10000, 64 * 1024 * 1024 };
        Run run = readPng( dpy, generous, argv[3] );
        printf( "%s: %s, %u bytes, %lld ms\n", argv[3], KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
        return run.result == KWPEXSelection::Ok ? 0 : 1;
    }

    pid_t pid = startOwner( argv[1], Serve, 1000 );
    Run run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %u bytes, %lld ms", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
    check( run.result == KWPEXSelection::Ok && run.data == payload( 1000 ), "small", detail );
    run = readPng( dpy, limits, "image/x-unknown" );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::Refused && run.ms < 500, "refused", detail );
    {
        std::string data;
        int format = 0;
        KWPEXSelection::Result r = KWPEXSelection::read( dpy, XInternAtom( dpy, "CLIPBOARD", False ), XInternAtom( dpy, "TARGETS", False ),
                                                         CurrentTime, limits, data, 0, &format );
        snprintf( detail, sizeof( detail ), "%s, format %d, %u items", KWPEXSelection::resultName( r ), format, unsigned( data.size() / 4 ) );
        check( r == KWPEXSelection::Ok && format == 32 && data.size() == 8, "targets", detail );
    }
    stopOwner( pid );

    const size_t big = 8 * 1024 * 1024 + 12345;
    pid = startOwner( argv[1], Serve, big );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %u bytes, %lld ms", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
    check( run.result == KWPEXSelection::Ok && run.data == payload( big ) && run.ms < 1500, "incr 8 MB", detail );
    stopOwner( pid );

    pid = startOwner( argv[1], Silent, 1000 );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::Timeout && run.ms >= 900 && run.ms < 1600, "silent owner", detail );
    stopOwner( pid );

    pid = startOwner( argv[1], IncrStall, big );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::Timeout && run.data.empty() && run.ms < 1600, "incr stall", detail );
    stopOwner( pid );
    pid = startOwner( argv[1], Serve, 300000 );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %u bytes, %lld ms", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
    check( run.result == KWPEXSelection::Ok && run.data == payload( 300000 ), "read after a stall", detail );
    stopOwner( pid );

    // one property larger than the limit: refused without fetching it all
    pid = startOwner( argv[1], Serve, 200000 );
    {
        const KWPEXSelection::Limits small = { 1000, 5000, 100000 };
        run = readPng( dpy, small );
        snprintf( detail, sizeof( detail ), "%s, %u bytes, %lld ms", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
        check( run.result == KWPEXSelection::TooLarge && run.data.empty(), "too large, one property", detail );
        run = readPng( dpy, limits );
        snprintf( detail, sizeof( detail ), "%s, %u bytes", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ) );
        check( run.result == KWPEXSelection::Ok && run.data == payload( 200000 ), "next read", detail );
    }
    stopOwner( pid );

    pid = startOwner( argv[1], Huge, 1000000 );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::TooLarge && run.ms < 500, "too large", detail );
    stopOwner( pid );

    pid = startOwner( argv[1], Serve, 2 * 1024 * 1024, 100 );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %u bytes, %lld ms", KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms );
    check( run.result == KWPEXSelection::Ok && run.data == payload( 2 * 1024 * 1024 ), "slow pieces", detail );
    stopOwner( pid );

    const KWPEXSelection::Limits shortTotal = { 1000, 1000, 32 * 1024 * 1024 };
    pid = startOwner( argv[1], Serve, 4 * 1024 * 1024, 300 );
    run = readPng( dpy, shortTotal );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::Timeout && run.ms < 1400, "total limit", detail );
    stopOwner( pid );

    // the owner floods our window with property events and never answers:
    // the deadline still holds
    pid = startOwner( argv[1], Flood, 1000 );
    run = readPng( dpy, limits );
    snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
    check( run.result == KWPEXSelection::Timeout && run.ms < 1300, "flooded window", detail );
    stopOwner( pid );
    usleep( 300000 );
    XSync( dpy, True );   // the flood to the old window: an event loop would take it, here nobody does

    // pieces keep coming (progress all the time): the total limit still ends it
    {
        const KWPEXSelection::Limits endless = { 1000, 1000, 1024u * 1024 * 1024 };
        pid = startOwner( argv[1], Endless, 1024 * 1024, 5 );
        run = readPng( dpy, endless );
        snprintf( detail, sizeof( detail ), "%s, %lld ms", KWPEXSelection::resultName( run.result ), run.ms );
        check( run.result == KWPEXSelection::Timeout && run.ms < 1300, "total limit, endless pieces", detail );
        stopOwner( pid );
    }

    // Our own CLIPBOARD (1 MB, INCR) asked for by another client while we
    // read PRIMARY from a slow owner: the other transfer must not wait for
    // the end of ours.
    {
        OwnServer own;
        own.dpy = dpy;
        own.window = XCreateSimpleWindow( dpy, DefaultRootWindow( dpy ), 0, 0, 1, 1, 0, 0, 0 );
        own.clipboard = XInternAtom( dpy, "CLIPBOARD", False );
        own.png = XInternAtom( dpy, "image/png", False );
        own.targets = XInternAtom( dpy, "TARGETS", False );
        own.incr = XInternAtom( dpy, "INCR", False );
        own.data = payload( 1024 * 1024 + 7 );
        own.piece = 64 * 1024;
        own.requestor = 0;
        own.property = None;
        own.offset = 0;
        own.active = false;
        own.handled = 0;
        XSetSelectionOwner( dpy, own.clipboard, own.window, CurrentTime );
        XSelectInput( dpy, DefaultRootWindow( dpy ), PropertyChangeMask );
        XSync( dpy, False );
        s_selection = "PRIMARY";
        pid = startOwner( argv[1], Serve, 2 * 1024 * 1024, 100 );   // about 1 s
        s_selection = "CLIPBOARD";
        int fds[2];
        if ( pipe( fds ) )
            return 2;
        pid_t req = fork();
        if ( req == 0 ) {
            close( fds[0] );
            requester( argv[1], fds[1] );
        }
        close( fds[1] );
        KWPEXSelection::Service service = { ownWants, ownHandle, &own };
        run = readPng( dpy, limits, "image/png", &service, "PRIMARY" );
        long long readEnd = nowMs();
        char line[64] = { 0 };
        ssize_t got = read( fds[0], line, sizeof( line ) - 1 );
        close( fds[0] );
        waitpid( req, 0, 0 );
        stopOwner( pid );
        unsigned bytes = 0;
        long long doneAt = 0;
        if ( got > 0 )
            sscanf( line, "%u %lld", &bytes, &doneAt );
        snprintf( detail, sizeof( detail ), "our read %s %u bytes in %lld ms; theirs %u bytes, done %lld ms before ours",
                  KWPEXSelection::resultName( run.result ), unsigned( run.data.size() ), run.ms, bytes, readEnd - doneAt );
        check( run.result == KWPEXSelection::Ok && bytes == own.data.size() && doneAt && doneAt < readEnd,
               "own INCR served during a read", detail );
        Atom noise = XInternAtom( dpy, "XSELTEST_NOISE", False );
        int queued = 0;
        XEvent e;
        while ( XCheckTypedWindowEvent( dpy, DefaultRootWindow( dpy ), PropertyNotify, &e ) )
            if ( e.xproperty.atom == noise && e.xproperty.state == PropertyDelete )
                ++queued;
        snprintf( detail, sizeof( detail ), "%d such event(s) left in the queue", queued );
        check( queued == 1, "foreign PropertyDelete not serviced", detail );
        XSelectInput( dpy, DefaultRootWindow( dpy ), NoEventMask );
        XDestroyWindow( dpy, own.window );
    }

    printf( failures ? "%d FAILED\n" : "all passed\n", failures );
    return failures ? 1 : 0;
}
