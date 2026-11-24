/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// xseltest DISPLAY: KWPEXSelection against owners that behave and owners
// that do not, on a nested X server (never :0/:1).  An owner runs in a
// child process per case; it serves "image/png" (or stalls, refuses, ...).
//
//   ok small       one property
//   ok incr        8 MB in INCR pieces (the owner's piece size: the
//                  server's max request size, as Qt 3 and xclip do)
//   silent         no answer at all: Timeout after the idle time
//   incr stall     INCR, then no more pieces: Timeout, the next read works
//   refused        an unknown target: Refused at once
//   too large      INCR announcing more than the limit: TooLarge
//   slow pieces    INCR pieces 100 ms apart: Ok (progress keeps it going)
//   total          INCR pieces 300 ms apart, total limit 1 s: Timeout

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

enum Mode { Serve, Silent, IncrStall, Huge, SlowPieces };

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
void owner( const char* name, Mode mode, size_t size, int pieceDelayMs, int ready )
{
    Display* dpy = XOpenDisplay( name );
    if ( !dpy )
        _exit( 2 );
    Window w = XCreateSimpleWindow( dpy, DefaultRootWindow( dpy ), 0, 0, 1, 1, 0, 0, 0 );
    Atom clipboard = XInternAtom( dpy, "CLIPBOARD", False );
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

Run readPng( Display* dpy, const KWPEXSelection::Limits& limits, const char* target = "image/png" )
{
    Run run;
    long long start = nowMs();
    run.result = KWPEXSelection::read( dpy, XInternAtom( dpy, "CLIPBOARD", False ), XInternAtom( dpy, target, False ),
                                       CurrentTime, limits, run.data );
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

    printf( failures ? "%d FAILED\n" : "all passed\n", failures );
    return failures ? 1 : 0;
}
