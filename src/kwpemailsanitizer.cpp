/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#include "kwpemailsanitizer.h"

#include <gumbo.h>

#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

namespace KWPEMailSanitizer
{

// ---------------------------------------------------------------------
// URLs

static std::string lower( const std::string& s )
{
    std::string r( s );
    for ( size_t i = 0; i < r.size(); ++i )
        if ( r[i] >= 'A' && r[i] <= 'Z' )
            r[i] = char( r[i] - 'A' + 'a' );
    return r;
}

static bool startsWith( const std::string& s, const char* prefix )
{
    return s.compare( 0, strlen( prefix ), prefix ) == 0;
}

// As the URL parser sees it: no leading or trailing C0 controls and
// spaces, no tabs and newlines anywhere.
static std::string normalizedUrl( const std::string& url )
{
    size_t begin = 0, end = url.size();
    while ( begin < end && (unsigned char)url[begin] <= 0x20 )
        ++begin;
    while ( end > begin && (unsigned char)url[end - 1] <= 0x20 )
        --end;
    std::string r;
    for ( size_t i = begin; i < end; ++i )
        if ( url[i] != '\t' && url[i] != '\n' && url[i] != '\r' )
            r += url[i];
    return r;
}

// The scheme in lower case; empty: a relative URL.
static std::string schemeOf( const std::string& url )
{
    if ( url.empty() || !( ( url[0] >= 'a' && url[0] <= 'z' ) || ( url[0] >= 'A' && url[0] <= 'Z' ) ) )
        return std::string();
    for ( size_t i = 1; i < url.size(); ++i ) {
        char c = url[i];
        if ( c == ':' )
            return lower( url.substr( 0, i ) );
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '+' || c == '-' || c == '.' ) )
            return std::string();
    }
    return std::string();
}

// data:<type>[;...],...: the type in lower case.
static std::string dataType( const std::string& url )
{
    size_t end = url.find_first_of( ";,", 5 );
    std::string type = url.substr( 5, end == std::string::npos ? std::string::npos : end - 5 );
    size_t b = type.find_first_not_of( " \t" ), e = type.find_last_not_of( " \t" );
    return b == std::string::npos ? std::string() : lower( type.substr( b, e - b + 1 ) );
}

static bool isDataImage( const std::string& type )
{
    // Not SVG: a document with references of its own (WebKit does not load
    // them for an image, but this layer does not depend on that).
    return startsWith( type, "image/" ) && type.find( "svg" ) == std::string::npos;
}

static bool isDataFont( const std::string& type )
{
    return startsWith( type, "font/" ) || startsWith( type, "application/font-" ) || startsWith( type, "application/x-font" )
           || type == "application/vnd.ms-fontobject" || type == "application/octet-stream";
}

bool allowedUrl( const std::string& rawUrl, UrlUse use, Profile profile, bool external )
{
    std::string url = normalizedUrl( rawUrl );
    if ( url.empty() )
        return false;
    if ( url[0] == '#' )
        return true;   // the same document
    std::string scheme = schemeOf( url );
    if ( scheme.empty() ) {
        // Relative: the message's own origin (kwpemail://m<n>/), which
        // serves the message's directory only.  Not //host (another host
        // of the scheme).  Content: nothing of its own but what the host
        // put in (cid: -> data:).
        if ( profile != Host )
            return false;
        if ( url.size() > 1 && ( url[0] == '/' || url[0] == '\\' ) && ( url[1] == '/' || url[1] == '\\' ) )
            return false;
        return true;
    }
    if ( use == Link ) {
        if ( scheme == "http" || scheme == "https" || scheme == "mailto" || scheme == "ftp" || scheme == "news" )
            return true;
        if ( profile == Host )
            // KMail's own links (kmail:, attachment files, ...): the host
            // gets them on a click; never script.
            return scheme != "javascript" && scheme != "vbscript" && scheme != "livescript" && scheme != "data";
        return false;
    }
    if ( scheme == "data" ) {
        std::string type = dataType( url );
        if ( use == Image )
            return isDataImage( type );
        if ( use == Font )
            return isDataFont( type ) || isDataImage( type );
        return false;   // a style sheet of its own: @import from data: could import more
    }
    if ( scheme == "cid" )
        return use != Style;   // never loaded: the host turns them into data:
    if ( scheme == "http" || scheme == "https" )
        return external;
    return false;
}

// ---------------------------------------------------------------------
// CSS

namespace
{

enum FunctionKind { OtherFunction, UrlStringsFunction };

struct CssFunction
{
    FunctionKind kind;
    size_t outStart;   // where the function starts in the output
    bool poisoned;     // var(), attr(), env() inside: strings we cannot check
};

class CssSanitizer
{
public:
    CssSanitizer( const std::string& css, Profile profile, bool external )
        : m_profile( profile ), m_external( external ), m_pos( 0 ), m_importPending( false )
    {
        // Preprocessing (CSS Syntax 3.3): CR LF, CR, FF are LF.
        m_s.reserve( css.size() );
        for ( size_t i = 0; i < css.size(); ++i ) {
            char c = css[i];
            if ( c == '\r' ) {
                m_s += '\n';
                if ( i + 1 < css.size() && css[i + 1] == '\n' )
                    ++i;
            } else if ( c == '\f' ) {
                m_s += '\n';
            } else {
                m_s += c;
            }
        }
    }

    std::string run();

private:
    char at( size_t i ) const { return i < m_s.size() ? m_s[i] : '\0'; }
    bool atEnd( size_t i ) const { return i >= m_s.size(); }
    static bool isNameStart( char c ) { return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_' || (unsigned char)c >= 0x80; }
    static bool isName( char c ) { return isNameStart( c ) || ( c >= '0' && c <= '9' ) || c == '-'; }
    static bool isHex( char c ) { return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ); }
    static bool isSpace( char c ) { return c == ' ' || c == '\t' || c == '\n'; }
    bool validEscape( size_t i ) const { return at( i ) == '\\' && !atEnd( i + 1 ) && at( i + 1 ) != '\n'; }
    bool startsIdent( size_t i ) const
    {
        if ( at( i ) == '-' )
            return isNameStart( at( i + 1 ) ) || at( i + 1 ) == '-' || validEscape( i + 1 );
        return isNameStart( at( i ) ) || validEscape( i );
    }
    void consumeEscape( std::string& decoded );
    std::string consumeName();
    bool consumeString( std::string& decoded );
    void consumeUrl();
    void checkUrl( size_t rawStart, const std::string& url, UrlUse use, const char* replacement );
    void closeFunction();
    void writeUrl( const std::string& value );
    void appendRaw( size_t start, size_t end );

