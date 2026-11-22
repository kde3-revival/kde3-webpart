/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include "abpconverter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace ABP
{

// ---------------------------------------------------------------------
// helpers

static bool startsWith( const std::string& s, const char* prefix )
{
    return s.compare( 0, strlen( prefix ), prefix ) == 0;
}

static bool endsWith( const std::string& s, const std::string& suffix )
{
    return s.size() >= suffix.size() && s.compare( s.size() - suffix.size(), suffix.size(), suffix ) == 0;
}

static std::string trim( const std::string& s )
{
    size_t b = 0, e = s.size();
    while ( b < e && ( s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n' ) )
        ++b;
    while ( e > b && ( s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' ) )
        --e;
    return s.substr( b, e - b );
}

static std::vector<std::string> split( const std::string& s, char sep )
{
    std::vector<std::string> parts;
    size_t start = 0;
    for ( ;; ) {
        size_t pos = s.find( sep, start );
        parts.push_back( s.substr( start, pos == std::string::npos ? std::string::npos : pos - start ) );
        if ( pos == std::string::npos )
            break;
        start = pos + 1;
    }
    return parts;
}

static std::string lower( const std::string& s )
{
    std::string r = s;
    for ( size_t i = 0; i < r.size(); ++i )
        if ( r[i] >= 'A' && r[i] <= 'Z' )
            r[i] = char( r[i] - 'A' + 'a' );
    return r;
}

static bool isASCII( const std::string& s )
{
    for ( size_t i = 0; i < s.size(); ++i )
        if ( static_cast<unsigned char>( s[i] ) >= 0x80 )
            return false;
    return true;
}

// a is b or a subdomain of b
static bool domainWithin( const std::string& a, const std::string& b )
{
    return a == b || ( a.size() > b.size() && endsWith( a, b ) && a[a.size() - b.size() - 1] == '.' );
}

// A site: punycode, lower case; uBlock's entities ("example.*": any
// public suffix) too, they become a top URL pattern (siteFilters()).
// Regular expressions cannot be said.
static bool normalizeDomain( std::string& domain )
{
    domain = lower( trim( domain ) );
    if ( startsWith( domain, "*." ) )
        domain = domain.substr( 2 );
    std::string name = endsWith( domain, ".*" ) ? domain.substr( 0, domain.size() - 2 ) : domain;
    if ( name.empty() || name.size() > 253 || name[0] == '.' || name[name.size() - 1] == '.' )
        return false;
    for ( size_t i = 0; i < name.size(); ++i ) {
        char c = name[i];
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '.' || c == '-' || c == '_' ) )
            return false;
    }
    return true;
}

static void appendJSONString( std::string& out, const std::string& s )
{
    out += '"';
    for ( size_t i = 0; i < s.size(); ++i ) {
        unsigned char c = s[i];
        switch ( c ) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ( c < 0x20 ) {
                char buf[8];
                snprintf( buf, sizeof( buf ), "\\u%04x", c );
                out += buf;
            } else {
                out += char( c );
            }
        }
    }
    out += '"';
}

static void appendJSONList( std::string& out, const char* key, const std::vector<std::string>& list )
{
    out += ",\"";
    out += key;
    out += "\":[";
    for ( size_t i = 0; i < list.size(); ++i ) {
        if ( i )
            out += ',';
        appendJSONString( out, list[i] );
    }
    out += ']';
}

// The top URL is on one of the sites (a domain or a subdomain of it).
static std::vector<std::string> siteFilters( const std::vector<std::string>& domains )
{
    std::vector<std::string> filters;
    filters.reserve( domains.size() );
    for ( size_t i = 0; i < domains.size(); ++i ) {
        std::string f = "^[a-z][a-z0-9.+-]*://([^/?#]*\\.)?";
        bool entity = endsWith( domains[i], ".*" );
        std::string name = entity ? domains[i].substr( 0, domains[i].size() - 2 ) : domains[i];
        for ( size_t k = 0; k < name.size(); ++k ) {
            if ( name[k] == '.' )
                f += '\\';
            f += name[k];
        }
        if ( entity )
            f += "\\.[^/:?#]+";   // example.* : example.com, example.co.uk, ...
        f += "[:/]";
        filters.push_back( f );
    }
    return filters;
}

std::string toJSON( const std::vector<Converter::Rule>& rules )
{
    std::string out = "[";
    out.reserve( rules.size() * 160 );
    for ( size_t i = 0; i < rules.size(); ++i ) {
        const Converter::Rule& r = rules[i];
        if ( i )
            out += ",\n";
        out += "{\"trigger\":{\"url-filter\":";
        appendJSONString( out, r.urlFilter );
        if ( r.caseSensitive )
            out += ",\"url-filter-is-case-sensitive\":true";
        if ( !r.resourceTypes.empty() )
            appendJSONList( out, "resource-type", r.resourceTypes );
        if ( !r.loadType.empty() ) {
            std::vector<std::string> l( 1, r.loadType );
            appendJSONList( out, "load-type", l );
        }
        // Sites as anchored top URL patterns, not WebKit's "if-domain":
        // that one finds the domain anywhere in the URL ("?next=http://
        // example.com/" would count as example.com).
        if ( !r.ifDomain.empty() )
            appendJSONList( out, "if-top-url", siteFilters( r.ifDomain ) );
        else if ( !r.unlessDomain.empty() )
            appendJSONList( out, "unless-top-url", siteFilters( r.unlessDomain ) );
        else if ( !r.ifTopURL.empty() )
            appendJSONList( out, "if-top-url", r.ifTopURL );
        if ( !r.requestMethod.empty() ) {
            out += ",\"request-method\":";
            appendJSONString( out, r.requestMethod );
        }
        out += "},\"action\":{\"type\":";
        appendJSONString( out, r.action );
        if ( !r.selector.empty() ) {
            out += ",\"selector\":";
            appendJSONString( out, r.selector );
        }
        out += "}}";
    }
    out += "]\n";
    return out;
}

// ---------------------------------------------------------------------
// URL patterns

// ABP's separator "^": anything but a letter, a digit or one of _-.%
static const char separatorClass[] = "[^a-zA-Z0-9_.%-]";
// "||": the start of the host name or of one of its labels
static const char domainAnchor[] = "^[a-z][a-z0-9.+-]*:(//)?([^/?#]*\\.)?";

bool patternToURLFilter( const std::string& patternIn, std::string& urlFilter, std::string& whyNot )
{
    std::string pattern = patternIn;
    if ( !isASCII( pattern ) ) {
        whyNot = "non-ascii";
        return false;
    }
    std::string out;
    bool hostAnchor = false;
    if ( startsWith( pattern, "||" ) ) {
        hostAnchor = true;
        pattern = pattern.substr( 2 );
        out = domainAnchor;
    } else if ( startsWith( pattern, "|" ) ) {
        pattern = pattern.substr( 1 );
        out = "^";
    }
    bool endAnchor = false;
    if ( !pattern.empty() && pattern[pattern.size() - 1] == '|' ) {
        endAnchor = true;
        pattern.erase( pattern.size() - 1 );
    }
    // Leading/trailing wildcards say nothing (WebKit matches substrings).
    if ( !hostAnchor && out.empty() )
        while ( !pattern.empty() && pattern[0] == '*' )
            pattern.erase( 0, 1 );
    if ( !endAnchor )
        while ( !pattern.empty() && pattern[pattern.size() - 1] == '*' )
            pattern.erase( pattern.size() - 1 );

    // Is the pattern only a host name ("||example.com^")?  Then a "^" at
    // its end is always followed by something: WebKit's URLs have a path.
    bool hostOnly = hostAnchor;
    for ( size_t i = 0; hostOnly && i < pattern.size(); ++i ) {
        char c = pattern[i];
        if ( c == '^' && i == pattern.size() - 1 )
            break;
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '.' || c == '-' || c == '_' ) )
            hostOnly = false;
    }

    for ( size_t i = 0; i < pattern.size(); ++i ) {
        char c = pattern[i];
        switch ( c ) {
        case '*':
            if ( !endsWith( out, ".*" ) || endsWith( out, "\\.*" ) )
                out += ".*";
            break;
        case '^':
            if ( i == pattern.size() - 1 && !hostOnly ) {
                // the separator or the end of the address
                out += '(';
                out += separatorClass;
                out += endAnchor ? ")?" : ".*)?";
                out += '$';
                endAnchor = false;
            } else {
                out += separatorClass;
            }
            break;
        case '.': case '?': case '+': case '(': case ')': case '[': case ']':
        case '{': case '}': case '\\': case '$': case '|':
            out += '\\';
            out += c;
            break;
        default:
            if ( static_cast<unsigned char>( c ) < 0x20 || c == 0x7f ) {
                whyNot = "control-character";
                return false;
            }
            out += c;
        }
    }
    if ( endAnchor )
        out += '$';
    if ( out.empty() || out == "^" || out == ".*" )
        out = ".*";
    urlFilter = out;
    return true;
}

