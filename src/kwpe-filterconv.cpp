/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

/*
 * kwpe-filterconv: Adblock Plus filter lists -> compiled WebKit content
 * filters.  The part runs it in the background (KProcess): converting
 * EasyList and compiling the result into WebKit's byte code takes
 * seconds and a few hundred MiB, which should not happen in Konqueror.
 *
 *   kwpe-filterconv [--store DIR --id PREFIX] [--json DIR] [--max-rules N] [--verbose] LIST...
 *
 * --store: compile into the WebKitUserContentFilterStore at DIR, as
 *          PREFIX-0, PREFIX-1, ... (one per rule list);
 * --json:  also write the rule lists as DIR/PREFIX-N.json (for testing);
 * --verbose: every dropped filter on stderr.
 * Included files ("!#include name") are read from the directory of the
 * list that includes them (a plain file name only).
 *
 * Output on stdout, one item per line (the part reads it):
 *   stats KEY VALUE        counters of the conversion
 *   dropped REASON COUNT   filters that cannot be expressed
 *   filter ID RULES MS     a compiled rule list
 *   error TEXT             what failed (exit status 1)
 *   time convert|compile MS, memory KiB (peak RSS)
 */

#include <wpe/webkit.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include <string>
#include <vector>

#include "abpconverter.h"

static bool readFile( const std::string& path, std::string& text )
{
    FILE* f = fopen( path.c_str(), "rb" );
    if ( !f )
        return false;
    char buf[65536];
    size_t n;
    text.clear();
    while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) {
        text.append( buf, n );
        // Filter lists are a few MiB; anything far bigger is not one.
        if ( text.size() > 64u * 1024 * 1024 ) {
            fclose( f );
            return false;
        }
    }
    bool ok = !ferror( f );
    fclose( f );
    return ok;
}

static bool loadInclude( const std::string& name, std::string& text, void* data )
{
    // A file next to the list, nothing else.
    if ( name.empty() || name[0] == '.' || name.find( '/' ) != std::string::npos || name.find( '\\' ) != std::string::npos )
        return false;
    const std::string* dir = static_cast<const std::string*>( data );
    return readFile( *dir + "/" + name, text );
}

static double msSince( gint64 start )
{
    return ( g_get_monotonic_time() - start ) / 1000.0;
}

// ---------------------------------------------------------------------
// compiling with WebKit

struct Compile
{
    GMainLoop* loop;
    WebKitUserContentFilter* filter;
    GError* error;
};

static void compiled( GObject* store, GAsyncResult* result, gpointer data )
{
    Compile* c = static_cast<Compile*>( data );
    c->filter = webkit_user_content_filter_store_save_finish( WEBKIT_USER_CONTENT_FILTER_STORE( store ), result, &c->error );
    g_main_loop_quit( c->loop );
}

static bool compile( WebKitUserContentFilterStore* store, const std::string& id, const std::string& json, std::string& error )
{
    Compile c;
    c.loop = g_main_loop_new( 0, FALSE );
    c.filter = 0;
    c.error = 0;
    GBytes* bytes = g_bytes_new( json.data(), json.size() );
    webkit_user_content_filter_store_save( store, id.c_str(), bytes, 0, compiled, &c );
    g_main_loop_run( c.loop );
    g_main_loop_unref( c.loop );
    g_bytes_unref( bytes );
    if ( c.filter ) {
        webkit_user_content_filter_unref( c.filter );
        return true;
    }
    error = c.error ? c.error->message : "unknown error";
    if ( c.error )
        g_error_free( c.error );
    return false;
}

// Rules WebKit refuses (a WebKit version that reads some construct
// differently): found by halving, so that one bad rule does not cost the
// whole list.
static void findBadRules( WebKitUserContentFilterStore* store, const std::string& id,
                          const std::vector<ABP::Converter::Rule>& rules, size_t from, size_t to,
                          std::vector<bool>& bad, int depth )
{
    if ( from >= to || depth > 40 )
        return;
    std::vector<ABP::Converter::Rule> part( rules.begin() + from, rules.begin() + to );
    std::string error;
    if ( compile( store, id, ABP::toJSON( part ), error ) )
        return;
    if ( to - from == 1 ) {
        bad[from] = true;
        printf( "badrule %s\n", ABP::toJSON( part ).c_str() );
        return;
    }
    size_t mid = from + ( to - from ) / 2;
    findBadRules( store, id, rules, from, mid, bad, depth + 1 );
    findBadRules( store, id, rules, mid, to, bad, depth + 1 );
}

static long peakRSS()
{
    struct rusage ru;
    getrusage( RUSAGE_SELF, &ru );
    return ru.ru_maxrss;
}