    std::string m_s;
    std::string m_out;
    Profile m_profile;
    bool m_external;
    size_t m_pos;
    bool m_importPending;
    std::vector<CssFunction> m_functions;
};

static void appendUtf8( std::string& s, unsigned long cp )
{
    if ( cp == 0 || cp > 0x10FFFF || ( cp >= 0xD800 && cp <= 0xDFFF ) )
        cp = 0xFFFD;
    if ( cp < 0x80 ) {
        s += char( cp );
    } else if ( cp < 0x800 ) {
        s += char( 0xC0 | ( cp >> 6 ) );
        s += char( 0x80 | ( cp & 0x3F ) );
    } else if ( cp < 0x10000 ) {
        s += char( 0xE0 | ( cp >> 12 ) );
        s += char( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
        s += char( 0x80 | ( cp & 0x3F ) );
    } else {
        s += char( 0xF0 | ( cp >> 18 ) );
        s += char( 0x80 | ( ( cp >> 12 ) & 0x3F ) );
        s += char( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
        s += char( 0x80 | ( cp & 0x3F ) );
    }
}

// At a backslash that starts a valid escape.
void CssSanitizer::consumeEscape( std::string& decoded )
{
    ++m_pos;   // the backslash
    if ( atEnd( m_pos ) ) {
        appendUtf8( decoded, 0xFFFD );
        return;
    }
    if ( isHex( at( m_pos ) ) ) {
        unsigned long cp = 0;
        for ( int n = 0; n < 6 && isHex( at( m_pos ) ); ++n, ++m_pos ) {
            char c = at( m_pos );
            cp = cp * 16 + ( c <= '9' ? c - '0' : ( c | 0x20 ) - 'a' + 10 );
        }
        if ( isSpace( at( m_pos ) ) )
            ++m_pos;
        appendUtf8( decoded, cp );
        return;
    }
    // One code point as it is (all bytes of a UTF-8 sequence).
    decoded += at( m_pos++ );
    while ( !atEnd( m_pos ) && ( (unsigned char)at( m_pos ) & 0xC0 ) == 0x80 )
        decoded += at( m_pos++ );
}

std::string CssSanitizer::consumeName()
{
    std::string name;
    for ( ;; ) {
        if ( isName( at( m_pos ) ) && !atEnd( m_pos ) )
            name += at( m_pos++ );
        else if ( validEscape( m_pos ) )
            consumeEscape( name );
        else
            return name;
    }
}

// At the quote.  False: a bad string (a newline before the end).
bool CssSanitizer::consumeString( std::string& decoded )
{
    char quote = at( m_pos++ );
    while ( !atEnd( m_pos ) ) {
        char c = at( m_pos );
        if ( c == quote ) {
            ++m_pos;
            return true;
        }
        if ( c == '\n' )
            return false;   // the newline is not part of it
        if ( c == '\\' ) {
            if ( atEnd( m_pos + 1 ) ) {
                ++m_pos;
                continue;
            }
            if ( at( m_pos + 1 ) == '\n' ) {
                m_pos += 2;   // a continued line
                continue;
            }
            consumeEscape( decoded );
            continue;
        }
        decoded += c;
        ++m_pos;
    }
    return true;   // EOF ends it
}

void CssSanitizer::checkUrl( size_t rawStart, const std::string& url, UrlUse use, const char* replacement )
{
    bool ok = allowedUrl( url, use, m_profile, m_external );
    if ( use == Image && !ok )
        ok = allowedUrl( url, Font, m_profile, m_external );
    if ( ok )
        appendRaw( rawStart, m_pos );
    else
        m_out += replacement;
}

// After "url(" (the name and the parenthesis consumed), m_pos at what
// follows the parenthesis; the output has nothing of it yet.
void CssSanitizer::consumeUrl()
{
    UrlUse use = m_importPending ? Style : Image;
    m_importPending = false;
    size_t p = m_pos;
    while ( isSpace( at( p ) ) )
        ++p;
    if ( at( p ) == '"' || at( p ) == '\'' ) {
        // url("..." [modifiers]): a function with a string.
        m_pos = p;
        std::string value;
        consumeString( value );
        // the rest up to the closing parenthesis (nested ones included)
        int depth = 0;
        while ( !atEnd( m_pos ) ) {
            char c = at( m_pos );
            if ( c == '\\' && !atEnd( m_pos + 1 ) ) {
                m_pos += 2;
                continue;
            }
            if ( c == '"' || c == '\'' ) {
                std::string ignored;
                consumeString( ignored );
                continue;
            }
            ++m_pos;
            if ( c == '(' )
                ++depth;
            else if ( c == ')' && depth-- == 0 )
                break;
        }
        bool ok = allowedUrl( value, use, m_profile, m_external ) || ( use == Image && allowedUrl( value, Font, m_profile, m_external ) );
        // Our own spelling (no modifiers).
        if ( ok )
            writeUrl( value );
        else
            m_out += "url(data:,)";
        return;
    }
    // An unquoted url token (CSS Syntax 4.3.6).
    m_pos = p;
    std::string value;
    bool bad = false;
    while ( !atEnd( m_pos ) ) {
        char c = at( m_pos );
        if ( c == ')' ) {
            ++m_pos;
            break;
        }
        if ( isSpace( c ) ) {
            while ( isSpace( at( m_pos ) ) )
                ++m_pos;
            if ( at( m_pos ) == ')' || atEnd( m_pos ) ) {
                if ( !atEnd( m_pos ) )
                    ++m_pos;
                break;
            }
            bad = true;
            break;
        }
        if ( c == '"' || c == '\'' || c == '(' || ( (unsigned char)c < 0x20 && c != '\t' ) || c == 0x7F ) {
            bad = true;
            break;
        }
        if ( c == '\\' ) {
            if ( validEscape( m_pos ) ) {
                consumeEscape( value );
                continue;
            }
            bad = true;
            break;
        }
        value += c;
        ++m_pos;
    }
    if ( bad ) {
        // The remnants of a bad url (4.3.14): never loaded.
        while ( !atEnd( m_pos ) ) {
            if ( at( m_pos ) == ')' ) {
                ++m_pos;
                break;
            }
            if ( validEscape( m_pos ) ) {
                std::string ignored;
                consumeEscape( ignored );
                continue;
            }
            ++m_pos;
        }
        m_out += "url(data:,)";
        return;
    }
    bool ok = allowedUrl( value, use, m_profile, m_external ) || ( use == Image && allowedUrl( value, Font, m_profile, m_external ) );
    if ( ok )
        writeUrl( value );
    else
        m_out += "url(data:,)";
}

void CssSanitizer::writeUrl( const std::string& value )
{
    m_out += "url(\"";
    for ( size_t i = 0; i < value.size(); ++i ) {
        char c = value[i];
        if ( c == '"' || c == '\\' || c == '<' || c == '\n' ) {
            char hex[8];
            snprintf( hex, sizeof( hex ), "\\%x ", (unsigned)(unsigned char)c );
            m_out += hex;
        } else {
            m_out += c;
        }
    }
    m_out += "\")";
}

void CssSanitizer::appendRaw( size_t start, size_t end )
{
    // never "</style" in the output ("<!--" means nothing here)
    for ( size_t i = start; i < end; ++i ) {
        if ( m_s[i] == '<' )
            m_out += "\\3c ";
        else
            m_out += m_s[i];
    }
}

void CssSanitizer::closeFunction()
{
    if ( m_functions.empty() )
        return;
    CssFunction f = m_functions.back();
    m_functions.pop_back();
    if ( f.kind == UrlStringsFunction && f.poisoned ) {
        m_out.erase( f.outStart );
        m_out += "none";
    }
}

std::string CssSanitizer::run()
{
    while ( !atEnd( m_pos ) ) {
        char c = at( m_pos );
        size_t start = m_pos;
        if ( c == '/' && at( m_pos + 1 ) == '*' ) {
            size_t end = m_s.find( "*/", m_pos + 2 );
            m_pos = end == std::string::npos ? m_s.size() : end + 2;
            m_out += ' ';   // comments go; a space keeps tokens apart
            continue;
        }
        if ( c == '"' || c == '\'' ) {
            std::string value;
            consumeString( value );
            bool urlString = m_importPending || ( !m_functions.empty() && m_functions.back().kind == UrlStringsFunction );
            if ( urlString ) {
                checkUrl( start, value, m_importPending ? Style : Image, "\"data:,\"" );
                m_importPending = false;
            } else {
                appendRaw( start, m_pos );
            }
            continue;
        }
        if ( c == '@' && startsIdent( m_pos + 1 ) ) {
            ++m_pos;
            std::string name = lower( consumeName() );
            if ( name == "import" )
                m_importPending = true;
            appendRaw( start, m_pos );
            continue;
        }
        if ( startsIdent( m_pos ) ) {
            std::string name = lower( consumeName() );
            if ( at( m_pos ) == '(' ) {
                ++m_pos;
                if ( name == "url" ) {
                    consumeUrl();
                    continue;
                }
                bool urlStrings = name == "src" || name == "image-set" || name == "-webkit-image-set" || name == "image"
                                  || name == "-webkit-image" || name == "cross-fade" || name == "-webkit-cross-fade";
                if ( ( name == "var" || name == "attr" || name == "env" ) )
                    for ( size_t i = 0; i < m_functions.size(); ++i )
                        if ( m_functions[i].kind == UrlStringsFunction )
                            m_functions[i].poisoned = true;
                CssFunction f;
                f.kind = urlStrings ? UrlStringsFunction : OtherFunction;
                f.outStart = m_out.size();
                f.poisoned = false;
                m_functions.push_back( f );
                appendRaw( start, m_pos );
                continue;
            }
            appendRaw( start, m_pos );
            continue;
        }
        ++m_pos;
        switch ( c ) {
        case '\\':
            // not a valid escape (a backslash before a newline): a delimiter
            m_out += "\\";
            break;
        case '(':
        case '[':
        case '{': {
            if ( c == '{' )
                m_importPending = false;
            CssFunction f;
            f.kind = OtherFunction;
            f.outStart = m_out.size();
            f.poisoned = false;
            m_functions.push_back( f );
            m_out += c;
            break;
        }
        case ')':
        case ']':
        case '}':
            m_out += c;
            closeFunction();
            break;
        case ';':
            m_importPending = false;
            m_out += c;
            break;
        case '<':
            appendRaw( start, m_pos );
            break;
        default:
            m_out += c;
            break;
        }
    }
    // Functions left open at the end are closed by the parser: a poisoned
    // one goes all the same.
    while ( !m_functions.empty() )
        closeFunction();
    return m_out;
}

} // namespace

std::string sanitizeCss( const std::string& css, Profile profile, bool external )
{
    return CssSanitizer( css, profile, external ).run();
}

// ---------------------------------------------------------------------
// HTML

// This thread's CPU time (load elsewhere on the machine counts little).
static double threadCpuSeconds()
{
    struct timespec t;
    if ( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &t ) != 0 )
        return 0;   // no clock: no limit
    return double( t.tv_sec ) + double( t.tv_nsec ) * 1e-9;
}

namespace
{

enum ElementAction { Keep, Unwrap, Drop, RenameToPre };

struct Serializer
{
    Serializer( Profile p, bool e, size_t mo, double dl )
        : profile( p ), external( e ), depth( 0 ), maxOut( mo ), deadline( dl ), abandoned( false ), steps( 0 ),
          checkedAt( 0 )
    {
    }

    void open( const GumboNode* n, std::string& close, bool& descend );
    void walk( const GumboNode* root );
    bool overLimits();
    void text( const char* t );
    void attribute( const char* name, const std::string& value );
    ElementAction actionFor( const GumboElement& e, std::string& name ) const;
    bool allowedAttribute( const std::string& element, const std::string& attr ) const;