// --- /regex/ filters ---------------------------------------------------
//
// WebKit's url-filter is a small subset of regular expressions: literal
// characters, ".", character classes, groups, the quantifiers ?, * and +,
// "^" first and "$" last.  No alternatives, no \d \w \b, no {n,m}, no
// look-ahead.  Alternatives are expanded into several filters (at most
// maxAlternatives), \d and \w become classes, {n,m} with small numbers
// is written out; the rest is refused.

static const unsigned maxAlternatives = 48;
static const unsigned maxRepeat = 8;

struct RegexParser
{
    const std::string& s;
    size_t i;
    std::string whyNot;

    bool widen;   // an exception: what cannot be said matches more

    RegexParser( const std::string& source, bool wider ) : s( source ), i( 0 ), widen( wider ) {}

    bool fail( const char* why )
    {
        if ( whyNot.empty() )
            whyNot = why;
        return false;
    }

    // alternatives := sequence ('|' sequence)*
    bool parseAlternatives( std::vector<std::string>& result, int depth )
    {
        if ( depth > 8 )
            return fail( "regex-nesting" );
        result.clear();
        for ( ;; ) {
            std::vector<std::string> seq;
            if ( !parseSequence( seq, depth ) )
                return false;
            result.insert( result.end(), seq.begin(), seq.end() );
            if ( result.size() > maxAlternatives )
                return fail( "regex-alternatives" );
            if ( i < s.size() && s[i] == '|' ) {
                ++i;
                continue;
            }
            return true;
        }
    }

    bool parseClass( std::string& out )
    {
        // s[i] == '['
        out = "[";
        ++i;
        if ( i < s.size() && s[i] == '^' ) {
            out += '^';
            ++i;
        }
        bool first = true;
        while ( i < s.size() ) {
            char c = s[i];
            if ( c == ']' && !first ) {
                ++i;
                out += ']';
                return out != "[]" && out != "[^]";
            }
            first = false;
            if ( c == '[' ) {
                out += "\\[";
                ++i;
                continue;
            }
            if ( c == '\\' ) {
                if ( i + 1 >= s.size() )
                    return fail( "regex-syntax" );
                char e = s[i + 1];
                i += 2;
                if ( e == 'd' )
                    out += "0-9";
                else if ( e == 'w' )
                    out += "_a-zA-Z0-9";   // ends with a range: "[\\w-.]" stays a "-"
                else if ( ( e >= 'a' && e <= 'z' ) || ( e >= 'A' && e <= 'Z' ) || ( e >= '0' && e <= '9' ) )
                    return fail( "regex-class-escape" );
                else {
                    out += '\\';
                    out += e;
                }
                continue;
            }
            if ( static_cast<unsigned char>( c ) >= 0x80 || static_cast<unsigned char>( c ) < 0x20 )
                return fail( "non-ascii" );
            out += c;
            ++i;
        }
        return fail( "regex-syntax" );
    }

    // One atom: its alternatives (a group may have several).
    bool parseAtom( std::vector<std::string>& alts, bool& single, int depth )
    {
        char c = s[i];
        single = true;
        alts.clear();
        if ( c == '(' ) {
            ++i;
            if ( i < s.size() && s[i] == '?' ) {
                if ( i + 1 < s.size() && s[i + 1] == ':' )
                    i += 2;
                else
                    return fail( "regex-lookaround" );
            }
            std::vector<std::string> inner;
            if ( !parseAlternatives( inner, depth + 1 ) )
                return false;
            if ( i >= s.size() || s[i] != ')' )
                return fail( "regex-syntax" );
            ++i;
            for ( size_t k = 0; k < inner.size(); ++k )
                alts.push_back( "(" + inner[k] + ")" );
            single = false;
            return true;
        }
        if ( c == '[' ) {
            std::string cls;
            if ( !parseClass( cls ) )
                return fail( "regex-class" );
            alts.push_back( cls );
            return true;
        }
        if ( c == '\\' ) {
            if ( i + 1 >= s.size() )
                return fail( "regex-syntax" );
            char e = s[i + 1];
            i += 2;
            if ( e == 'd' ) alts.push_back( "[0-9]" );
            else if ( e == 'D' ) alts.push_back( "[^0-9]" );
            else if ( e == 'w' ) alts.push_back( "[a-zA-Z0-9_]" );
            else if ( e == 'W' ) alts.push_back( "[^a-zA-Z0-9_]" );
            else if ( ( e == 'b' || e == 'B' ) && widen ) { alts.push_back( std::string() ); return true; }
            else if ( e == 'b' || e == 'B' ) return fail( "regex-word-boundary" );
            else if ( ( e >= 'a' && e <= 'z' ) || ( e >= 'A' && e <= 'Z' ) || ( e >= '0' && e <= '9' ) )
                return fail( "regex-escape" );
            else if ( e == '/' || e == '-' || e == '=' || e == '&' || e == ':' || e == ',' || e == '_' || e == '%'
                      || e == '#' || e == ';' || e == '@' || e == '!' || e == '~' || e == '\'' || e == '"' || e == '<' || e == '>' )
                alts.push_back( std::string( 1, e ) );
            else if ( static_cast<unsigned char>( e ) < 0x80 )
                alts.push_back( std::string( "\\" ) + e );
            else
                return fail( "non-ascii" );
            return true;
        }
        if ( c == '.' ) {
            ++i;
            alts.push_back( "." );
            return true;
        }
        if ( c == '{' || c == '}' || c == ']' || c == '*' || c == '+' || c == '?' )
            return fail( "regex-syntax" );
        if ( static_cast<unsigned char>( c ) >= 0x80 || static_cast<unsigned char>( c ) < 0x20 )
            return fail( "non-ascii" );
        ++i;
        alts.push_back( std::string( 1, c ) );
        return true;
    }