int main( int argc, char** argv )
{
    std::string storeDir, prefix = "kwpe", jsonDir;
    unsigned maxRules = 50000;
    bool verbose = false;
    std::vector<std::string> lists;
    for ( int i = 1; i < argc; ++i ) {
        std::string a = argv[i];
        if ( a == "--store" && i + 1 < argc )
            storeDir = argv[++i];
        else if ( a == "--id" && i + 1 < argc )
            prefix = argv[++i];
        else if ( a == "--json" && i + 1 < argc )
            jsonDir = argv[++i];
        else if ( a == "--verbose" )
            verbose = true;
        else if ( a == "--max-rules" && i + 1 < argc )
            maxRules = unsigned( atoi( argv[++i] ) );
        else if ( !a.empty() && a[0] == '-' ) {
            fprintf( stderr, "usage: %s [--store DIR --id PREFIX] [--json DIR] [--max-rules N] LIST...\n", argv[0] );
            return 2;
        } else
            lists.push_back( a );
    }
    if ( lists.empty() || maxRules < 1000 || maxRules > 150000 ) {
        fprintf( stderr, "usage: %s [--store DIR --id PREFIX] [--json DIR] [--max-rules N] LIST...\n", argv[0] );
        return 2;
    }
    setvbuf( stdout, 0, _IOLBF, 0 );

    gint64 start = g_get_monotonic_time();
    ABP::Converter conv;
    conv.verbose = verbose;
    for ( size_t i = 0; i < lists.size(); ++i ) {
        std::string text;
        if ( !readFile( lists[i], text ) ) {
            printf( "error cannot read %s: %s\n", lists[i].c_str(), strerror( errno ) );
            return 1;
        }
        size_t slash = lists[i].rfind( '/' );
        std::string dir = slash == std::string::npos ? std::string( "." ) : lists[i].substr( 0, slash );
        conv.setIncludeLoader( loadInclude, &dir );
        conv.addList( text );
    }
    std::vector<std::vector<ABP::Converter::Rule> > ruleLists = conv.ruleLists( maxRules );
    const ABP::Stats& s = conv.stats;
    printf( "stats lines %u\nstats duplicates %u\nstats badfiltered %u\nstats network-block %u\n"
            "stats network-exception %u\nstats cosmetic-generic %u\nstats cosmetic-specific %u\n"
            "stats cosmetic-exceptions %u\nstats regex-converted %u\nstats dropped %u\n",
            s.lines, s.duplicates, s.badfiltered, s.networkBlock, s.networkException, s.cosmeticGeneric,
            s.cosmeticSpecific, s.cosmeticExceptions, s.regexConverted, s.droppedTotal() );
    for ( std::map<std::string, unsigned>::const_iterator it = s.dropped.begin(); it != s.dropped.end(); ++it )
        printf( "dropped %s %u\n", it->first.c_str(), it->second );
    printf( "time convert %.0f\n", msSince( start ) );

    if ( !jsonDir.empty() ) {
        for ( size_t i = 0; i < ruleLists.size(); ++i ) {
            char name[64];
            snprintf( name, sizeof( name ), "-%u.json", unsigned( i ) );
            std::string path = jsonDir + "/" + prefix + name;
            FILE* f = fopen( path.c_str(), "wb" );
            std::string json = ABP::toJSON( ruleLists[i] );
            if ( !f || fwrite( json.data(), 1, json.size(), f ) != json.size() ) {
                printf( "error cannot write %s\n", path.c_str() );
                if ( f )
                    fclose( f );
                return 1;
            }
            fclose( f );
        }
    }

    if ( !storeDir.empty() ) {
        gint64 compileStart = g_get_monotonic_time();
        WebKitUserContentFilterStore* store = webkit_user_content_filter_store_new( storeDir.c_str() );
        for ( size_t i = 0; i < ruleLists.size(); ++i ) {
            char suffix[32];
            snprintf( suffix, sizeof( suffix ), "-%u", unsigned( i ) );
            std::string id = prefix + suffix;
            std::vector<ABP::Converter::Rule>& rules = ruleLists[i];
            gint64 t = g_get_monotonic_time();
            std::string error;
            if ( !compile( store, id, ABP::toJSON( rules ), error ) ) {
                printf( "warning %s: %s; looking for the rules WebKit refuses\n", id.c_str(), error.c_str() );
                std::vector<bool> bad( rules.size(), false );
                findBadRules( store, id, rules, 0, rules.size(), bad, 0 );
                std::vector<ABP::Converter::Rule> good;
                for ( size_t k = 0; k < rules.size(); ++k )
                    if ( !bad[k] )
                        good.push_back( rules[k] );
                printf( "dropped webkit-refused %u\n", unsigned( rules.size() - good.size() ) );
                rules.swap( good );
                if ( rules.empty() || !compile( store, id, ABP::toJSON( rules ), error ) ) {
                    printf( "error %s: %s\n", id.c_str(), error.c_str() );
                    return 1;
                }
            }
            printf( "filter %s %u %.0f\n", id.c_str(), unsigned( rules.size() ), msSince( t ) );
        }
        g_object_unref( store );
        printf( "time compile %.0f\n", msSince( compileStart ) );
    }
    printf( "memory %ld\n", peakRSS() );
    return 0;
}