    Profile profile;
    bool external;
    std::string out;
    unsigned depth;   // elements written and open
    static const unsigned maxDepth = 400;
    // beyond maxOut bytes written, or past the deadline (this thread's CPU
    // time), the walk stops: abandoned
    size_t maxOut;
    double deadline;
    bool abandoned;
    unsigned steps;
    size_t checkedAt;   // the output's size when the clock was last read
};

static bool inList( const std::string& s, const char* const* list )
{
    for ( ; *list; ++list )
        if ( s == *list )
            return true;
    return false;
}

static const char* const keptElements[] = {
    "html", "head", "body", "title", "style", "div", "span", "p", "br", "hr", "h1", "h2", "h3", "h4", "h5", "h6",
    "b", "i", "u", "s", "strike", "em", "strong", "small", "big", "sub", "sup", "tt", "code", "pre", "kbd", "samp",
    "var", "cite", "q", "blockquote", "abbr", "acronym", "address", "del", "ins", "mark", "dfn", "bdi", "bdo", "wbr",
    "font", "center", "ul", "ol", "li", "dl", "dt", "dd", "menu", "dir", "table", "caption", "thead", "tbody", "tfoot",
    "tr", "td", "th", "col", "colgroup", "img", "a", "map", "area", "figure", "figcaption", "section", "article",
    "header", "footer", "nav", "main", "aside", "details", "summary", "picture", "time", "ruby", "rt", "rp", "rb",
    "rtc", "nobr", "hgroup", 0
};

// The element goes, its content stays (as text and kept elements).
static const char* const unwrappedElements[] = {
    "form", "fieldset", "legend", "label", "button", "dialog", "marquee", "blink", "search", "slot", "data",
    "optgroup", 0
};

static const char* const preElements[] = { "xmp", "listing", "plaintext", 0 };

static const char* const voidElements[] = { "br", "hr", "img", "col", "wbr", "area", "link", 0 };

static const char* const globalAttributes[] = {
    "class", "id", "style", "title", "dir", "lang", "align", "valign", "width", "height", "bgcolor", "background",
    "border", "color", "face", "size", "nowrap", "hidden", "role", "translate", 0
};

struct ElementAttributes
{
    const char* element;
    const char* attributes[16];
};

static const ElementAttributes elementAttributes[] = {
    { "table", { "cellpadding", "cellspacing", "frame", "rules", "summary", 0 } },
    { "td", { "colspan", "rowspan", "abbr", "scope", "headers", "axis", "char", "charoff", 0 } },
    { "th", { "colspan", "rowspan", "abbr", "scope", "headers", "axis", "char", "charoff", 0 } },
    { "tr", { "char", "charoff", 0 } },
    { "thead", { "char", "charoff", 0 } },
    { "tbody", { "char", "charoff", 0 } },
    { "tfoot", { "char", "charoff", 0 } },
    { "col", { "span", "char", "charoff", 0 } },
    { "colgroup", { "span", "char", "charoff", 0 } },
    { "ol", { "start", "type", "reversed", "compact", 0 } },
    { "ul", { "type", "compact", 0 } },
    { "li", { "type", "value", 0 } },
    { "dl", { "compact", 0 } },
    { "menu", { "compact", 0 } },
    { "dir", { "compact", 0 } },
    { "img", { "src", "alt", "hspace", "vspace", "usemap", 0 } },
    { "a", { "href", "name", 0 } },
    { "area", { "href", "shape", "coords", "alt", 0 } },
    { "map", { "name", 0 } },
    { "hr", { "noshade", 0 } },
    { "body", { "text", "link", "vlink", "alink", "leftmargin", "topmargin", "rightmargin", "bottommargin",
                "marginwidth", "marginheight", 0 } },
    { "br", { "clear", 0 } },
    { "del", { "datetime", 0 } },
    { "ins", { "datetime", 0 } },
    { "time", { "datetime", 0 } },
    { "details", { "open", 0 } },
    { "pre", { "width", 0 } },
    { "style", { "media", 0 } },
    { 0, { 0 } }
};

ElementAction Serializer::actionFor( const GumboElement& e, std::string& name ) const
{
    if ( e.tag_namespace != GUMBO_NAMESPACE_HTML )
        return Drop;   // <svg>, <math>: scripts, links, references of their own
    if ( e.tag == GUMBO_TAG_UNKNOWN ) {
        name = std::string();
        return Unwrap;   // custom elements: nothing of their own without scripts
    }
    name = gumbo_normalized_tagname( e.tag );
    if ( inList( name, keptElements ) )
        return Keep;
    if ( inList( name, unwrappedElements ) )
        return Unwrap;
    if ( inList( name, preElements ) )
        return RenameToPre;
    if ( name == "iframe" && profile == Host )
        return Keep;   // checked below: frames of the message's HTML parts only
    if ( name == "link" && external )
        return Keep;   // checked below: remote style sheets only
    // Everything else with its content: script, noscript (its content is
    // what a reader without scripts would load, often a tracker), template,
    // frames, object, embed, applet, media, canvas, form fields, meta, base,
    // link, portal, ...
    return Drop;
}

bool Serializer::allowedAttribute( const std::string& element, const std::string& attr ) const
{
    if ( inList( attr, globalAttributes ) || startsWith( attr, "aria-" ) )
        return true;
    for ( const ElementAttributes* e = elementAttributes; e->element; ++e )
        if ( element == e->element )
            return inList( attr, e->attributes );
    return false;
}

static void escapeInto( std::string& out, const char* t, bool attribute )
{
    for ( ; *t; ++t ) {
        switch ( *t ) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"':
            if ( attribute ) {
                out += "&quot;";
                break;
            }
            out += *t;
            break;
        default: out += *t; break;
        }
    }
}

void Serializer::text( const char* t )
{
    escapeInto( out, t, false );
}

void Serializer::attribute( const char* name, const std::string& value )
{
    out += ' ';
    out += name;
    out += "=\"";
    escapeInto( out, value.c_str(), true );
    out += '"';
}

static const GumboAttribute* findAttribute( const GumboElement& e, const char* name )
{
    return gumbo_get_attribute( &e.attributes, name );
}

static bool isBodyFrame( const std::string& src )
{
    // body-<n>.html in the same directory
    if ( !startsWith( src, "body-" ) || src.size() < 11 || src.compare( src.size() - 5, 5, ".html" ) != 0 )
        return false;
    for ( size_t i = 5; i < src.size() - 5; ++i )
        if ( src[i] < '0' || src[i] > '9' )
            return false;
    return src.size() > 10;
}

// An element's start tag (and what it is if it has no content of its
// own); @p close gets its end tag, @p descend whether its children follow.
void Serializer::open( const GumboNode* n, std::string& close, bool& descend )
{
    const GumboElement& e = n->v.element;
    std::string name;
    close = std::string();
    descend = false;
    ElementAction action = actionFor( e, name );
    if ( action == Drop )
        return;
    if ( action == Unwrap || ( action != Drop && depth >= maxDepth ) ) {
        // Deeper than WebKit would build the tree anyway (its parser stops
        // nesting at 512): the content without the element.
        descend = true;
        return;
    }
    if ( action == RenameToPre )
        name = "pre";

    if ( name == "iframe" ) {
        const GumboAttribute* src = findAttribute( e, "src" );
        if ( !src || !isBodyFrame( src->value ) )
            return;
        out += "<iframe";
        attribute( "src", src->value );
        // the host's sandbox, whatever the host wrote
        attribute( "sandbox", "allow-same-origin allow-popups allow-popups-to-escape-sandbox "
                              "allow-top-navigation-by-user-activation" );
        if ( const GumboAttribute* c = findAttribute( e, "class" ) )
            attribute( "class", c->value );
        if ( const GumboAttribute* s = findAttribute( e, "style" ) )
            attribute( "style", sanitizeCss( s->value, profile, external ) );
        out += "></iframe>";
        return;
    }
    if ( name == "link" ) {
        // external: <link rel=stylesheet href=http(s)...>, nothing else
        const GumboAttribute* rel = findAttribute( e, "rel" );
        const GumboAttribute* href = findAttribute( e, "href" );
        if ( !rel || !href || lower( normalizedUrl( rel->value ) ) != "stylesheet" )
            return;
        std::string url = normalizedUrl( href->value ), scheme = schemeOf( url );
        if ( scheme != "http" && scheme != "https" )
            return;
        out += "<link";
        attribute( "rel", "stylesheet" );
        attribute( "href", href->value );
        if ( const GumboAttribute* media = findAttribute( e, "media" ) )
            attribute( "media", media->value );
        out += ">";
        return;
    }

    out += '<';
    out += name;
    for ( unsigned i = 0; i < e.attributes.length; ++i ) {
        const GumboAttribute* a = static_cast<const GumboAttribute*>( e.attributes.data[i] );
        if ( a->attr_namespace != GUMBO_ATTR_NAMESPACE_NONE )
            continue;
        std::string attr = a->name;   // lower case (HTML namespace)
        if ( !allowedAttribute( name, attr ) )
            continue;
        std::string value = a->value;
        if ( attr == "style" ) {
            attribute( "style", sanitizeCss( value, profile, external ) );
            continue;
        }
        if ( attr == "href" ) {
            if ( allowedUrl( value, Link, profile, external ) )
                attribute( "href", value );
            continue;
        }
        if ( attr == "src" || attr == "background" ) {
            if ( allowedUrl( value, Image, profile, external ) )
                attribute( attr.c_str(), value );
            continue;
        }
        if ( attr == "usemap" ) {
            if ( !normalizedUrl( value ).empty() && normalizedUrl( value )[0] == '#' )
                attribute( "usemap", value );
            continue;
        }
        attribute( attr.c_str(), value );
    }
    out += '>';
    if ( inList( name, voidElements ) )
        return;
    if ( name == "pre" )
        out += '\n';   // the parser drops one newline after <pre>
    close = "</" + name + ">";
    if ( name == "style" ) {
        std::string css;
        for ( unsigned i = 0; i < e.children.length; ++i ) {
            const GumboNode* c = static_cast<const GumboNode*>( e.children.data[i] );
            if ( c->type == GUMBO_NODE_TEXT || c->type == GUMBO_NODE_WHITESPACE )
                css += c->v.text.text;
        }
        out += sanitizeCss( css, profile, external );
        return;
    }
    descend = true;
}

// Whether the output has grown beyond maxOut or the deadline has passed:
// the clock is read every 64th node and after every 256 KB written (a
// long style attribute sanitized).
bool Serializer::overLimits()
{
    if ( out.size() > maxOut )
        return true;
    if ( ( ++steps & 63 ) != 0 && out.size() - checkedAt < 256 * 1024 )
        return false;
    checkedAt = out.size();
    return threadCpuSeconds() > deadline;
}

// The tree without recursion: a message may nest elements 100000 deep
// (Gumbo builds that tree; WebKit's parser would flatten it at 512).  The
// stack lives on the heap; elements deeper than maxDepth are unwrapped.
// Over the limits (overLimits()) it stops, abandoned.
void Serializer::walk( const GumboNode* root )
{
    struct Frame
    {
        const GumboNode* node;
        unsigned next;
        std::string close;
        bool counted;
    };
    std::vector<Frame> stack;
    Frame f;
    f.node = root;
    f.next = 0;
    bool descend = false;
    open( root, f.close, descend );
    if ( overLimits() ) {
        abandoned = true;
        return;
    }
    if ( !descend ) {
        out += f.close;
        return;
    }
    f.counted = !f.close.empty();
    depth += f.counted ? 1 : 0;
    stack.push_back( f );
    while ( !stack.empty() ) {
        Frame& top = stack.back();
        const GumboVector& c = top.node->v.element.children;
        if ( top.next >= c.length ) {
            out += top.close;
            if ( top.counted )
                --depth;
            stack.pop_back();
            continue;
        }
        const GumboNode* child = static_cast<const GumboNode*>( c.data[top.next++] );
        if ( child->type == GUMBO_NODE_TEXT || child->type == GUMBO_NODE_WHITESPACE ) {
            text( child->v.text.text );
            if ( overLimits() ) {
                abandoned = true;
                return;
            }
            continue;
        }
        if ( child->type != GUMBO_NODE_ELEMENT )
            continue;   // template (inert), CDATA (foreign content), comments
        Frame g;
        g.node = child;
        g.next = 0;
        open( child, g.close, descend );
        if ( overLimits() ) {
            abandoned = true;
            return;
        }
        if ( !descend ) {
            out += g.close;
            continue;
        }
        g.counted = !g.close.empty();
        depth += g.counted ? 1 : 0;
        stack.push_back( g );   // may move "top": not used after this
    }
}

} // namespace