    bool parseSequence( std::vector<std::string>& result, int depth )
    {
        result.assign( 1, std::string() );
        while ( i < s.size() && s[i] != '|' && s[i] != ')' ) {
            char c = s[i];
            if ( c == '^' ) {
                if ( depth != 0 || i != 0 )
                    return fail( "regex-anchor" );
                ++i;
                for ( size_t k = 0; k < result.size(); ++k )
                    result[k] += '^';
                continue;
            }
            if ( c == '$' ) {
                if ( depth != 0 || i != s.size() - 1 )
                    return fail( "regex-anchor" );
                ++i;
                for ( size_t k = 0; k < result.size(); ++k )
                    result[k] += '$';
                continue;
            }
            std::vector<std::string> alts;
            bool single;
            if ( !parseAtom( alts, single, depth ) )
                return false;

            // quantifier
            std::vector<std::string> quantified;
            if ( i < s.size() && ( s[i] == '*' || s[i] == '+' || s[i] == '?' ) ) {
                char q = s[i++];
                if ( i < s.size() && s[i] == '?' )
                    ++i;   // lazy: the same for matching
                if ( q == '?' ) {
                    if ( alts.size() == 1 ) {
                        quantified.push_back( alts[0] + "?" );
                    } else {
                        quantified = alts;
                        quantified.push_back( std::string() );
                    }
                } else {
                    if ( alts.size() != 1 )
                        return fail( "regex-alternatives" );
                    quantified.push_back( alts[0] + q );
                }
            } else if ( i < s.size() && s[i] == '{' ) {
                size_t close = s.find( '}', i );
                if ( close == std::string::npos )
                    return fail( "regex-syntax" );
                std::string spec = s.substr( i + 1, close - i - 1 );
                i = close + 1;
                if ( i < s.size() && s[i] == '?' )
                    ++i;
                unsigned lo = 0, hi = 0;
                bool open = false;
                size_t comma = spec.find( ',' );
                // "{,3}" is three characters in JavaScript, not a repeat
                if ( spec.empty() || spec[0] == ',' || spec.find_first_not_of( "0123456789," ) != std::string::npos )
                    return fail( "regex-syntax" );
                if ( comma == std::string::npos ) {
                    lo = hi = unsigned( atoi( spec.c_str() ) );
                } else {
                    lo = unsigned( atoi( spec.substr( 0, comma ).c_str() ) );
                    if ( comma + 1 == spec.size() )
                        open = true;
                    else
                        hi = unsigned( atoi( spec.substr( comma + 1 ).c_str() ) );
                }
                if ( alts.size() != 1 || ( !open && hi < lo ) )
                    return fail( "regex-repeat" );
                if ( widen && ( lo > maxRepeat || hi > maxRepeat ) ) {
                    // an exception: "one or more" (or "any") instead
                    open = true;
                    lo = lo ? 1 : 0;
                }
                if ( lo > maxRepeat || ( !open && hi > maxRepeat ) )
                    return fail( "regex-repeat" );
                std::string a = alts[0];
                std::string r;
                for ( unsigned k = 0; k < lo; ++k )
                    r += a;
                if ( open )
                    r += single ? a + "*" : "(" + a + ")*";
                else
                    for ( unsigned k = lo; k < hi; ++k )
                        r += single ? a + "?" : "(" + a + ")?";
                quantified.push_back( r );
            } else {
                quantified = alts;
            }

            std::vector<std::string> next;
            for ( size_t a = 0; a < result.size(); ++a )
                for ( size_t b = 0; b < quantified.size(); ++b )
                    next.push_back( result[a] + quantified[b] );
            if ( next.size() > maxAlternatives )
                return fail( "regex-alternatives" );
            result.swap( next );
        }
        return true;
    }
};

bool regexToURLFilters( const std::string& regex, std::vector<std::string>& urlFilters, std::string& whyNot, bool widen )
{
    if ( !isASCII( regex ) ) {
        whyNot = "non-ascii";
        return false;
    }
    RegexParser p( regex, widen );
    std::vector<std::string> alts;
    if ( !p.parseAlternatives( alts, 0 ) || p.i != regex.size() ) {
        whyNot = p.whyNot.empty() ? "regex-syntax" : p.whyNot;
        return false;
    }
    urlFilters.clear();
    for ( size_t k = 0; k < alts.size(); ++k ) {
        std::string f = alts[k];
        if ( f.empty() || f == "^" || f == "$" || f == "^$" ) {
            whyNot = "regex-empty";
            return false;
        }
        urlFilters.push_back( f );
    }
    return true;
}

