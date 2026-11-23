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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
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

namespace
{

enum ElementAction { Keep, Unwrap, Drop, RenameToPre };

struct Serializer
{
    Serializer( Profile p, bool e ) : profile( p ), external( e ), depth( 0 ) {}

    void open( const GumboNode* n, std::string& close, bool& descend );
    void walk( const GumboNode* root );
    void text( const char* t );
    void attribute( const char* name, const std::string& value );
    ElementAction actionFor( const GumboElement& e, std::string& name ) const;
    bool allowedAttribute( const std::string& element, const std::string& attr ) const;

    Profile profile;
    bool external;
    std::string out;
    unsigned depth;   // elements written and open
    static const unsigned maxDepth = 400;
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

// The tree without recursion: a message may nest elements 100000 deep
// (Gumbo builds that tree; WebKit's parser would flatten it at 512).  The
// stack lives on the heap; elements deeper than maxDepth are unwrapped.
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
            continue;
        }
        if ( child->type != GUMBO_NODE_ELEMENT )
            continue;   // template (inert), CDATA (foreign content), comments
        Frame g;
        g.node = child;
        g.next = 0;
        open( child, g.close, descend );
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

// Gumbo's work grows with the depth of its stack of open elements times
// the number of tags (100000 nested <div>: 12 s), and reconstructing
// formatting elements multiplies nodes (1000 open <font> before 1000
// paragraphs: a million elements).  A bound that nothing in the document
// can lower: D, every start tag of an element that can hold others (not
// void, not one whose end tag is implied by a sibling: those nest only
// inside elements that count), times T, every tag; both counted without
// regard to quotes, comments or raw text (counting too many is safe,
// skipping one is not).  Above the bound the document is not parsed: it
// is shown as its source text.  The corpus's newsletters (35-45 KB) have
// D x T = 2e4-4e4; at 5e6 the worst shapes measured take a few ms.  What
// the bound cannot see, the nodes Gumbo makes by reconstructing formatting
// elements, the allocation budget below stops (parseBudget()).
static const double maxParseWork = 5e6;

static bool isTagNameChar( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '-';
}

static bool tooMuchParsing( const std::string& html )
{
    static const char* const noContentNesting[] = {
        // void
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track",
        "wbr", "keygen", "basefont", "bgsound", "frame", "image", "isindex",
        // end tag implied by a sibling; nested only inside counted elements
        "p", "li", "dt", "dd", "tr", "td", "th", "tbody", "thead", "tfoot", "colgroup", "option", "optgroup",
        "rt", "rp", "rb", "rtc", "html", "head", "body", 0
    };
    double containers = 0, tags = 0;
    size_t n = html.size();
    for ( size_t i = html.find( '<' ); i != std::string::npos && i + 1 < n; i = html.find( '<', i + 1 ) ) {
        char c = html[i + 1];
        if ( c == '/' || c == '!' || c == '?' ) {
            tags += 1;
            continue;
        }
        if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ) )
            continue;
        tags += 1;
        size_t end = i + 1;
        while ( end < n && end - i <= 16 && isTagNameChar( html[end] ) )
            ++end;
        if ( !inList( lower( html.substr( i + 1, end - i - 1 ) ), noContentNesting ) )
            containers += 1;
    }
    return containers * tags > maxParseWork;
}

// The source text in a <pre>: every character as text, NUL as U+FFFD.
static void escapeSource( std::string& out, const std::string& text )
{
    for ( size_t i = 0; i < text.size(); ++i ) {
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
// are skipped; everything it allocated is in "pointers") and the document
// is shown as its source text.
struct Allocations
{
    std::unordered_set<void*> pointers;
    size_t allocated;
    size_t budget;
    jmp_buf abandon;
};

static void* allocate( void* userdata, size_t size )
{
    Allocations* a = static_cast<Allocations*>( userdata );
    a->allocated += size;
    if ( a->allocated > a->budget )
        longjmp( a->abandon, 1 );
    void* p = malloc( size ? size : 1 );
    if ( p )
        a->pointers.insert( p );
    return p;
}

static void deallocate( void* userdata, void* p )
{
    if ( !p )
        return;
    static_cast<Allocations*>( userdata )->pointers.erase( p );
    free( p );
}

static void freeAll( Allocations* arena )
{
    for ( std::unordered_set<void*>::const_iterator it = arena->pointers.begin(); it != arena->pointers.end(); ++it )
        free( *it );
    arena->pointers.clear();
}

static std::string sourceText( const std::string& html )
{
    std::string out = "<!DOCTYPE html>\n<html><body><p>[This message is too complex to be shown; its source:]</p><pre>\n";
    escapeSource( out, html );
    return out + "</pre></body></html>\n";
}

std::string sanitizeHtml( const std::string& html, Profile profile, bool external )
{
    if ( tooMuchParsing( html ) )
        return sourceText( html );   // not parsed (see maxParseWork)
    GumboOptions options = kGumboDefaultOptions;
    options.max_errors = 0;   // no error list: not needed, can be large
    Allocations arena;
    arena.allocated = 0;
    // The corpus's newsletters: 5-6 times their size (0.2 MB for 40 KB).
    arena.budget = 4 * 1024 * 1024 + 16 * html.size();
    options.allocator = allocate;
    options.deallocator = deallocate;
    options.userdata = &arena;
    if ( setjmp( arena.abandon ) ) {
        freeAll( &arena );
        return sourceText( html );
    }
    GumboOutput* output = gumbo_parse_with_options( &options, html.data(), html.size() );
    Serializer s( profile, external );
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
    return s.out;
}

} // namespace KWPEMailSanitizer