// ---------------------------------------------------------------------
// What parsing will cost, before Gumbo is asked
//
// Gumbo's work is linear in the input except where it walks its stack of
// open elements or its list of active formatting elements: scope checks
// and implied end tags for every tag (100000 nested <div>: 12 s), and,
// for every character, a search of the stack for the last active
// formatting element (1500 open <div> and a <b>, then 1 MB of text:
// 0.4 s).  Reconstructing formatting elements also adds elements no tag
// opened (1000 open <font> before 1000 paragraphs: a million elements).
//
// ParseCost follows the document as Gumbo will, in one pass, and adds up
// that work: for every tag the depth of the stack plus the length of the
// list; for every character of text the depth of the stack while the
// last entry of the list is an element (an eighth of a tag's: a step of
// that search costs a fraction of a step of a scope check); cloneWork
// for every element reconstructed.  The depth is that of the stack as
// the tree builder keeps it, approximated so that it is never lower than
// Gumbo's:
//  - the tokenizer is the standard's where it decides what is a tag
//    (quoted attribute values, comments, bogus comments, doctypes), so an
//    end tag counts only where Gumbo sees one: "</div>" in an attribute
//    value or in a comment closes nothing;
//  - a start tag opens an element, an end tag closes back to its element
//    only where the tree builder would (in scope: special elements and
//    scope boundaries stop it; </body>, </html> close nothing; </form>
//    removes the form alone); a formatting end tag with a block opened
//    after its element (the adoption agency moves elements) closes
//    nothing;
//  - the end tags implied by p, li, dd, dt, option, h1-h6, button, a,
//    nobr and a table in a table; cells, rows, row groups, captions and
//    column groups only in a table (else ignored), with the tbody and tr
//    a table implies;
//  - formatting elements (a, b, font, ...) as the list keeps them: closed
//    by an end tag not theirs, they come back (are reconstructed) before
//    the next text or inline start tag, three alike at most (only
//    identical start tags are taken to be alike); markers as the list
//    keeps them (a table closing an object leaves its marker);
//  - in svg and math every start tag opens an element ("<.../>" one that
//    closes at once) and nothing implies an end tag; what leaves them (b,
//    div, p, font with color, face or size, ...; </br> and </p>, as in
//    Gumbo 0.14) closes their elements; an end tag closes one of theirs
//    by name, else it is taken as in HTML, where theirs never match;
//  - raw text (script, style, textarea, title, xmp, iframe, noembed,
//    noframes, plaintext) holds no tags.  Where that is not sure (inside
//    svg or math these may be ordinary elements, CDATA there may be a
//    bogus comment, a <script> holding "<!--" and "<script" may not end
//    where it seems to), every start tag in it counts as an element
//    that never closes.
// Counting too much is safe, too little is not: where Gumbo might do
// either, the higher count is taken.  Checked against Gumbo 0.14.1 built
// to count its steps over the stack and the list, on the hostile shapes
// of tests/render-only/sanitize-timing.py and 35000 random tag soups
// repeated 300-2000 times: none grew faster than the estimate.
//
// Above maxParseWork plus maxParseWorkPerByte per byte of input the
// document is not parsed: it is shown as its source text.  The size term
// keeps large ordinary documents parsed (KMail's HTML of a 7.5 MB log,
// 100000 lines, costs 6e5); what it lets a hostile one buy costs less
// than parsing it at all (at most 4 ns per unit measured: 20 ms, then
// 30 ns per byte, where Gumbo takes 60-200 ns per byte).  The corpus's
// newsletters (35-45 KB) cost 3e3-4e3.
//
// A clone also copies the attributes of its start tag, and the tree
// holds every copy (written out again for each): a few characters of
// text after a long start tag closed early cost its length again.  Above
// maxCloneBytes plus maxCloneBytesPerByte per byte of input in the start
// tags cloned the document is not parsed either.  What elements and
// clones allocate, the allocation budget bounds (sanitizeHtml()).
static const double maxParseWork = 5e6;
static const double maxParseWorkPerByte = 8;
static const double maxCloneBytes = 4 * 1024 * 1024;
static const double maxCloneBytesPerByte = 2;
// An element no tag opened (a clone of a formatting element): about what
// Gumbo spends on an element, allocating it and its attributes, in steps
// of a scope check.
static const double cloneWork = 256;

namespace
{

enum ElementFlag {
    Void = 1 << 0,            // no end tag, never on the stack
    NotOnStack = 1 << 1,      // html, head, body: there once, at the bottom
    Formatting = 1 << 2,      // in the list of active formatting elements
    Marker = 1 << 3,          // puts a marker in that list
    Special = 1 << 4,         // the standard's special category
    Scope = 1 << 5,           // a boundary of every scope
    ListScope = 1 << 6,       // ... of the list item scope (ol, ul)
    ButtonScope = 1 << 7,     // ... of the button scope
    TableScope = 1 << 8,      // ... of the table scope (html, table, template)
    ClosesP = 1 << 9,         // its start tag closes an open p
    NoReconstruct = 1 << 10,  // its start tag does not reconstruct the list
    Raw = 1 << 11,            // raw text or RCDATA follows its start tag
    Heading = 1 << 12,        // h1-h6
    BlockEnd = 1 << 13,       // its end tag closes it when in scope
    Cell = 1 << 14,           // td, th
    RowGroup = 1 << 15,       // tbody, thead, tfoot
    Row = 1 << 16,            // tr
    TableElement = 1 << 17,   // table
    TablePart = 1 << 18,      // inserted only in a table: caption, colgroup, col, row groups, rows, cells
    TableEnd = 1 << 19,       // its end tag closes it when in table scope
    ClosesTable = 1 << 20,    // a <table> here closes the table: table, row groups, tr
    Foreign = 1 << 21,        // svg, math
    Breakout = 1 << 22,       // its start tag leaves svg and math (font: with color, face or size)
    IntegrationPoint = 1 << 23,  // (on the stack only) of svg or math, holds HTML
    InForeign = 1 << 24,      // (on the stack only) an element of svg or math
    InSvg = 1 << 27,          // (on the stack only) ... of svg
    MarkerOnPop = 1 << 25,    // the list loses its marker whenever it is closed (td, th, caption, template)
    Fake = 1 << 26            // (on the stack only) a start tag maybe not one
};

// Every boundary at once: what a start tag seen in raw text is taken to be.
static const unsigned FakeFlags = Special | Scope | ListScope | ButtonScope | TableScope | Fake;

typedef std::unordered_map<std::string, unsigned> FlagTable;

static FlagTable flagTable()
{
    FlagTable table;
    struct Group { unsigned flags; const char* names; };
    static const Group groups[] = {
        { Void, "area base br col embed hr img input link meta param source track wbr keygen basefont bgsound "
                "frame image" },
        { NotOnStack, "html head body" },
        { Formatting, "a b big code em font i nobr s small strike strong tt u" },
        { Marker, "applet object marquee template td th caption" },
        { MarkerOnPop, "template td th caption" },
        { Special, "address applet area article aside base basefont bgsound blockquote body br button caption "
                   "center col colgroup dd details dir div dl dt embed fieldset figcaption figure footer form "
                   "frame frameset h1 h2 h3 h4 h5 h6 head header hgroup hr html iframe img input keygen li link "
                   "listing main marquee menu meta nav noembed noframes noscript object ol p param plaintext pre "
                   "script search section select source style summary table tbody td template textarea tfoot th "
                   "thead title tr track ul wbr xmp mi mo mn ms mtext annotation-xml foreignobject desc" },
        // select: a boundary too (an older Gumbo ignores end tags inside it)
        { Scope, "applet caption html table td th marquee object template mi mo mn ms mtext annotation-xml "
                 "foreignobject desc title select" },
        { ListScope, "ol ul" },
        { ButtonScope, "button" },
        { TableScope, "html table template" },
        { ClosesP, "address article aside blockquote center details dialog dir div dl fieldset figcaption figure "
                   "footer header hgroup main menu nav ol p search section summary ul h1 h2 h3 h4 h5 h6 pre "
                   "listing form plaintext hr xmp li dd dt" },
        { NoReconstruct, "address article aside blockquote center details dialog dir div dl fieldset figcaption "
                         "figure footer header hgroup main menu nav ol p search section summary ul h1 h2 h3 h4 "
                         "h5 h6 pre listing form li dd dt plaintext table hr caption col colgroup tbody td tfoot "
                         "th thead tr base basefont bgsound link meta noframes script style template title "
                         "textarea iframe noembed param source track frame frameset rb rp rt rtc html head "
                         "body" },
        { Raw, "script style xmp iframe noembed noframes textarea title plaintext" },
        { Heading, "h1 h2 h3 h4 h5 h6" },
        { BlockEnd, "address article aside blockquote button center details dialog dir div dl fieldset "
                    "figcaption figure footer header hgroup listing main menu nav ol pre search section summary "
                    "ul applet marquee object template dd dt" },
        { Cell, "td th" },
        { RowGroup, "tbody thead tfoot" },
        { Row, "tr" },
        { TableElement, "table" },
        { TablePart, "caption colgroup col tbody thead tfoot tr td th" },
        { TableEnd, "caption table tbody thead tfoot tr td th" },
        { ClosesTable, "table tbody thead tfoot tr" },
        { Foreign, "svg math" },
        { Breakout, "b big blockquote body br center code dd div dl dt em embed h1 h2 h3 h4 h5 h6 head hr i img li "
                    "listing menu meta nobr ol p pre ruby s small span strong strike sub sup table tt u ul var font" },
    };
    for ( size_t g = 0; g < sizeof groups / sizeof *groups; ++g ) {
        const char* p = groups[g].names;
        while ( *p ) {
            const char* e = strchr( p, ' ' );
            if ( !e )
                e = p + strlen( p );
            table[std::string( p, e )] |= groups[g].flags;
            p = *e ? e + 1 : e;
        }
    }
    return table;
}

static unsigned elementFlags( const std::string& name )
{
    static const FlagTable table = flagTable();
    FlagTable::const_iterator it = table.find( name );
    return it == table.end() ? 0 : it->second;
}

static bool isSpace( char c )
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static bool isAlpha( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' );
}

class ParseCost
{
public:
    ParseCost( const std::string& html, double limit, double cloneLimit )
        : m_html( html ), m_limit( limit ), m_cloneLimit( cloneLimit ), m_work( 0 ), m_textWork( 0 ),
          m_elements( 0 ), m_attributes( 0 ), m_clones( 0 ), m_cloneBytes( 0 ), m_serial( 0 ),
          m_foreign( 0 ), m_fakeFormatting( false ), m_frameset( false ), m_commentEnd( "-->" ), m_commentBangEnd( "--!>" ),
          m_cdataEnd( "]]>" ), m_scriptComment( "<!--" )
    {
    }