// Can the url-filter (WebKit's subset, as written here) match an empty
// string?  WebKit then applies the rule to every load.
bool canMatchEmpty( const std::string& filterIn )
{
    std::string f = filterIn;
    if ( !f.empty() && f[0] == '^' )
        f.erase( 0, 1 );
    if ( !f.empty() && f[f.size() - 1] == '$' && ( f.size() < 2 || f[f.size() - 2] != '\\' ) )
        f.erase( f.size() - 1 );
    size_t i = 0;
    while ( i < f.size() ) {
        size_t start = i;
        std::string inner;
        bool group = false;
        if ( f[i] == '\\' ) {
            i += 2;
        } else if ( f[i] == '[' ) {
            ++i;
            if ( i < f.size() && f[i] == '^' ) ++i;
            if ( i < f.size() && f[i] == ']' ) ++i;
            while ( i < f.size() && f[i] != ']' )
                i += f[i] == '\\' ? 2 : 1;
            ++i;
        } else if ( f[i] == '(' ) {
            int depth = 0;
            for ( ; i < f.size(); ++i ) {
                if ( f[i] == '\\' ) { ++i; continue; }
                if ( f[i] == '[' ) { while ( i < f.size() && f[i] != ']' ) i += f[i] == '\\' ? 2 : 1; continue; }
                if ( f[i] == '(' ) ++depth;
                else if ( f[i] == ')' && --depth == 0 ) { ++i; break; }
            }
            inner = f.substr( start + 1, i - start - 2 );
            group = true;
        } else {
            ++i;
        }
        char q = i < f.size() ? f[i] : 0;
        if ( q == '?' || q == '*' || q == '+' )
            ++i;
        bool optional = q == '?' || q == '*';
        if ( !optional && !( group && canMatchEmpty( inner ) ) )
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Converter

Stats::Stats()
    : lines( 0 ), duplicates( 0 ), badfiltered( 0 ), networkBlock( 0 ), networkException( 0 ),
      cosmeticGeneric( 0 ), cosmeticSpecific( 0 ), cosmeticExceptions( 0 ), regexConverted( 0 ),
      scriptlets( 0 ), scriptletExceptions( 0 )
{
}

unsigned Stats::droppedTotal() const
{
    unsigned n = 0;
    for ( std::map<std::string, unsigned>::const_iterator it = dropped.begin(); it != dropped.end(); ++it )
        n += it->second;
    return n;
}

Converter::Converter()
    : verbose( false ), m_includeLoader( 0 ), m_includeData( 0 ), m_trusted( false )
{
}

void Converter::setIncludeLoader( IncludeLoader loader, void* data )
{
    m_includeLoader = loader;
    m_includeData = data;
}

void Converter::drop( const char* reason )
{
    stats.dropped[reason]++;
    if ( verbose )
        fprintf( stderr, "drop %s %s\n", reason, m_current.c_str() );
}

bool Converter::active() const
{
    for ( size_t i = 0; i < m_ifStack.size(); ++i )
        if ( !m_ifStack[i] )
            return false;
    return true;
}

// uBlock's "!#if": env_safari is true (its Safari variants are written
// for WebKit's content blockers), every other token false; "!" negates;
// && and || from left to right.
static bool evalCondition( const std::string& expr )
{
    std::string e;
    for ( size_t i = 0; i < expr.size(); ++i )
        if ( expr[i] != ' ' && expr[i] != '(' && expr[i] != ')' )
            e += expr[i];
    bool result = false;
    char op = 0;
    size_t i = 0;
    while ( i <= e.size() ) {
        size_t next = e.size();
        char nextOp = 0;
        size_t a = e.find( "&&", i ), o = e.find( "||", i );
        if ( a != std::string::npos && ( o == std::string::npos || a < o ) ) {
            next = a;
            nextOp = '&';
        } else if ( o != std::string::npos ) {
            next = o;
            nextOp = '|';
        }
        std::string tok = e.substr( i, next - i );
        bool neg = false;
        while ( !tok.empty() && tok[0] == '!' ) {
            neg = !neg;
            tok.erase( 0, 1 );
        }
        bool v = ( tok == "env_safari" ) != neg;
        if ( op == 0 )
            result = v;
        else if ( op == '&' )
            result = result && v;
        else
            result = result || v;
        if ( next >= e.size() )
            break;
        op = nextOp;
        i = next + 2;
    }
    return result;
}

void Converter::addList( const std::string& text, bool trusted )
{
    m_ifStack.clear();
    m_trusted = trusted;
    addText( text, 0 );
    m_trusted = false;
    m_ifStack.clear();
}

void Converter::addText( const std::string& text, int depth )
{
    size_t start = 0;
    while ( start < text.size() ) {
        size_t end = text.find( '\n', start );
        if ( end == std::string::npos )
            end = text.size();
        addLine( text.substr( start, end - start ), depth );
        start = end + 1;
    }
}

void Converter::addLine( const std::string& raw, int depth )
{
    std::string line = trim( raw );
    if ( line.empty() )
        return;
    if ( startsWith( line, "!#if " ) ) {
        m_ifStack.push_back( evalCondition( line.substr( 5 ) ) );
        return;
    }
    if ( startsWith( line, "!#else" ) ) {
        if ( !m_ifStack.empty() )
            m_ifStack.back() = !m_ifStack.back();
        return;
    }
    if ( startsWith( line, "!#endif" ) ) {
        if ( !m_ifStack.empty() )
            m_ifStack.pop_back();
        return;
    }
    if ( !active() )
        return;
    if ( startsWith( line, "!#include " ) ) {
        std::string name = trim( line.substr( 10 ) );
        std::string text;
        if ( depth < 3 && m_includeLoader && m_includeLoader( name, text, m_includeData ) ) {
            addText( text, depth + 1 );
        } else {
            m_current = line;
            drop( "include-missing" );
        }
        return;
    }
    if ( line[0] == '!' || line[0] == '[' )
        return;
    // hosts-file comments ("# ..."), not "##sel" or "#@#sel"
    if ( line[0] == '#' && line.size() > 1 && line[1] != '#' && line[1] != '@' && line[1] != '?' && line[1] != '$' && line[1] != '%' )
        return;
    stats.lines++;
    std::map<std::string, size_t>::iterator seen = m_seen.find( line );
    if ( seen != m_seen.end() ) {
        stats.duplicates++;
        // a trusted list has it too: it is trusted
        if ( m_trusted )
            m_filterTrusted[seen->second] = true;
        return;
    }
    m_seen[line] = m_filters.size();
    m_filters.push_back( line );
    m_filterTrusted.push_back( m_trusted );
}

// The separator of a cosmetic filter ("##", "#@#", "#?#", ...): its
// position and kind, or npos.
static size_t cosmeticSeparator( const std::string& f, std::string& kind )
{
    size_t pos = 0;
    while ( ( pos = f.find( '#', pos ) ) != std::string::npos ) {
        static const char* const markers[] = { "##", "#@#", "#?#", "#@?#", "#$#", "#@$#", "#%#", "#@%#", "#$?#", "#@$?#" };
        for ( unsigned m = 0; m < sizeof( markers ) / sizeof( markers[0] ); ++m ) {
            if ( f.compare( pos, strlen( markers[m] ), markers[m] ) == 0 ) {
                // what comes before must be a domain list
                std::string domains = f.substr( 0, pos );
                if ( domains.find_first_of( "/|^$@\"!" ) != std::string::npos )
                    return std::string::npos;
                kind = markers[m];
                return pos;
            }
        }
        ++pos;
    }
    return std::string::npos;
}

static size_t cosmeticSeparatorPos( const std::string& f )
{
    std::string kind;
    return cosmeticSeparator( f, kind );
}

// uBlock's procedural cosmetic operators, ABP's extended selectors: they
// need a script, a content blocker cannot run them.
static bool isProcedural( const std::string& selector )
{
    static const char* const ops[] = {
        ":has-text(", ":-abp-contains(", ":contains(", ":matches-css", ":-abp-properties(", ":xpath(", ":upward(",
        ":remove(", ":remove-attr(", ":remove-class(", ":style(", ":watch-attr(", ":min-text-length(", ":others(",
        ":matches-path(", ":matches-attr(", ":matches-prop(", ":matches-media(", ":if(", ":if-not(",
        ":nth-ancestor(", ":spath(", ":shadow(", ":-abp-", "{", "}"
    };
    for ( unsigned i = 0; i < sizeof( ops ) / sizeof( ops[0] ); ++i )
        if ( selector.find( ops[i] ) != std::string::npos )
            return true;
    return false;
}

static std::string replaceAll( std::string s, const std::string& from, const std::string& to )
{
    size_t pos = 0;
    while ( ( pos = s.find( from, pos ) ) != std::string::npos ) {
        s.replace( pos, from.size(), to );
        pos += to.size();
    }
    return s;
}

void Converter::convertCosmetic( const std::string& domainList, const std::string& selectorIn, bool exception )
{
    std::string selector = trim( selectorIn );
    // ABP's :-abp-has() is CSS's :has()
    selector = replaceAll( selector, ":-abp-has(", ":has(" );
    if ( selector.empty() ) {
        drop( "cosmetic-empty" );
        return;
    }
    if ( selector[0] == '^' ) {
        drop( "html-filter" );
        return;
    }
    if ( isProcedural( selector ) ) {
        drop( "procedural-cosmetic" );
        return;
    }
    // brackets, parentheses and quotes must balance (WebKit drops invalid
    // selectors itself, this only keeps garbage out of the JSON)
    int paren = 0, bracket = 0;
    char quote = 0;
    for ( size_t i = 0; i < selector.size(); ++i ) {
        char c = selector[i];
        if ( quote ) {
            if ( c == '\\' )
                ++i;
            else if ( c == quote )
                quote = 0;
            continue;
        }
        if ( c == '"' || c == '\'' ) quote = c;
        else if ( c == '(' ) ++paren;
        else if ( c == ')' ) --paren;
        else if ( c == '[' ) ++bracket;
        else if ( c == ']' ) --bracket;
        if ( paren < 0 || bracket < 0 )
            break;
    }
    if ( quote || paren || bracket ) {
        drop( "cosmetic-syntax" );
        return;
    }

    std::vector<std::string> include, exclude;
    bool unsupportedInclude = false, unsupportedExclude = false;
    if ( !trim( domainList ).empty() ) {
        std::vector<std::string> entries = split( domainList, ',' );
        for ( size_t i = 0; i < entries.size(); ++i ) {
            std::string d = trim( entries[i] );
            bool neg = !d.empty() && d[0] == '~';
            if ( neg )
                d.erase( 0, 1 );
            if ( d == "*" && !neg )
                continue;   // uBlock: "*##sel" is generic
            if ( !normalizeDomain( d ) ) {
                ( neg ? unsupportedExclude : unsupportedInclude ) = true;
                continue;
            }
            ( neg ? exclude : include ).push_back( d );
        }
    }

    if ( exception ) {
        // "#@#sel" (everywhere) or "sites#@#sel"; an exception we cannot
        // read completely is applied everywhere: hiding less is safe.
        std::set<std::string>& sites = m_cosmeticExceptions[selector];
        if ( include.empty() || unsupportedInclude )
            sites.insert( std::string() );
        else
            sites.insert( include.begin(), include.end() );
        return;
    }
    if ( unsupportedExclude ) {
        drop( "cosmetic-domain" );
        return;
    }
    if ( unsupportedInclude && include.empty() ) {
        drop( "cosmetic-domain" );
        return;
    }
    Cosmetic c;
    c.selector = selector;
    c.include = include;
    c.exclude = exclude;
    m_cosmetic.push_back( c );
}

// ---------------------------------------------------------------------
// scriptlets

namespace
{
struct ScriptletName
{
    const char* name;      // canonical (the part's library has these)
    const char* aliases;   // space-separated
    bool trusted;          // only from trusted lists
};

// What the part's library implements (src/scriptlets.js).  Scriptlets
// that can put code or arbitrary content into a page are trusted-only,
// as in uBlock; replace-node-text too (it rewrites inline scripts).
const ScriptletName scriptletNames[] = {
    { "set-constant", "set", false },
    { "abort-on-property-read", "aopr", false },
    { "abort-on-property-write", "aopw", false },
    { "abort-current-script", "acs abort-current-inline-script acis", false },
    { "abort-on-stack-trace", "aost", false },
    { "no-setTimeout-if", "nostif prevent-setTimeout setTimeout-defuser", false },
    { "no-setInterval-if", "nosiif prevent-setInterval setInterval-defuser", false },
    { "adjust-setTimeout", "nano-stb nano-setTimeout-booster", false },
    { "adjust-setInterval", "nano-sib nano-setInterval-booster", false },
    { "noeval-if", "prevent-eval-if noeval", false },
    { "remove-attr", "ra", false },
    { "remove-class", "rc", false },
    { "json-prune", "", false },
    { "json-prune-fetch-response", "", false },
    { "json-prune-xhr-response", "", false },
    { "prevent-fetch", "no-fetch-if", false },
    { "prevent-xhr", "no-xhr-if", false },
    { "remove-node-text", "rmnt", false },
    { "prevent-window-open", "nowoif window.open-defuser no-window-open-if", false },
    { "prevent-addEventListener", "aeld addEventListener-defuser", false },
    { "trusted-set-constant", "trusted-set", true },
    { "trusted-replace-fetch-response", "trusted-rpfr", true },
    { "trusted-replace-xhr-response", "trusted-rpxr", true },
    { "trusted-prevent-dom-bypass", "", true },
    { "replace-node-text", "rpnt", true },
    { "trusted-replace-node-text", "trusted-rpnt", true },
};

const ScriptletName* findScriptlet( const std::string& name )
{
    for ( unsigned i = 0; i < sizeof( scriptletNames ) / sizeof( scriptletNames[0] ); ++i ) {
        if ( name == scriptletNames[i].name )
            return &scriptletNames[i];
        std::vector<std::string> aliases = split( scriptletNames[i].aliases, ' ' );
        for ( size_t a = 0; a < aliases.size(); ++a )
            if ( !aliases[a].empty() && name == aliases[a] )
                return &scriptletNames[i];
    }
    return 0;
}

// The arguments of "+js(...)": separated by commas ("\," is one), each
// trimmed; an argument in quotes (' " `) is what is inside them (commas
// need no escape there, "\<quote>" is the quote).
bool splitScriptletArgs( const std::string& text, std::vector<std::string>& args )
{
    size_t i = 0;
    const size_t n = text.size();
    for ( ;; ) {
        while ( i < n && ( text[i] == ' ' || text[i] == '\t' ) )
            ++i;
        std::string arg;
        char q = i < n ? text[i] : 0;
        bool quoted = false;
        if ( q == '\'' || q == '"' || q == '`' ) {
            // a quoted argument: up to the closing quote, then a comma
            size_t j = i + 1;
            std::string inner;
            bool closed = false;
            for ( ; j < n; ++j ) {
                if ( text[j] == '\\' && j + 1 < n && text[j + 1] == q ) {
                    inner += q;
                    ++j;
                } else if ( text[j] == q ) {
                    closed = true;
                    break;
                } else {
                    inner += text[j];
                }
            }
            size_t k = j + 1;
            while ( closed && k < n && ( text[k] == ' ' || text[k] == '\t' ) )
                ++k;
            if ( closed && ( k == n || text[k] == ',' ) ) {
                arg = inner;
                quoted = true;
                i = k;
            }
        }
        if ( !quoted ) {
            while ( i < n && text[i] != ',' ) {
                if ( text[i] == '\\' && i + 1 < n && text[i + 1] == ',' ) {
                    arg += ',';
                    i += 2;
                } else {
                    arg += text[i++];
                }
            }
            arg = trim( arg );
        }
        args.push_back( arg );
        if ( i >= n )
            break;
        ++i;   // the comma
    }
    return true;
}
}

static std::string scriptletKey( const std::string& name, const std::vector<std::string>& args )
{
    std::string key = name;
    for ( size_t i = 0; i < args.size(); ++i ) {
        key += '\0';
        key += args[i];
    }
    return key;
}

bool parseScriptlet( const std::string& bodyIn, bool trusted, std::string& name, std::vector<std::string>& args,
                     std::string& whyNot )
{
    std::string body = trim( bodyIn );
    args.clear();
    name.clear();
    if ( !startsWith( body, "+js(" ) || body[body.size() - 1] != ')' ) {
        whyNot = "scriptlet-syntax";
        return false;
    }
    std::string inner = body.substr( 4, body.size() - 5 );
    if ( trim( inner ).empty() )
        return true;   // "+js()": every scriptlet (exceptions only)
    std::vector<std::string> parts;
    splitScriptletArgs( inner, parts );
    std::string raw = parts[0];
    if ( endsWith( raw, ".js" ) )
        raw = raw.substr( 0, raw.size() - 3 );
    const ScriptletName* sn = findScriptlet( raw );
    if ( !sn ) {
        whyNot = "scriptlet-unsupported";
        return false;
    }
    if ( sn->trusted && !trusted ) {
        whyNot = "scriptlet-untrusted";
        return false;
    }
    name = sn->name;
    // "noeval" is noeval-if without a needle
    if ( raw == "noeval" )
        parts.resize( 1 );
    args.assign( parts.begin() + 1, parts.end() );
    // trailing empty arguments mean nothing ("set, x, 1, " = "set, x, 1")
    while ( !args.empty() && args.back().empty() )
        args.pop_back();
    if ( args.size() > 32 ) {
        whyNot = "scriptlet-syntax";
        return false;
    }
    return true;
}

void Converter::convertScriptlet( const std::string& domainList, const std::string& body, bool exception, bool trusted )
{
    std::string name, why;
    std::vector<std::string> args;
    // exceptions name scriptlets of any list
    if ( !parseScriptlet( body, trusted || exception, name, args, why ) ) {
        drop( why.c_str() );
        return;
    }
    if ( !exception && name.empty() ) {
        drop( "scriptlet-syntax" );
        return;
    }
    std::vector<std::string> include, exclude;
    bool unsupportedInclude = false, unsupportedExclude = false;
    std::vector<std::string> entries = split( domainList, ',' );
    for ( size_t i = 0; i < entries.size(); ++i ) {
        std::string d = trim( entries[i] );
        if ( d.empty() )
            continue;
        bool neg = d[0] == '~';
        if ( neg )
            d.erase( 0, 1 );
        if ( d == "*" && !neg )
            continue;
        if ( !normalizeDomain( d ) ) {
            ( neg ? unsupportedExclude : unsupportedInclude ) = true;
            continue;
        }
        ( neg ? exclude : include ).push_back( d );
    }
    if ( exception ) {
        // one we cannot read completely: everywhere (injecting less is safe)
        std::set<std::string>& sites = m_scriptletExceptions[name.empty() ? std::string() : scriptletKey( name, args )];
        if ( include.empty() || unsupportedInclude )
            sites.insert( std::string() );
        else
            sites.insert( include.begin(), include.end() );
        return;
    }
    if ( unsupportedExclude || ( unsupportedInclude && include.empty() ) ) {
        drop( "scriptlet-domain" );
        return;
    }
    if ( include.empty() ) {
        // uBlock runs generic scriptlets on every page; that is not
        // worth the cost (and a risk) for a handful of filters
        drop( "scriptlet-generic" );
        return;
    }
    Scriptlet sc;
    sc.name = name;
    sc.args = args;
    sc.include = include;
    sc.exclude = exclude;
    m_scriptlets.push_back( sc );
}

std::string scriptletsText( const std::vector<Converter::Scriptlet>& scriptlets )
{
    std::string out = "kwpe-scriptlets 1\n";
    for ( size_t i = 0; i < scriptlets.size(); ++i ) {
        const Converter::Scriptlet& sc = scriptlets[i];
        for ( size_t k = 0; k < sc.include.size(); ++k )
            out += ( k ? "," : "" ) + sc.include[k];
        out += '\t';
        for ( size_t k = 0; k < sc.exclude.size(); ++k )
            out += ( k ? "," : "" ) + sc.exclude[k];
        out += "\t[";
        appendJSONString( out, sc.name );
        for ( size_t k = 0; k < sc.args.size(); ++k ) {
            out += ',';
            appendJSONString( out, sc.args[k] );
        }
        out += "]\n";
    }
    return out;
}

namespace
{
enum TypeBit {
    TScript = 1 << 0, TImage = 1 << 1, TStyle = 1 << 2, TFont = 1 << 3, TMedia = 1 << 4, TSubdoc = 1 << 5,
    TFetch = 1 << 6, TWebSocket = 1 << 7, TPing = 1 << 8, TOther = 1 << 9, TSVG = 1 << 10,
    TPopup = 1 << 11, TDocument = 1 << 12, TUnsupported = 1 << 13
};
const unsigned defaultTypes = TScript | TImage | TStyle | TFont | TMedia | TSubdoc | TFetch | TWebSocket | TPing | TOther | TSVG;

struct TypeName { const char* abp; unsigned bit; };
const TypeName typeNames[] = {
    { "script", TScript }, { "image", TImage }, { "stylesheet", TStyle }, { "css", TStyle },
    { "font", TFont }, { "media", TMedia }, { "subdocument", TSubdoc }, { "frame", TSubdoc },
    { "xmlhttprequest", TFetch }, { "xhr", TFetch }, { "websocket", TWebSocket },
    { "ping", TPing }, { "beacon", TPing }, { "other", TOther }, { "popup", TPopup },
    { "document", TDocument }, { "doc", TDocument },
    { "object", TUnsupported }, { "object-subrequest", TUnsupported }, { "webrtc", TUnsupported },
    { "popunder", TUnsupported },
};

std::vector<std::string> webkitTypes( unsigned bits )
{
    std::vector<std::string> t;
    if ( bits & TDocument ) t.push_back( ( bits & TSubdoc ) ? "document" : "top-document" );
    else if ( bits & TSubdoc ) t.push_back( "child-document" );
    if ( bits & TScript ) t.push_back( "script" );
    if ( bits & TImage ) t.push_back( "image" );
    if ( bits & TStyle ) t.push_back( "style-sheet" );
    if ( bits & TFont ) t.push_back( "font" );
    if ( bits & TMedia ) t.push_back( "media" );
    if ( bits & TSVG ) t.push_back( "svg-document" );
    // "raw" is fetch + websocket + other + ping
    if ( ( bits & ( TFetch | TWebSocket | TOther | TPing ) ) == ( TFetch | TWebSocket | TOther | TPing ) ) {
        t.push_back( "raw" );
    } else {
        if ( bits & TFetch ) t.push_back( "fetch" );
        if ( bits & TWebSocket ) t.push_back( "websocket" );
        if ( bits & TOther ) t.push_back( "other" );
        if ( bits & TPing ) t.push_back( "ping" );
    }
    if ( bits & TPopup ) t.push_back( "popup" );
    return t;
}
}

void Converter::convertNetwork( const std::string& filterIn )
{
    std::string filter = filterIn;
    bool exception = startsWith( filter, "@@" );
    if ( exception )
        filter = filter.substr( 2 );

    // pattern and options
    std::string pattern = filter, options;
    bool isRegex = false;
    if ( filter.size() > 2 && filter[0] == '/' ) {
        size_t end = filter.rfind( "/$" );
        if ( filter[filter.size() - 1] == '/' ) {
            isRegex = true;
            pattern = filter.substr( 1, filter.size() - 2 );
        } else if ( end != std::string::npos && end > 0 ) {
            isRegex = true;
            pattern = filter.substr( 1, end - 1 );
            options = filter.substr( end + 2 );
        }
    }
    if ( !isRegex ) {
        size_t dollar = filter.rfind( '$' );
        if ( dollar != std::string::npos ) {
            pattern = filter.substr( 0, dollar );
            options = filter.substr( dollar + 1 );
        }
    }

    unsigned types = 0, notTypes = 0;
    std::string loadType;
    bool caseSensitive = false, important = false;
    bool docException = false, elemhide = false, generichide = false, specifichide = false;
    std::vector<std::string> include, exclude;
    bool unsupportedInclude = false, unsupportedExclude = false;
    std::string method;
    bool methodUnsupported = false;

    if ( !options.empty() ) {
        std::vector<std::string> opts = split( options, ',' );
        for ( size_t k = 0; k < opts.size(); ++k ) {
            std::string opt = lower( trim( opts[k] ) );
            std::string value;
            size_t eq = opt.find( '=' );
            if ( eq != std::string::npos ) {
                value = trim( opts[k] ).substr( eq + 1 );
                opt = opt.substr( 0, eq );
            }
            bool neg = !opt.empty() && opt[0] == '~';
            if ( neg )
                opt.erase( 0, 1 );

            bool known = false;
            for ( unsigned t = 0; t < sizeof( typeNames ) / sizeof( typeNames[0] ); ++t ) {
                if ( opt == typeNames[t].abp ) {
                    ( neg ? notTypes : types ) |= typeNames[t].bit;
                    known = true;
                    break;
                }
            }
            if ( known )
                continue;
            if ( opt == "third-party" || opt == "3p" ) {
                loadType = neg ? "first-party" : "third-party";
            } else if ( opt == "first-party" || opt == "1p" ) {
                loadType = neg ? "third-party" : "first-party";
            } else if ( opt == "domain" || opt == "from" ) {
                std::vector<std::string> entries = split( value, '|' );
                for ( size_t e = 0; e < entries.size(); ++e ) {
                    std::string d = entries[e];
                    bool dneg = !d.empty() && d[0] == '~';
                    if ( dneg )
                        d.erase( 0, 1 );
                    if ( !normalizeDomain( d ) ) {
                        ( dneg ? unsupportedExclude : unsupportedInclude ) = true;
                        continue;
                    }
                    ( dneg ? exclude : include ).push_back( d );
                }
            } else if ( opt == "match-case" ) {
                caseSensitive = !neg;
            } else if ( opt == "important" ) {
                important = true;
            } else if ( opt == "elemhide" || opt == "ehide" ) {
                elemhide = true;
            } else if ( opt == "generichide" || opt == "ghide" ) {
                generichide = true;
            } else if ( opt == "specifichide" || opt == "shide" ) {
                specifichide = true;
            } else if ( opt == "all" ) {
                types |= defaultTypes | TPopup | TDocument;
            } else if ( opt == "method" ) {
                // WebKit compares request-method with the method of the
                // document, not of the request: blocks are dropped, an
                // exception applies to every method.
                methodUnsupported = true;
            } else if ( exception && ( opt == "to" || opt == "denyallow" ) ) {
                // narrows an exception: without it, it applies more widely
            } else if ( opt == "_" || opt == "badfilter" ) {
                // uBlock's no-op option; badfilter was handled before
            } else {
                // redirect, redirect-rule, removeparam, csp, rewrite, header,
                // permissions, replace, urlskip, to, denyallow, sitekey,
                // genericblock, inline-script, empty, strict1p, ...: either
                // not expressible, or blocking without them breaks pages.
                drop( ( "option-" + opt ).c_str() );
                return;
            }
        }
    }

    // $elemhide & co. say something only about exceptions; on a blocking
    // filter (an error in a list) they must not leave a block of everything
    if ( !exception && ( elemhide || generichide || specifichide ) ) {
        drop( "hide-option-on-block" );
        return;
    }

    // document-level exceptions
    bool docLevel = exception && ( types & TDocument );
    if ( exception && ( docLevel || elemhide || generichide || specifichide ) ) {
        docException = docLevel;
        std::string topFilter, why;
        bool patternEmpty = pattern.empty() || pattern == "*" || pattern == "|" || pattern == "||";
        Rule r;
        r.urlFilter = ".*";
        r.action = "ignore-previous-rules";
        if ( !patternEmpty ) {
            if ( isRegex ) {
                std::vector<std::string> filters;
                if ( !regexToURLFilters( pattern, filters, why, true ) ) {
                    drop( why.c_str() );
                    return;
                }
                r.ifTopURL = filters;
            } else {
                if ( !patternToURLFilter( pattern, topFilter, why ) ) {
                    drop( why.c_str() );
                    return;
                }
                if ( topFilter != ".*" )
                    r.ifTopURL.push_back( topFilter );
            }
        }
        if ( r.ifTopURL.empty() ) {
            if ( include.empty() ) {
                // "@@*$document" without sites: would switch everything off
                drop( "exception-everything" );
                return;
            }
            for ( size_t k = 0; k < include.size(); ++k )
                r.ifDomain.push_back( include[k] );
        }
        if ( docException )
            m_documentExceptions.push_back( r );
        if ( elemhide && !docException )
            m_elemhideExceptions.push_back( r );
        if ( generichide && !docException && !elemhide )
            m_generichideExceptions.push_back( r );
        if ( specifichide && !docException && !elemhide )
            m_specifichideExceptions.push_back( r );
        stats.networkException++;
        types &= ~TDocument;
        if ( !( types & ~TDocument ) && !notTypes )
            return;   // nothing but document-level options
    }

    // resource types
    if ( types & TUnsupported ) {
        types &= ~TUnsupported;
        if ( !types ) {
            drop( "type-unsupported" );
            return;
        }
    }
    unsigned effective = types ? types : defaultTypes;
    if ( notTypes ) {
        if ( !types )
            effective = defaultTypes;
        effective &= ~notTypes;
        if ( !effective ) {
            drop( "type-unsupported" );
            return;
        }
    }

    // domains
    std::vector<std::string> ifDomain, unlessDomain;
    std::vector<std::string> carveOut;   // block: sites inside ifDomain the rule must spare
    if ( !exception ) {
        if ( unsupportedExclude ) {
            drop( "domain-unsupported" );
            return;
        }
        if ( unsupportedInclude && include.empty() ) {
            drop( "domain-unsupported" );
            return;
        }
        if ( !include.empty() ) {
            for ( size_t k = 0; k < include.size(); ++k ) {
                bool excluded = false;
                for ( size_t e = 0; e < exclude.size(); ++e )
                    if ( domainWithin( include[k], exclude[e] ) )
                        excluded = true;
                if ( !excluded )
                    ifDomain.push_back( include[k] );
            }
            for ( size_t e = 0; e < exclude.size(); ++e )
                for ( size_t k = 0; k < include.size(); ++k )
                    if ( exclude[e] != include[k] && domainWithin( exclude[e], include[k] ) ) {
                        carveOut.push_back( exclude[e] );
                        break;
                    }
            if ( ifDomain.empty() ) {
                drop( "domain-empty" );
                return;
            }
        } else {
            for ( size_t e = 0; e < exclude.size(); ++e )
                unlessDomain.push_back( exclude[e] );
        }
        if ( methodUnsupported ) {
            drop( "option-method" );
            return;
        }
    } else {
        // An exception we cannot read completely applies more widely:
        // that blocks less, which is safe.
        if ( !unsupportedInclude ) {
            for ( size_t k = 0; k < include.size(); ++k )
                ifDomain.push_back( include[k] );
            if ( ifDomain.empty() && !unsupportedExclude )
                for ( size_t e = 0; e < exclude.size(); ++e )
                    unlessDomain.push_back( exclude[e] );
        }
        if ( methodUnsupported )
            method.clear();
    }

    // URL filters
    std::vector<std::string> urlFilters;
    std::string why;
    if ( isRegex ) {
        if ( !regexToURLFilters( pattern, urlFilters, why, exception ) ) {
            drop( why.c_str() );
            return;
        }
        stats.regexConverted++;
    } else {
        std::string f;
        if ( !patternToURLFilter( pattern, f, why ) ) {
            drop( why.c_str() );
            return;
        }
        urlFilters.push_back( f );
    }

    // A block of everything, without a site, would break the web; only
    // pings (beacons) are never needed by a page.
    if ( !exception && urlFilters.size() == 1 && urlFilters[0] == ".*" && ifDomain.empty() && ( effective & ~TPing ) ) {
        drop( "matches-everything" );
        return;
    }
    // A pattern that can match nothing at all ("/(ads)?/", "^"): WebKit
    // applies it to every load.
    for ( size_t k = 0; !exception && k < urlFilters.size(); ++k ) {
        if ( urlFilters[k] != ".*" && canMatchEmpty( urlFilters[k] ) ) {
            drop( "matches-everything" );
            return;
        }
    }

    for ( size_t k = 0; k < urlFilters.size(); ++k ) {
        Rule r;
        r.urlFilter = urlFilters[k];
        r.caseSensitive = caseSensitive;
        r.resourceTypes = webkitTypes( effective );
        r.loadType = loadType;
        r.ifDomain = ifDomain;
        r.unlessDomain = unlessDomain;
        r.requestMethod = method;
        if ( exception ) {
            r.action = "ignore-previous-rules";
            m_exceptions.push_back( r );
            stats.networkException++;
        } else {
            r.action = "block";
            ( important ? m_important : m_block ).push_back( r );
            stats.networkBlock++;
            if ( !carveOut.empty() ) {
                Rule spare = r;
                spare.action = "ignore-previous-rules";
                spare.ifDomain = carveOut;
                spare.unlessDomain.clear();
                // after the block it spares ($important ones come after
                // every other exception)
                ( important ? m_importantCarveOuts : m_exceptions ).push_back( spare );
            }
        }
    }
}

// A network filter with its options sorted and "badfilter" left out
// (*badfilter: it was there).
static std::string canonicalFilter( const std::string& f, bool* badfilter )
{
    *badfilter = false;
    size_t dollar = f.rfind( '$' );
    if ( dollar == std::string::npos || cosmeticSeparatorPos( f ) != std::string::npos )
        return f;
    std::vector<std::string> opts = split( f.substr( dollar + 1 ), ',' ), keep;
    for ( size_t k = 0; k < opts.size(); ++k ) {
        std::string o = trim( opts[k] );
        if ( lower( o ) == "badfilter" )
            *badfilter = true;
        else
            keep.push_back( o );
    }
    std::sort( keep.begin(), keep.end() );
    std::string key = f.substr( 0, dollar );
    for ( size_t k = 0; k < keep.size(); ++k )
        key += ( k ? "," : "$" ) + keep[k];
    return key;
}

std::vector<std::vector<Converter::Rule> > Converter::ruleLists( unsigned maxRules )
{
    // $badfilter: the filter it names (without the option, options in any
    // order) is disabled, and itself.
    for ( size_t i = 0; i < m_filters.size(); ++i ) {
        bool bad = false;
        std::string key = canonicalFilter( m_filters[i], &bad );
        if ( bad )
            m_badfilters.insert( key );
    }
    for ( size_t i = 0; i < m_filters.size(); ++i ) {
        const std::string& f = m_filters[i];
        m_current = f;
        bool isBadfilter = false;
        std::string key = m_badfilters.empty() ? std::string() : canonicalFilter( f, &isBadfilter );
        if ( isBadfilter || ( !m_badfilters.empty() && m_badfilters.count( key ) ) ) {
            stats.badfiltered++;
            continue;
        }
        std::string kind;
        size_t sep = cosmeticSeparator( f, kind );
        if ( sep != std::string::npos ) {
            std::string domains = f.substr( 0, sep );
            std::string body = f.substr( sep + kind.size() );
            if ( ( kind == "##" || kind == "#@#" ) && startsWith( trim( body ), "+js(" ) )
                convertScriptlet( domains, trim( body ), kind == "#@#", m_filterTrusted[i] );
            else if ( kind == "##" )
                convertCosmetic( domains, body, false );
            else if ( kind == "#@#" )
                convertCosmetic( domains, body, true );
            else if ( kind == "#?#" || kind == "#@?#" )
                drop( "extended-css" );
            else if ( kind == "#$#" || kind == "#@$#" || kind == "#$?#" || kind == "#@$?#" )
                drop( "snippet-or-css-injection" );
            else
                drop( "adguard-script" );
            continue;
        }
        convertNetwork( f );
    }

    // scriptlets, with the exceptions applied
    if ( !m_scriptletExceptions.empty() ) {
        std::vector<Scriptlet> kept;
        const std::set<std::string>* all = 0;
        if ( m_scriptletExceptions.count( std::string() ) )
            all = &m_scriptletExceptions[std::string()];
        for ( size_t i = 0; i < m_scriptlets.size(); ++i ) {
            Scriptlet& sc = m_scriptlets[i];
            std::string key = scriptletKey( sc.name, sc.args );
            std::map<std::string, std::set<std::string> >::const_iterator ex = m_scriptletExceptions.find( key );
            const std::set<std::string>* lists[] = { all, ex != m_scriptletExceptions.end() ? &ex->second : 0 };
            bool everywhere = false;
            for ( unsigned l = 0; l < 2; ++l ) {
                if ( !lists[l] )
                    continue;
                stats.scriptletExceptions++;
                for ( std::set<std::string>::const_iterator it = lists[l]->begin(); it != lists[l]->end(); ++it ) {
                    if ( it->empty() )
                        everywhere = true;
                    else
                        sc.exclude.push_back( *it );
                }
            }
            if ( !everywhere )
                kept.push_back( sc );
        }
        m_scriptlets.swap( kept );
    }
    stats.scriptlets = m_scriptlets.size();

    // cosmetic filters, with the exceptions applied
    std::vector<Rule> generic, specific;
    for ( size_t i = 0; i < m_cosmetic.size(); ++i ) {
        const Cosmetic& c = m_cosmetic[i];
        std::map<std::string, std::set<std::string> >::const_iterator ex = m_cosmeticExceptions.find( c.selector );
        std::vector<std::string> exclude = c.exclude;
        if ( ex != m_cosmeticExceptions.end() ) {
            if ( ex->second.count( std::string() ) ) {
                stats.cosmeticExceptions++;
                continue;
            }
            exclude.insert( exclude.end(), ex->second.begin(), ex->second.end() );
            stats.cosmeticExceptions++;
        }
        Rule r;
        r.urlFilter = ".*";
        r.action = "css-display-none";
        r.selector = c.selector;
        if ( c.include.empty() ) {
            for ( size_t e = 0; e < exclude.size(); ++e )
                r.unlessDomain.push_back( exclude[e] );
            generic.push_back( r );
            stats.cosmeticGeneric++;
            continue;
        }
        // Sites minus the excluded ones; a site with an excluded
        // subdomain cannot be said ("a.com but not x.a.com"): left out.
        for ( size_t k = 0; k < c.include.size(); ++k ) {
            bool keep = true;
            for ( size_t e = 0; e < exclude.size() && keep; ++e )
                if ( domainWithin( c.include[k], exclude[e] ) || domainWithin( exclude[e], c.include[k] ) )
                    keep = false;
            if ( keep )
                r.ifDomain.push_back( c.include[k] );
        }
        if ( r.ifDomain.empty() ) {
            m_current = c.selector;
            drop( "cosmetic-all-sites-excepted" );
            continue;
        }
        specific.push_back( r );
        stats.cosmeticSpecific++;
    }

    std::vector<std::vector<Rule> > lists;
    // network: blocks in slices, each followed by every exception, the
    // $important blocks (exceptions do not cancel them) and the $document
    // exceptions (which do)
    std::vector<Rule> tail = m_exceptions;
    tail.insert( tail.end(), m_important.begin(), m_important.end() );
    tail.insert( tail.end(), m_importantCarveOuts.begin(), m_importantCarveOuts.end() );
    tail.insert( tail.end(), m_documentExceptions.begin(), m_documentExceptions.end() );
    // css: generic ones before any ignore-previous-rules: WebKit puts those
    // into one style sheet for every page
    std::vector<Rule> genericTail = m_documentExceptions;
    genericTail.insert( genericTail.end(), m_elemhideExceptions.begin(), m_elemhideExceptions.end() );
    genericTail.insert( genericTail.end(), m_generichideExceptions.begin(), m_generichideExceptions.end() );
    std::vector<Rule> specificTail = m_documentExceptions;
    specificTail.insert( specificTail.end(), m_elemhideExceptions.begin(), m_elemhideExceptions.end() );
    specificTail.insert( specificTail.end(), m_specifichideExceptions.begin(), m_specifichideExceptions.end() );

    struct Part { const std::vector<Rule>* head; const std::vector<Rule>* tail; };
    Part parts[] = { { &m_block, &tail }, { &generic, &genericTail }, { &specific, &specificTail } };
    for ( unsigned p = 0; p < 3; ++p ) {
        const std::vector<Rule>& head = *parts[p].head;
        const std::vector<Rule>& t = *parts[p].tail;
        if ( head.empty() && ( p != 0 || m_important.empty() ) )
            continue;
        size_t room = maxRules > t.size() + 1000 ? maxRules - t.size() : 1000;
        size_t pos = 0;
        do {
            size_t n = std::min( room, head.size() - pos );
            std::vector<Rule> list( head.begin() + pos, head.begin() + pos + n );
            list.insert( list.end(), t.begin(), t.end() );
            lists.push_back( list );
            pos += n;
        } while ( pos < head.size() );
    }
    return lists;
}

}