    // false: the work, or what clones copy, exceeds its limit (the pass
    // stops there)
    bool run();
    double work() const { return m_work + m_textWork / 8; }
    // elements start tags make (not clones), their attributes and those
    // of the clones
    size_t elements() const { return m_elements; }
    size_t attributes() const { return m_attributes; }
    // elements no tag opened (reconstructed formatting elements) and the
    // length of the start tags they copy
    size_t clones() const { return m_clones; }
    size_t cloneBytes() const { return m_cloneBytes; }

private:
    struct Element
    {
        std::string name;
        unsigned flags;
        unsigned long serial;
    };
    struct Entry   // of the list of active formatting elements
    {
        std::string name;
        std::string tag;       // the start tag as written (for the Noah's Ark clause)
        size_t hash;           // of tag (most compare unequal at once, however long)
        size_t attributes;     // in it
        unsigned long serial;  // its element; 0: a marker
        bool open;             // its element is on the stack
    };

    bool over() const { return work() > m_limit || double( m_cloneBytes ) > m_cloneLimit; }
    // a character makes Gumbo search the stack: the list ends with an element
    bool searchesStack() const { return m_fakeFormatting || ( !m_list.empty() && m_list.back().serial != 0 ); }
    void charge() { m_work += double( m_stack.size() + m_list.size() ); }
    // Gumbo compares each attribute's name with those before it in the
    // tag (to drop a duplicate): a tag with a attributes, a(a-1)/2 steps
    void chargeAttributes( size_t a ) { m_work += double( a ) * double( a ? a - 1 : 0 ) / 2; }
    void text( size_t length );
    size_t tagEnd( size_t pos, bool* fontAttributes = 0, bool* selfClosing = 0, size_t* attributes = 0 ) const;
    size_t rawEnd( size_t pos, const std::string& name, size_t& endTag, bool& unsure );
    std::string tagName( size_t& pos ) const;
    void startTag( const std::string& name, size_t begin, size_t end, bool fontAttributes, bool selfClosing,
                   size_t attributes );
    void ignored( size_t attributes );
    void pushForeign( const std::string& name, unsigned flags, bool selfClosing );
    void fakeStartTag( const std::string& name );
    void endTag( const std::string& name );
    void endFormatting( const std::string& name );
    long find( const std::string* name, unsigned anyOf, unsigned boundary ) const;
    long tableContext( unsigned anyOf ) const;
    bool topIs( unsigned anyOf, const std::string* name = 0 ) const;
    bool inForeignContent() const;
    void fakeStartTags( size_t from, size_t to );
    void push( const std::string& name, unsigned flags );
    void popTo( size_t index );
    void pop();
    void clearToMarker();
    void addFormatting( const std::string& name, const std::string& tag, size_t attributes );
    void reconstruct();
    long lastFormatting( const std::string& name ) const;

    // std::string::find() for a position that only grows: the last answer
    // is kept (searching anew each time from each comment for a "--!>"
    // that is not there would be quadratic)
    struct Finder
    {
        explicit Finder( const char* n ) : needle( n ), from( 0 ), at( std::string::npos ), valid( false ) {}
        size_t find( const std::string& s, size_t pos )
        {
            if ( !valid || pos < from || ( at != std::string::npos && at < pos ) ) {
                at = s.find( needle, pos );
                from = pos;
                valid = true;
            }
            return at;
        }
        const char* needle;
        size_t from, at;
        bool valid;
    };

    const std::string& m_html;
    double m_limit, m_cloneLimit;
    double m_work, m_textWork;
    size_t m_elements, m_attributes, m_clones, m_cloneBytes;
    unsigned long m_serial;
    unsigned m_foreign;   // svg and math on the stack
    bool m_fakeFormatting;
    bool m_frameset;   // a <frameset> seen
    std::vector<Element> m_stack;
    std::vector<Entry> m_list;
    Finder m_commentEnd, m_commentBangEnd, m_cdataEnd, m_scriptComment;
};

void ParseCost::push( const std::string& name, unsigned flags )
{
    Element e;
    e.name = name;
    e.flags = flags;
    e.serial = ++m_serial;
    m_stack.push_back( e );
    if ( flags & Foreign )
        ++m_foreign;
}

void ParseCost::pop()
{
    const Element& e = m_stack.back();
    if ( e.flags & Foreign )
        --m_foreign;
    if ( ( e.flags & ( Formatting | Fake ) ) == Formatting ) {
        m_work += double( m_list.size() );
        for ( size_t i = m_list.size(); i-- > 0; )
            if ( m_list[i].serial == e.serial ) {
                m_list[i].open = false;
                break;
            }
    }
    if ( ( e.flags & ( MarkerOnPop | Fake ) ) == MarkerOnPop )
        clearToMarker();
    m_stack.pop_back();
}

// The list up to its last marker.
void ParseCost::clearToMarker()
{
    while ( !m_list.empty() ) {
        bool marker = m_list.back().serial == 0;
        m_list.pop_back();
        if ( marker )
            break;
    }
}

void ParseCost::popTo( size_t index )
{
    while ( m_stack.size() > index )
        pop();
}

// The topmost HTML element (not a fake) named @p name or with one of the
// flags @p anyOf, if no element with one of the flags @p boundary is
// above it; -1: none.
long ParseCost::find( const std::string* name, unsigned anyOf, unsigned boundary ) const
{
    for ( size_t i = m_stack.size(); i-- > 0; ) {
        const Element& e = m_stack[i];
        if ( !( e.flags & ( Fake | InForeign ) ) && ( ( name && e.name == *name ) || ( e.flags & anyOf ) ) )
            return long( i );
        if ( e.flags & boundary )
            return -1;
    }
    return -1;
}

// The topmost HTML element with one of the flags @p anyOf or a template,
// whatever is above it; -1: none.
long ParseCost::tableContext( unsigned anyOf ) const
{
    static const std::string templ( "template" );
    for ( size_t i = m_stack.size(); i-- > 0; ) {
        const Element& e = m_stack[i];
        if ( !( e.flags & ( Fake | InForeign ) ) && ( ( e.flags & anyOf ) || e.name == templ ) )
            return long( i );
    }
    return -1;
}

// Whether the current node is an HTML element (not a fake) with one of
// the flags @p anyOf or named @p name.
bool ParseCost::topIs( unsigned anyOf, const std::string* name ) const
{
    if ( m_stack.empty() || ( m_stack.back().flags & ( Fake | InForeign ) ) )
        return false;
    return ( m_stack.back().flags & anyOf ) || ( name && m_stack.back().name == *name );
}

// Whether tags are svg's or math's: the current node is an element of
// theirs that does not hold HTML (or maybe not a tag at all).
bool ParseCost::inForeignContent() const
{
    if ( m_stack.empty() )
        return false;
    unsigned flags = m_stack.back().flags;
    return ( flags & Fake ) || ( ( flags & InForeign ) && !( flags & IntegrationPoint ) );
}

long ParseCost::lastFormatting( const std::string& name ) const
{
    for ( size_t i = m_list.size(); i-- > 0; ) {
        if ( m_list[i].serial == 0 )
            return -1;
        if ( m_list[i].name == name )
            return long( i );
    }
    return -1;
}

void ParseCost::addFormatting( const std::string& name, const std::string& tag, size_t attributes )
{
    // Noah's Ark: three alike after the last marker at most
    size_t alike = 0, earliest = 0;
    size_t hash = std::hash<std::string>()( tag );
    for ( size_t i = m_list.size(); i-- > 0 && m_list[i].serial != 0; )
        if ( m_list[i].hash == hash && m_list[i].tag == tag ) {
            ++alike;
            earliest = i;
        }
    if ( alike >= 3 )
        m_list.erase( m_list.begin() + long( earliest ) );
    Entry f;
    f.name = name;
    f.tag = tag;
    f.hash = hash;
    f.attributes = attributes;
    f.serial = m_stack.back().serial;
    f.open = true;
    m_list.push_back( f );
}

void ParseCost::reconstruct()
{
    if ( m_list.empty() || m_list.back().serial == 0 || m_list.back().open )
        return;
    size_t i = m_list.size() - 1;
    while ( i > 0 && m_list[i - 1].serial != 0 && !m_list[i - 1].open )
        --i;
    // a search of the stack for each entry looked at, then the clones:
    // elements no tag opened, each with a copy of the attributes
    m_work += double( m_list.size() - i + 1 ) * double( m_stack.size() ) + cloneWork * double( m_list.size() - i );
    for ( ; i < m_list.size(); ++i ) {
        ++m_clones;
        m_cloneBytes += m_list[i].tag.size();
        m_attributes += m_list[i].attributes;
        push( m_list[i].name, elementFlags( m_list[i].name ) );
        m_list[i].serial = m_stack.back().serial;
        m_list[i].open = true;
    }
}

void ParseCost::text( size_t length )
{
    if ( !length )
        return;
    reconstruct();
    if ( searchesStack() )
        m_textWork += double( length ) * double( m_stack.size() );
}

// The adoption agency, as far as the depth goes.
void ParseCost::endFormatting( const std::string& name )
{
    long f = lastFormatting( name );
    if ( f < 0 ) {
        // as any other end tag
        long i = find( &name, 0, Special );
        if ( i >= 0 )
            popTo( size_t( i ) );
        return;
    }
    if ( !m_list[f].open ) {
        m_list.erase( m_list.begin() + f );
        return;
    }
    size_t s = m_stack.size();
    while ( s-- > 0 && m_stack[s].serial != m_list[f].serial )
        ;
    for ( size_t i = s + 1; i < m_stack.size(); ++i ) {
        if ( m_stack[i].flags & Scope )
            return;   // not in scope: ignored
    }
    for ( size_t i = s + 1; i < m_stack.size(); ++i ) {
        if ( m_stack[i].flags & Special ) {
            // a furthest block: elements move, up to eight rounds; the
            // depth is taken to stay.  The formatting element is cloned
            // (the clone takes its place).
            m_work += 8 * double( m_stack.size() + m_list.size() );
            ++m_clones;
            m_cloneBytes += m_list[f].tag.size();
            m_attributes += m_list[f].attributes;
            return;
        }
    }
    m_list.erase( m_list.begin() + f );
    popTo( s );
}

void ParseCost::fakeStartTag( const std::string& name )
{
    charge();
    ++m_elements;   // maybe one
    unsigned flags = elementFlags( name );
    if ( flags & Void )
        return;
    reconstruct();
    push( name, FakeFlags | ( flags & Foreign ) );
    if ( flags & Formatting )
        m_fakeFormatting = true;
}

// Every start tag in [@p from, @p to) as a fake one.
// Their attributes are charged as a real tag's (Gumbo's search for
// duplicates), except for those starting within a tag already measured
// (each byte is scanned for that once at most).
void ParseCost::fakeStartTags( size_t from, size_t to )
{
    size_t measured = from;
    for ( size_t s = m_html.find( '<', from ); s < to && !over(); s = m_html.find( '<', s + 1 ) ) {
        if ( s + 1 < to && isAlpha( m_html[s + 1] ) ) {
            size_t q = s + 1;
            std::string name = tagName( q );
            if ( q >= measured ) {
                size_t attributes = 0;
                measured = tagEnd( q, 0, 0, &attributes );
                chargeAttributes( attributes );
            }
            fakeStartTag( name );
        }
    }
}

// An element of svg or math: of the namespace of the current node, or of
// its own (svg, math); those that hold HTML are special and scope
// boundaries.  Written "<.../>", it closes at once.
void ParseCost::pushForeign( const std::string& name, unsigned flags, bool selfClosing )
{
    if ( selfClosing )
        return;
    bool svg = name == "svg" || ( name != "math" && !m_stack.empty() && ( m_stack.back().flags & InSvg ) );
    unsigned f = ( flags & Foreign ) | InForeign | ( svg ? InSvg : 0 );
    if ( svg ? ( name == "foreignobject" || name == "desc" || name == "title" )
             : ( name == "mi" || name == "mo" || name == "mn" || name == "ms" || name == "mtext" ) )
        f |= IntegrationPoint | Special | Scope;
    push( name, f );
}

// A start tag that makes no element after all (with @p attributes).
void ParseCost::ignored( size_t attributes )
{
    --m_elements;
    m_attributes -= attributes;
}

// The start tag @p name written at [@p begin, @p end); @p fontAttributes:
// it has an attribute color, face or size; @p selfClosing: "/>";
// @p attributes: how many it has.
void ParseCost::startTag( const std::string& name, size_t begin, size_t end, bool fontAttributes,
                          bool selfClosing, size_t attributes )
{
    charge();
    ++m_elements;
    m_attributes += attributes;
    unsigned flags = elementFlags( name );
    static const std::string font( "font" );
    if ( inForeignContent() ) {
        if ( !( flags & Breakout ) || ( name == font && !fontAttributes ) ) {
            // an element of svg or math, whatever its name: no implied
            // end tags, no formatting elements (nor, for a fake current
            // node, the HTML that it may be: the list, but nothing closes)
            bool fake = m_stack.back().flags & Fake;
            if ( fake ) {
                reconstruct();
                if ( flags & Formatting )
                    m_fakeFormatting = true;
            }
            pushForeign( name, flags, selfClosing && !fake );
            return;
        }
        // an HTML element: the elements of svg and math above close
        if ( !( m_stack.back().flags & Fake ) )
            while ( !m_stack.empty() && ( m_stack.back().flags & ( InForeign | IntegrationPoint ) ) == InForeign )
                pop();
    }
    if ( name == "frameset" )
        m_frameset = true;
    if ( flags & NotOnStack ) {
        ignored( attributes );   // html, head, body: theirs, if new, go to the one there
        return;
    }
    static const std::string a( "a" ), nobr( "nobr" ), li( "li" ), dd( "dd" ), dt( "dt" ), p( "p" ),
        button( "button" ), option( "option" ), optgroup( "optgroup" ), select( "select" ), tr( "tr" ),
        table( "table" ), col( "col" ), colgroup( "colgroup" ), tbody( "tbody" );
    long i;
    if ( name == a ) {
        long f = lastFormatting( a );
        if ( f >= 0 ) {
            // closed as by </a>; if that left it in the list, it leaves the
            // list (the element stays open)
            unsigned long serial = m_list[f].serial;
            endFormatting( a );
            for ( size_t k = m_list.size(); k-- > 0; )
                if ( m_list[k].serial == serial ) {
                    m_list.erase( m_list.begin() + long( k ) );
                    break;
                }
        }
    } else if ( name == nobr ) {
        if ( find( &nobr, 0, Scope ) >= 0 )
            endFormatting( nobr );
    } else if ( name == li || name == dd || name == dt ) {
        for ( size_t k = m_stack.size(); k-- > 0; ) {
            const Element& e = m_stack[k];
            if ( !( e.flags & ( Fake | InForeign ) ) &&
                 ( name == li ? e.name == li : ( e.name == dd || e.name == dt ) ) ) {
                popTo( k );
                break;
            }
            if ( ( e.flags & Special ) && e.name != "address" && e.name != "div" && e.name != "p" )
                break;
        }
    } else if ( name == button ) {
        if ( ( i = find( &button, 0, Scope ) ) >= 0 )
            popTo( size_t( i ) );
    } else if ( name == select ) {
        if ( ( i = find( &select, 0, Scope ) ) >= 0 ) {
            popTo( size_t( i ) );
            ignored( attributes );
            return;
        }
    } else if ( name == option || name == optgroup ) {
        if ( topIs( 0, &option ) )
            pop();
    } else if ( flags & TablePart ) {
        // the cell, row or row group open closes, then the stack back to
        // the table, row group or row it goes in; outside a table (or a
        // template) the tag is ignored
        unsigned context = TableElement;
        if ( flags & Cell ) {
            i = find( 0, Cell, TableScope );
            context |= Row | RowGroup;
        } else if ( flags & Row ) {
            i = find( 0, Row, TableScope );
            context |= RowGroup;
        } else if ( flags & RowGroup )
            i = find( 0, RowGroup, TableScope );
        else
            i = -1;
        if ( i >= 0 )
            popTo( size_t( i ) );
        long c = tableContext( context );
        if ( c < 0 ) {
            ignored( attributes );
            return;
        }
        popTo( size_t( c ) + 1 );
        // the row group and row a table implies, the colgroup of a col
        if ( ( flags & ( Cell | Row ) ) && topIs( TableElement ) ) {
            push( tbody, elementFlags( tbody ) );
            ++m_elements;
        }
        if ( ( flags & Cell ) && topIs( RowGroup ) ) {
            push( tr, elementFlags( tr ) );
            ++m_elements;
        }
        if ( name == col && !topIs( 0, &colgroup ) ) {
            push( colgroup, elementFlags( colgroup ) );
            ++m_elements;
        }
    } else if ( name == table ) {
        if ( topIs( ClosesTable ) && ( i = find( &table, 0, TableScope ) ) >= 0 )
            popTo( size_t( i ) );
    }
    if ( flags & ClosesP ) {
        if ( ( i = find( &p, 0, Scope | ButtonScope ) ) >= 0 )
            popTo( size_t( i ) );
        if ( ( flags & Heading ) && topIs( Heading ) )
            pop();
    }
    if ( !( flags & NoReconstruct ) )
        reconstruct();
    if ( flags & Void )
        return;
    if ( flags & Foreign ) {
        pushForeign( name, flags, selfClosing );
        return;
    }
    push( name, flags );
    if ( flags & Formatting )
        addFormatting( name, m_html.substr( begin, end - begin ), attributes );
    if ( flags & Marker ) {
        Entry m;
        m.attributes = 0;
        m.hash = 0;
        m.serial = 0;
        m.open = false;
        m_list.push_back( m );
    }
}

void ParseCost::endTag( const std::string& name )
{
    charge();
    unsigned flags = elementFlags( name );
    static const std::string p( "p" ), li( "li" ), br( "br" ), form( "form" ), colgroup( "colgroup" );
    long i = -1;
    if ( !m_stack.empty() && ( m_stack.back().flags & Fake ) )
        return;   // maybe no element: nothing closes
    if ( ( name == br || name == p ) && inForeignContent() ) {
        // leaves svg and math (as Gumbo 0.14 does): their elements above
        // close, then as in HTML
        while ( !m_stack.empty() && ( m_stack.back().flags & ( InForeign | IntegrationPoint ) ) == InForeign )
            pop();
    } else {
        // svg's or math's: closes the element of that name above the
        // first HTML element, else as in HTML
        for ( size_t k = m_stack.size(); k-- > 0 && ( m_stack[k].flags & InForeign ); ) {
            if ( m_stack[k].name == name ) {
                popTo( k );
                return;
            }
        }
    }
    if ( flags & NotOnStack )
        return;   // </body>, </html>: nothing closes
    if ( name == br ) {
        // as <br> (void: only the list is reconstructed)
        reconstruct();
        return;
    }
    if ( flags & Formatting ) {
        endFormatting( name );
        return;
    }
    if ( name == p )
        i = find( &p, 0, Scope | ButtonScope );
    else if ( name == li )
        i = find( &li, 0, Scope | ListScope );
    else if ( name == form ) {
        // the form element alone leaves the stack
        if ( ( i = find( &form, 0, Scope ) ) >= 0 )
            m_stack.erase( m_stack.begin() + i );
        return;
    } else if ( name == colgroup ) {
        if ( topIs( 0, &colgroup ) )
            pop();
        return;
    } else if ( flags & Heading )
        i = find( 0, Heading, Scope );
    else if ( flags & TableEnd )
        i = find( &name, 0, TableScope );
    else if ( flags & BlockEnd )
        i = find( &name, 0, Scope );
    else
        i = find( &name, 0, Special );   // any other end tag
    if ( i >= 0 ) {
        popTo( size_t( i ) );
        // applet, marquee, object: their marker leaves the list only
        // with their own end tag (not when a table closes them)
        if ( ( flags & ( Marker | MarkerOnPop ) ) == Marker )
            clearToMarker();
    }
}

// The name of the tag whose name starts at @p pos, in lower case; @p pos
// ends after it.
std::string ParseCost::tagName( size_t& pos ) const
{
    size_t n = m_html.size(), b = pos;
    while ( pos < n && !isSpace( m_html[pos] ) && m_html[pos] != '/' && m_html[pos] != '>' )
        ++pos;
    return lower( m_html.substr( b, pos - b ) );
}

// After the tag's '>', from just after its name (the standard's states
// from "tag name" on); npos: the input ends in the tag.  @p fontAttributes:
// whether it has an attribute color, face or size; @p selfClosing: whether
// it ends with "/>"; @p attributes: how many it has (duplicates too).
size_t ParseCost::tagEnd( size_t pos, bool* fontAttributes, bool* selfClosing, size_t* attributes ) const
{
    enum State { BeforeName, Name, AfterName, BeforeValue, Quoted, Unquoted, AfterQuoted, SelfClosing };
    State state = BeforeName;
    char quote = 0;
    size_t n = m_html.size(), name = 0;
    if ( fontAttributes )
        *fontAttributes = false;
    if ( selfClosing )
        *selfClosing = false;
    if ( attributes )
        *attributes = 0;
    if ( pos < n && m_html[pos] == '/' ) {
        state = SelfClosing;
        ++pos;
    }
    for ( ; pos < n; ++pos ) {
        char c = m_html[pos];
        if ( state == Name && fontAttributes && ( isSpace( c ) || c == '/' || c == '=' || c == '>' ) ) {
            std::string a = lower( m_html.substr( name, pos - name ) );
            if ( a == "color" || a == "face" || a == "size" )
                *fontAttributes = true;
        }
        switch ( state ) {
        case BeforeName:
            if ( isSpace( c ) )
                break;
            if ( c == '/' )
                state = SelfClosing;
            else if ( c == '>' )
                return pos + 1;
            else {
                state = Name;   // '=' too: the first character of the name
                name = pos;
                if ( attributes )
                    ++*attributes;
            }
            break;
        case Name:
            if ( isSpace( c ) )
                state = AfterName;
            else if ( c == '/' )
                state = SelfClosing;
            else if ( c == '=' )
                state = BeforeValue;
            else if ( c == '>' )
                return pos + 1;
            break;
        case AfterName:
            if ( isSpace( c ) )
                break;
            if ( c == '/' )
                state = SelfClosing;
            else if ( c == '=' )
                state = BeforeValue;
            else if ( c == '>' )
                return pos + 1;
            else {
                state = Name;
                name = pos;
                if ( attributes )
                    ++*attributes;
            }
            break;
        case BeforeValue:
            if ( isSpace( c ) )
                break;
            if ( c == '"' || c == '\'' ) {
                quote = c;
                state = Quoted;
            } else if ( c == '>' )
                return pos + 1;
            else
                state = Unquoted;
            break;
        case Quoted:
            if ( c == quote )
                state = AfterQuoted;
            break;
        case Unquoted:
            if ( isSpace( c ) )
                state = BeforeName;
            else if ( c == '>' )
                return pos + 1;
            break;
        case AfterQuoted:
            if ( isSpace( c ) )
                state = BeforeName;
            else if ( c == '/' )
                state = SelfClosing;
            else if ( c == '>' )
                return pos + 1;
            else {
                state = BeforeName;
                --pos;   // reconsumed
            }
            break;
        case SelfClosing:
            if ( c == '>' ) {
                if ( selfClosing )
                    *selfClosing = true;
                return pos + 1;
            }
            state = BeforeName;
            --pos;   // reconsumed
            break;
        }
    }
    return std::string::npos;
}

// Where the raw text of element @p name starting at @p pos ends: at the
// '<' of its end tag (@p endTag after that tag's name), npos: it holds
// the rest of the input (@p unsure: or maybe not).
size_t ParseCost::rawEnd( size_t pos, const std::string& name, size_t& endTag, bool& unsure )
{
    unsure = false;
    if ( name == "plaintext" )
        return std::string::npos;
    size_t n = m_html.size(), len = name.size();
    for ( size_t i = m_html.find( "</", pos ); i != std::string::npos; i = m_html.find( "</", i + 1 ) ) {
        if ( i + 2 + len >= n )
            return std::string::npos;
        if ( lower( m_html.substr( i + 2, len ) ) != name )
            continue;
        char c = m_html[i + 2 + len];
        if ( !isSpace( c ) && c != '/' && c != '>' )
            continue;
        if ( name == "script" ) {
            // "<!--" then "<script": maybe double escaped, this end tag
            // maybe text
            size_t comment = m_scriptComment.find( m_html, pos );
            if ( comment < i ) {
                for ( size_t s = m_html.find( '<', comment + 4 ); s < i; s = m_html.find( '<', s + 1 ) )
                    if ( lower( m_html.substr( s + 1, 6 ) ) == "script" ) {
                        unsure = true;
                        return std::string::npos;
                    }
            }
        }
        endTag = i + 2 + len;
        return i;
    }
    return std::string::npos;
}

bool ParseCost::run()
{
    const std::string& h = m_html;
    size_t n = h.size(), i = 0;
    while ( i < n ) {
        size_t lt = h.find( '<', i );
        if ( lt == std::string::npos ) {
            text( n - i );
            break;
        }
        text( lt - i );
        i = lt + 1;
        if ( i >= n ) {
            text( 1 );
            break;
        }
        char c = h[i];
        if ( isAlpha( c ) ) {
            size_t pos = i;
            std::string name = tagName( pos );
            bool fontAttributes = false, selfClosing = false;
            size_t attributes = 0;
            size_t end = tagEnd( pos, name == "font" ? &fontAttributes : 0, &selfClosing, &attributes );
            if ( end == std::string::npos )
                break;   // the input ends in the tag: it is dropped
            chargeAttributes( attributes );
            startTag( name, lt, end, fontAttributes, selfClosing, attributes );
            i = end;
            if ( ( elementFlags( name ) & Raw ) && !m_stack.empty() && m_stack.back().name == name ) {
                size_t element = m_stack.size() - 1;
                size_t endTagName = 0;
                bool unsure;
                size_t rawStop = rawEnd( i, name, endTagName, unsure );
                // after a <frameset> Gumbo may ignore the raw text element
                if ( m_frameset )
                    unsure = true;
                size_t stop = rawStop == std::string::npos ? n : rawStop;
                if ( m_foreign || unsure ) {
                    // maybe an ordinary element, maybe not the end: its
                    // start tags count
                    fakeStartTags( i, stop );
                    if ( over() )
                        return false;
                }
                if ( searchesStack() )
                    m_textWork += double( stop - i ) * double( m_stack.size() );
                if ( rawStop == std::string::npos )
                    break;
                charge();
                if ( m_foreign )
                    m_stack.erase( m_stack.begin() + long( element ) );   // the start tags in it stay
                else
                    popTo( element );
                size_t attributes = 0;
                end = tagEnd( endTagName, 0, 0, &attributes );
                if ( end == std::string::npos )
                    break;
                chargeAttributes( attributes );
                i = end;
            }
        } else if ( c == '/' ) {
            if ( i + 1 >= n ) {
                text( 2 );
                break;
            }
            if ( isAlpha( h[i + 1] ) ) {
                size_t pos = i + 1;
                std::string name = tagName( pos );
                size_t attributes = 0;
                size_t end = tagEnd( pos, 0, 0, &attributes );
                if ( end == std::string::npos )
                    break;
                chargeAttributes( attributes );
                endTag( name );
                i = end;
            } else if ( h[i + 1] == '>' )
                i += 2;   // "</>": nothing
            else {
                size_t gt = h.find( '>', i );   // a bogus comment
                if ( gt == std::string::npos )
                    break;
                i = gt + 1;
            }
        } else if ( c == '!' ) {
            if ( h.compare( i, 3, "!--" ) == 0 ) {
                size_t b = i + 3, e;
                if ( b < n && h[b] == '>' )
                    e = b + 1;   // "<!-->"
                else if ( b + 1 < n && h[b] == '-' && h[b + 1] == '>' )
                    e = b + 2;   // "<!--->"
                else {
                    size_t d = m_commentEnd.find( h, b ), x = m_commentBangEnd.find( h, b );
                    if ( d == std::string::npos && x == std::string::npos )
                        break;
                    e = d < x ? d + 3 : x + 4;
                }
                i = e;
            } else if ( h.compare( i, 8, "![CDATA[" ) == 0 && m_foreign ) {
                // CDATA in svg or math, else a bogus comment: either way
                // ends at its "]]>", the start tags up to there count
                size_t e = m_cdataEnd.find( h, i );
                fakeStartTags( i, e == std::string::npos ? n : e );
                if ( e == std::string::npos )
                    break;
                i = e + 3;
            } else {
                // a doctype or a bogus comment: ends at the first '>'
                size_t e = h.find( '>', i );
                if ( e == std::string::npos )
                    break;
                i = e + 1;
            }
        } else if ( c == '?' ) {
            size_t gt = h.find( '>', i );
            if ( gt == std::string::npos )
                break;
            i = gt + 1;
        } else
            text( 1 );   // the '<'
        if ( over() )
            return false;
    }
    return !over();
}

} // namespace

// The source text in a <pre>: every character as text, NUL as U+FFFD;
// what would make it longer than 4 MB plus twice the source's length (up
// to 5 times: "&amp;") is left out, with a note.
static void escapeSource( std::string& out, const std::string& text )
{
    size_t limit = out.size() + 4 * 1024 * 1024 + 2 * text.size();
    for ( size_t i = 0; i < text.size(); ++i ) {
        if ( out.size() >= limit ) {
            // not in the middle of a character (of at most 4 bytes: a
            // longer run of continuation bytes is no character)
            for ( int back = 0; back < 3 && i > 0 && ( static_cast<unsigned char>( text[i] ) & 0xc0 ) == 0x80;
                  ++back ) {
                --i;
                out.erase( out.size() - 1 );
            }
            char note[96];
            snprintf( note, sizeof note, "\n[... %zu more bytes not shown]", text.size() - i );
            out += note;
            return;
        }
        switch ( text[i] ) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\0': out += "\xef\xbf\xbd"; break;
        default: out += text[i]; break;
        }
    }
}

// Gumbo's memory, freed in one go after the tree has been written, and
// a budget of what Gumbo may allocate (in all, freed or not): beyond it
// the parse is abandoned (longjmp out of Gumbo, which is C: no destructors
// are skipped; every block it holds is in the list "blocks") and the
// document is shown as its source text.  So it is too beyond what Gumbo
// may hold at once (each block counted with the list's header and
// malloc's: see maxHeld), and when malloc fails.
//
// The same happens when the parse and the walk have taken more than
// maxParseCpu seconds plus maxParseCpuPerByte per byte of input of this
// thread's CPU time (load elsewhere on the machine counts little): a
// safety net under the estimate (ParseCost), should Gumbo find work it
// does not foresee.  An ordinary message stays far below it (7.5 MB of
// KMail's plain text HTML: 0.4 s, 3.9 s allowed; the time per byte allowed
// is 4-5 times what that takes, for slower or throttled machines), but
// never more than maxParseCpuTotal (what the net lets a hostile message
// hold KMail's thread for, whatever its size); every
// 64th allocation looks at the clock, so the net holds where Gumbo
// allocates (every tag does), not within one long run of text.
static const double maxParseCpu = 2.0;
static const double maxParseCpuPerByte = 0.25e-6;
static const double maxParseCpuTotal = 4.0;

// What the sanitized document may grow to (sanitizeHtml()).
static const size_t maxOutput = 8 * 1024 * 1024;
static const size_t maxOutputPerByte = 8;

// What Gumbo may hold at once: maxHeld plus maxHeldPerByte per byte of
// input, maxHeldTotal at most.  KMail's plain text holds 36 bytes per byte
// of one-character lines (780 per line), less for longer ones: a 7.5 MB
// log (100000 lines) 84 MB; 300000 one-character lines (6.6 MB) 235 MB,
// 400000 (8.8 MB) 312 MB, of 550 allowed; about 640000 (14 MB) reach the
// ceiling.  Empty elements take more, 75 bytes per byte of "<br>", 80-120
// of "<p>": a flood of them stops at half of what it would hold or at
// maxHeldTotal.
static const size_t maxHeld = 128 * 1024 * 1024;
static const size_t maxHeldPerByte = 48;
static const size_t maxHeldTotal = 512 * 1024 * 1024;

// Each block Gumbo gets is preceded by a Block (32 bytes on LP64, a
// multiple of malloc's alignment: what follows stays aligned), in a list:
// no allocation of ours that could fail within Gumbo.
struct Block
{
    Block* prev;
    Block* next;
    size_t size;
    size_t unused;
};
// what a block costs beyond its size: the Block, malloc's header and
// rounding
static const size_t blockOverhead = sizeof( Block ) + 16;
static_assert( sizeof( Block ) % alignof( max_align_t ) == 0, "a block's header keeps malloc's alignment" );

struct Allocations
{
    Block* blocks;
    size_t allocated;   // in all, freed or not
    size_t budget;
    size_t held;   // now, with blockOverhead each
    size_t maxHeld;
    size_t peak;
    unsigned calls;
    double cpuDeadline;
    jmp_buf abandon;
};

static void* allocate( void* userdata, size_t size )
{
    Allocations* a = static_cast<Allocations*>( userdata );
    a->allocated += size;
    a->held += size + blockOverhead;
    if ( a->allocated > a->budget || a->held > a->maxHeld )
        longjmp( a->abandon, 1 );
    if ( ( ++a->calls & 63 ) == 0 && threadCpuSeconds() > a->cpuDeadline )
        longjmp( a->abandon, 1 );
    Block* b = static_cast<Block*>( malloc( sizeof( Block ) + size ) );
    if ( !b )
        longjmp( a->abandon, 1 );   // out of memory
    if ( a->held > a->peak )
        a->peak = a->held;
    b->size = size;
    b->prev = 0;
    b->next = a->blocks;
    if ( a->blocks )
        a->blocks->prev = b;
    a->blocks = b;
    return b + 1;
}

static void deallocate( void* userdata, void* p )
{
    if ( !p )
        return;
    Allocations* a = static_cast<Allocations*>( userdata );
    Block* b = static_cast<Block*>( p ) - 1;
    if ( b->prev )
        b->prev->next = b->next;
    else
        a->blocks = b->next;
    if ( b->next )
        b->next->prev = b->prev;
    a->held -= b->size + blockOverhead;
    free( b );
}

static void freeAll( Allocations* arena )
{
    while ( Block* b = arena->blocks ) {
        arena->blocks = b->next;
        free( b );
    }
    arena->held = 0;
#ifdef __GLIBC__
    // glibc keeps freed small blocks for reuse: after a large parse (up to
    // maxHeldTotal) the process (KMail) would stay that large.  Given back
    // to the system (a few ms).
    if ( arena->peak > 64 * 1024 * 1024 )
        malloc_trim( 0 );
#endif
}

static std::string sourceText( const std::string& html )
{
    std::string out = "<!DOCTYPE html>\n<html><body><p>[This message is too complex to be shown; its source:]</p><pre>\n";
    // one block of the most it can take (no copies left behind growing)
    out.reserve( out.size() + 4 * 1024 * 1024 + 2 * html.size() + 256 );
    escapeSource( out, html );
    out += "</pre></body></html>\n";
    return out;
}

// The test tool alone, with KWPE_SANITIZE_STATS set: how it went, on
// stderr.
static void stats( const char* what, const Allocations& arena, const ParseCost& cost, size_t out )
{
#ifdef KWPE_SANITIZE_TESTING
    if ( getenv( "KWPE_SANITIZE_STATS" ) )
        fprintf( stderr,
                 "%s: allocated %zu of %zu, held %zu at most, output %zu; elements %zu, attributes %zu, "
                 "clones %zu (%zu bytes)\n",
                 what, arena.allocated, arena.budget, arena.peak, out, cost.elements(), cost.attributes(),
                 cost.clones(), cost.cloneBytes() );
#else
    (void)what;
    (void)arena;
    (void)cost;
    (void)out;
#endif
}

std::string sanitizeHtml( const std::string& html, Profile profile, bool external )
{
    double n = double( html.size() );
    ParseCost cost( html, maxParseWork + maxParseWorkPerByte * n, maxCloneBytes + maxCloneBytesPerByte * n );
    if ( !cost.run() )
        return sourceText( html );   // Gumbo would work or clone too much (see maxParseWork)
    GumboOptions options = kGumboDefaultOptions;
    options.max_errors = 0;   // no error list: not needed, can be large
    Allocations arena;
    arena.blocks = 0;
    arena.allocated = 0;
    arena.held = 0;
    arena.maxHeld = std::min( maxHeld + maxHeldPerByte * html.size(), maxHeldTotal );
    arena.peak = 0;
    // Gumbo makes an element for each start tag that is not ignored
    // (175-500 bytes with its attributes and text: KMail's plain text, one
    // <div dir="ltr"> per line, 450-490 bytes per tag; an empty line, <br>,
    // 175 bytes for 4), and keeps text and attributes 3-7 times their size
    // (the corpus's newsletters: 5-6 times theirs).  A clone of a
    // formatting element is an element with a copy of its attributes.
    // Tags Gumbo ignores (a <td> outside a table) count nothing.
    arena.budget = 4 * 1024 * 1024 + 16 * html.size() + 512 * cost.elements() + 256 * cost.attributes() +
                   512 * cost.clones() + 2 * cost.cloneBytes();
    arena.calls = 0;
    double cpuLimit = std::min( maxParseCpu + maxParseCpuPerByte * double( html.size() ), maxParseCpuTotal );
#ifdef KWPE_SANITIZE_TESTING
    // the test tool alone (tests/render-only/kwpe-sanitize) may lower it
    const char* ms = getenv( "KWPE_SANITIZE_CPU_LIMIT_MS" );
    if ( ms && *ms )
        cpuLimit = atof( ms ) / 1000;
#endif
    arena.cpuDeadline = threadCpuSeconds() + cpuLimit;
    options.allocator = allocate;
    options.deallocator = deallocate;
    options.userdata = &arena;
    if ( setjmp( arena.abandon ) ) {
        freeAll( &arena );
        stats( "parse abandoned", arena, cost, 0 );
        return sourceText( html );
    }
    GumboOutput* output = gumbo_parse_with_options( &options, html.data(), html.size() );
    // The output: KMail's plain text grows by a third at most (an empty
    // line's <br> becomes <br />, entities stay); beyond maxOutput plus
    // maxOutputPerByte per byte of input, or past the CPU deadline, the
    // walk is abandoned as the parse is.
    Serializer s( profile, external, maxOutput + maxOutputPerByte * html.size(), arena.cpuDeadline );
    // The document's mode as Gumbo found it (tables of mails depend on
    // quirks mode): no doctype is quirks mode.
    switch ( output->document->v.document.doc_type_quirks_mode ) {
    case GUMBO_DOCTYPE_NO_QUIRKS:
        s.out = "<!DOCTYPE html>\n";
        break;
    case GUMBO_DOCTYPE_LIMITED_QUIRKS:
        s.out = "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
                "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n";
        break;
    case GUMBO_DOCTYPE_QUIRKS:
    default:
        break;
    }
    s.walk( output->root );
    // Not gumbo_destroy_output(): Gumbo frees its tree recursively (a deep
    // message would exhaust the stack there).  Everything Gumbo allocated
    // is ours to free.
    freeAll( &arena );
    stats( s.abandoned ? "walk abandoned" : "written", arena, cost, s.out.size() );
    if ( s.abandoned ) {
        std::string().swap( s.out );
        return sourceText( html );
    }
    return s.out;
}

} // namespace KWPEMailSanitizer
