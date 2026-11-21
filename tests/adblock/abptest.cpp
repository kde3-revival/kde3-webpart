/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// abptest: the Adblock Plus -> WebKit converter (src/abpconverter.cpp) on
// tricky filters.  URL filters are matched with std::regex (ECMAScript,
// what WebKit's subset is a part of), searched anywhere in the URL like
// WebKit does.  Prints FAIL lines and exits 1 on a failure.

#include <stdio.h>

#include <regex>
#include <string>
#include <vector>

#include "abpconverter.h"

typedef ABP::Converter::Rule Rule;

static int failures = 0;
static int checks = 0;

static void fail( const std::string& what )
{
    ++failures;
    printf( "FAIL %s\n", what.c_str() );
}

// All rules a list of filters gives (every rule list, in order).
static std::vector<Rule> convert( const std::string& text, ABP::Stats* stats = 0 )
{
    ABP::Converter c;
    c.addList( text );
    std::vector<std::vector<Rule> > lists = c.ruleLists();
    std::vector<Rule> all;
    for ( size_t i = 0; i < lists.size(); ++i )
        all.insert( all.end(), lists[i].begin(), lists[i].end() );
    if ( stats )
        *stats = c.stats;
    return all;
}

static bool matches( const Rule& r, const std::string& url )
{
    std::regex re( r.urlFilter, r.caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase );
    return std::regex_search( url, re );
}

// The block rule of a single network filter matches (or not) the URL.
static void network( const std::string& filter, const std::string& url, bool expected )
{
    ++checks;
    std::vector<Rule> rules = convert( filter );
    bool any = false;
    for ( size_t i = 0; i < rules.size(); ++i )
        if ( rules[i].action == "block" && matches( rules[i], url ) )
            any = true;
    if ( any != expected )
        fail( filter + " on " + url + ( expected ? ": should match" : ": should not match" ) );
}

static void dropped( const std::string& filter, const char* reason )
{
    ++checks;
    ABP::Stats stats;
    std::vector<Rule> rules = convert( filter, &stats );
    if ( stats.dropped[reason] != 1 )
        fail( filter + ": should be dropped as " + reason + ( rules.empty() ? " (dropped otherwise)" : " (converted)" ) );
}

static const Rule* findRule( const std::vector<Rule>& rules, const std::string& action )
{
    for ( size_t i = 0; i < rules.size(); ++i )
        if ( rules[i].action == action )
            return &rules[i];
    return 0;
}

static std::string join( const std::vector<std::string>& v )
{
    std::string s;
    for ( size_t i = 0; i < v.size(); ++i )
        s += ( i ? "," : "" ) + v[i];
    return s;
}

static void expectEq( const std::string& what, const std::string& got, const std::string& expected )
{
    ++checks;
    if ( got != expected )
        fail( what + ": got [" + got + "], expected [" + expected + "]" );
}

int main()
{
    // --- URL patterns -------------------------------------------------
    network( "||example.com^", "https://example.com/", true );
    network( "||example.com^", "https://www.example.com/x", true );
    network( "||example.com^", "http://example.com:8080/", true );
    network( "||example.com^", "https://notexample.com/", false );
    network( "||example.com^", "https://example.com.evil.net/", false );
    network( "||example.com^", "https://other.net/?u=https://example.com/", false );
    network( "||example.com/ads/", "https://cdn.example.com/ads/1.png", true );
    network( "||example.com/ads/", "https://other.net/example.com/ads/", false );
    network( "||ads.", "https://ads.example.com/", true );
    network( "||ads.", "https://myads.example.com/", false );
    network( "/banner/*/img^", "https://x.com/banner/a/img?x=1", true );
    network( "/banner/*/img^", "https://x.com/banner/a/img", true );
    network( "/banner/*/img^", "https://x.com/banner/a/imgx", false );
    network( "|https://ads.", "https://ads.x.com/", true );
    network( "|https://ads.", "http://x.com/?u=https://ads.y/", false );
    network( "swf|", "https://x.com/a.swf", true );
    network( "swf|", "https://x.com/a.swf?x", false );
    network( "a.b?c", "https://x.com/a.b?c", true );
    network( "a.b?c", "https://x.com/aXb?c", false );
    network( "a.b?c", "https://x.com/a.bc", false );
    network( "&ad_type=", "https://x.com/p?q=1&ad_type=2", true );
    network( "/ad(1)[2].js", "https://x.com/ad(1)[2].js", true );
    network( "/ad|x", "https://x.com/ad|x", true );
    network( "/ad|x", "https://x.com/ad", false );
    network( "-ad-*.gif", "https://x.com/a-ad-123.gif", true );
    network( "/Ads/$match-case", "https://x.com/ads/", false );
    network( "/Ads/$match-case", "https://x.com/Ads/", true );
    network( "/Ads/", "https://x.com/ads/", true );

    // --- /regex/ filters ----------------------------------------------
    network( "/banner\\d+\\.gif/", "https://x.com/banner12.gif", true );
    network( "/banner\\d+\\.gif/", "https://x.com/bannerX.gif", false );
    network( "/^https?:\\/\\/(ads|track)\\./", "https://track.x.com/", true );
    network( "/^https?:\\/\\/(ads|track)\\./", "https://x.com/ads.js", false );
    network( "/ad[0-9]{2,3}\\.js/", "https://x.com/ad12.js", true );
    network( "/ad[0-9]{2,3}\\.js/", "https://x.com/ad1.js", false );
    network( "/ad[0-9]{2,3}\\.js/", "https://x.com/ad1234.js", false );
    network( "/(?:pop|push)under/$script", "https://x.com/popunder.js", true );
    dropped( "/\\bads\\b/", "regex-word-boundary" );
    dropped( "/ad(?=s)/", "regex-lookaround" );
    dropped( "/ad\\s/", "regex-escape" );
    dropped( "/a{20}/", "regex-repeat" );

    // --- options --------------------------------------------------------
    {
        std::vector<Rule> r = convert( "||x.com^$script,third-party" );
        const Rule* b = findRule( r, "block" );
        expectEq( "types of $script", b ? join( b->resourceTypes ) : "", "script" );
        expectEq( "load type of $third-party", b ? b->loadType : "", "third-party" );
    }
    {
        std::vector<Rule> r = convert( "||x.com^$~script,~third-party" );
        const Rule* b = findRule( r, "block" );
        expectEq( "types of $~script", b ? join( b->resourceTypes ) : "",
                  "child-document,image,style-sheet,font,media,svg-document,raw" );
        expectEq( "load type of $~third-party", b ? b->loadType : "", "first-party" );
    }
    {
        // no type: everything but the page itself and popups
        std::vector<Rule> r = convert( "||x.com^" );
        const Rule* b = findRule( r, "block" );
        expectEq( "types without an option", b ? join( b->resourceTypes ) : "",
                  "child-document,script,image,style-sheet,font,media,svg-document,raw" );
    }
    {
        std::vector<Rule> r = convert( "||x.com^$xmlhttprequest,subdocument,popup,ping" );
        const Rule* b = findRule( r, "block" );
        expectEq( "xhr, subdocument, popup, ping", b ? join( b->resourceTypes ) : "", "child-document,fetch,ping,popup" );
    }
    {
        // a site with an exception for one of its subdomains: blocked on
        // the site, an exception for the subdomain
        std::vector<Rule> r = convert( "||x.com^$domain=a.com|~b.a.com|c.org" );
        const Rule* b = findRule( r, "block" );
        const Rule* e = findRule( r, "ignore-previous-rules" );
        expectEq( "domain= includes", b ? join( b->ifDomain ) : "", "a.com,c.org" );
        expectEq( "domain= carve-out", e ? join( e->ifDomain ) : "", "b.a.com" );
    }
    {
        std::vector<Rule> r = convert( "||x.com^$domain=~a.com|~b.org" );
        const Rule* b = findRule( r, "block" );
        expectEq( "domain= excludes only", b ? join( b->unlessDomain ) : "", "a.com,b.org" );
    }
    {
        // the excluded site is a parent of the included one: no site left
        dropped( "||x.com^$domain=sub.a.com|~a.com", "domain-empty" );
    }
    {
        // uBlock entities: the name with any public suffix
        std::vector<Rule> r = convert( "||x.com^$domain=example.*" );
        const Rule* b = findRule( r, "block" );
        expectEq( "entity site", b ? join( b->ifDomain ) : "", "example.*" );
        Rule top;
        top.urlFilter = "^[a-z][a-z0-9.+-]*://([^/?#]*\\.)?example\\.[^/:?#]+[:/]";
        ++checks;
        if ( ABP::toJSON( r ).find( "example\\\\.[^/:?#]+[:/]" ) == std::string::npos || !matches( top, "https://www.example.co.uk/" )
             || matches( top, "https://example/" ) || matches( top, "https://myexample.com/" ) )
            fail( "entity site pattern: " + ABP::toJSON( r ) );
    }
    {
        // an exception with entities stays tied to them (not global)
        std::vector<Rule> r = convert( "||gpt.example^$script\n@@||gpt.example/tag.js$script,domain=gentside.*|ohmymag.*" );
        const Rule* e = findRule( r, "ignore-previous-rules" );
        expectEq( "exception with entities", e ? join( e->ifDomain ) : "", "gentside.*,ohmymag.*" );
    }
    dropped( "||x.com^$redirect=noopjs", "option-redirect" );
    dropped( "||x.com^$csp=script-src 'none'", "option-csp" );
    dropped( "||x.com^$removeparam=utm_source", "option-removeparam" );
    dropped( "||x.com^$object", "type-unsupported" );
    dropped( "$script,third-party", "matches-everything" );
    dropped( "*$image", "matches-everything" );
    {
        // pings may go everywhere
        std::vector<Rule> r = convert( "*$ping,third-party" );
        const Rule* b = findRule( r, "block" );
        expectEq( "*$ping,third-party", b ? b->urlFilter + " " + join( b->resourceTypes ) : "", ".* ping" );
    }
    {
        std::vector<Rule> r = convert( "*$script,domain=a.com" );
        const Rule* b = findRule( r, "block" );
        expectEq( "*$script,domain=", b ? b->urlFilter + " " + join( b->ifDomain ) : "", ".* a.com" );
    }
    {
        // $badfilter cancels the filter it names, and itself
        ABP::Stats stats;
        std::vector<Rule> r = convert( "||x.com^$script\n||x.com^$script,badfilter\n||y.com^", &stats );
        const Rule* b = findRule( r, "block" );
        expectEq( "badfilter", b ? b->urlFilter : "", std::string( "^[a-z][a-z0-9.+-]*:(//)?([^/?#]*\\.)?y\\.com" ) + "[^a-zA-Z0-9_.%-]" );
        expectEq( "badfilter count", std::to_string( r.size() ) + " " + std::to_string( stats.badfiltered ), "1 2" );
    }
    {
        // $important blocks come after the exceptions (not cancelled by them)
        std::vector<Rule> r = convert( "||x.com^$important\n@@||x.com^" );
        expectEq( "important order", r.size() == 2 ? r[0].action + " " + r[1].action : "",
                  "ignore-previous-rules block" );
    }
    {
        // exceptions: in every network list
        std::vector<Rule> r = convert( "||a.com^\n@@||a.com/ok^" );
        expectEq( "exception order", r.size() == 2 ? r[0].action + " " + r[1].action : "", "block ignore-previous-rules" );
    }
    {
        // a whole page: every rule (network and css) on pages matching it
        std::vector<Rule> r = convert( "||x.com^\n##.ad\n@@||good.com^$document" );
        int n = 0;
        for ( size_t i = 0; i < r.size(); ++i )
            if ( r[i].action == "ignore-previous-rules" && r[i].urlFilter == ".*" && r[i].ifTopURL.size() == 1 )
                ++n;
        expectEq( "$document exception in each list", std::to_string( n ), "2" );
        Rule page;
        page.urlFilter = r.back().ifTopURL.empty() ? std::string( "x" ) : r.back().ifTopURL[0];
        ++checks;
        if ( !matches( page, "https://www.good.com/a" ) || matches( page, "https://bad.com/?good.com/" ) )
            fail( "$document page pattern" );
    }
    {
        std::vector<Rule> r = convert( "##.ad\nexample.com##.x\n@@||a.com^$generichide" );
        // generichide: only in the list of generic css rules
        expectEq( "generichide placement", r.size() == 3 ? r[0].action + " " + r[1].action + " " + r[2].action : std::to_string( r.size() ),
                  "css-display-none ignore-previous-rules css-display-none" );
    }
    dropped( "@@*$document", "exception-everything" );

    // --- from the review ----------------------------------------------
    {
        // the carve-out of an $important block comes after it
        std::vector<Rule> r = convert( "||ads.example^$important,domain=a.com|~b.a.com" );
        expectEq( "important carve-out order", r.size() == 2 ? r[0].action + " " + r[1].action : std::to_string( r.size() ),
                  "block ignore-previous-rules" );
    }
    dropped( "||track.example^$elemhide", "hide-option-on-block" );
    dropped( "/x{,3}/$image", "regex-syntax" );
    dropped( "/(ads)?/$script", "matches-everything" );
    dropped( "^$script,third-party", "matches-everything" );
    dropped( "||void.example/rec$method=post", "option-method" );
    {
        std::vector<Rule> r = convert( "||x.com^$script\n@@||x.com/ok$method=post" );
        const Rule* e = findRule( r, "ignore-previous-rules" );
        expectEq( "exception $method: any method", e ? "[" + e->requestMethod + "]" : "", "[]" );
    }
    network( "/[\\w-.]+\\.example\\//", "https://a-b.example/", true );
    network( "/[\\w-~]x/", "https://a/{x", false );
    {
        ABP::Stats stats;
        std::vector<Rule> r = convert( "||baz.example^$script,third-party\n||baz.example^$third-party,script,badfilter", &stats );
        expectEq( "badfilter, other option order", std::to_string( r.size() ) + " " + std::to_string( stats.badfiltered ), "0 2" );
    }
    {
        std::vector<Rule> r = convert( "!#if env_safari\n||safari.example^\n!#else\n||other.example^\n!#endif" );
        expectEq( "env_safari", r.size() == 1 ? r[0].urlFilter.substr( r[0].urlFilter.rfind( ")?" ) + 2 ) : std::to_string( r.size() ),
                  "safari\\.example[^a-zA-Z0-9_.%-]" );
    }
    {
        // exceptions: what cannot be said matches more (blocks less)
        std::vector<Rule> r = convert( "||img.example^\n@@/img\\.example\\/[a-z0-9]{32}\\b/$image" );
        const Rule* e = findRule( r, "ignore-previous-rules" );
        expectEq( "widened exception", e ? e->urlFilter : "", "img\\.example/[a-z0-9][a-z0-9]*" );
        dropped( "/[a-z0-9]{32}\\b/$image", "regex-repeat" );
    }

    // --- cosmetic -------------------------------------------------------
    {
        std::vector<Rule> r = convert( "##.ad\na.com#@#.ad" );
        const Rule* c = findRule( r, "css-display-none" );
        expectEq( "generic with an exception", c ? c->selector + " unless " + join( c->unlessDomain ) : "", ".ad unless a.com" );
    }
    {
        std::vector<Rule> r = convert( "##.ad\n#@#.ad" );
        expectEq( "global exception", std::to_string( r.size() ), "0" );
    }
    {
        std::vector<Rule> r = convert( "a.com,b.org,~x.b.org##.ad" );
        const Rule* c = findRule( r, "css-display-none" );
        // b.org has an excluded subdomain: left out (hiding less)
        expectEq( "site with an excluded subdomain", c ? join( c->ifDomain ) : "", "a.com" );
    }
    {
        std::vector<Rule> r = convert( "A.COM##.ad\na.com#@#.ad" );
        expectEq( "site list case, site exception", std::to_string( r.size() ), "0" );
    }
    {
        std::vector<Rule> r = convert( "a.com##.x:-abp-has(.y)" );
        const Rule* c = findRule( r, "css-display-none" );
        expectEq( ":-abp-has", c ? c->selector : "", ".x:has(.y)" );
    }
    {
        std::vector<Rule> r = convert( "a.com##div[data-x=\"a\\\"b\"]" );
        expectEq( "quotes in selectors (JSON)", ABP::toJSON( r ).find( "div[data-x=\\\"a\\\\\\\"b\\\"]" ) != std::string::npos ? "ok" : ABP::toJSON( r ), "ok" );
    }
    dropped( "a.com##.x:has-text(Ad)", "procedural-cosmetic" );
    dropped( "a.com##.x:style(display: block !important)", "procedural-cosmetic" );
    dropped( "a.com##+js(set-constant, x, 1)", "scriptlet" );
    dropped( "a.com##^script:has-text(ad)", "html-filter" );
    dropped( "a.com#?#.x:-abp-contains(Ad)", "extended-css" );
    dropped( "a.com#$#abort-on-property-read x", "snippet-or-css-injection" );
    {
        std::vector<Rule> r = convert( "example.*##.ad" );
        const Rule* c = findRule( r, "css-display-none" );
        expectEq( "entity cosmetic", c ? join( c->ifDomain ) : "", "example.*" );
    }
    dropped( "a.com##.x[", "cosmetic-syntax" );

    // --- sites as top URL patterns (not WebKit's if-domain) -----------
    {
        std::vector<Rule> r = convert( "a.com##.ad" );
        std::string json = ABP::toJSON( r );
        ++checks;
        if ( json.find( "\"if-top-url\":[\"^[a-z][a-z0-9.+-]*://([^/?#]*\\\\.)?a\\\\.com[:/]\"]" ) == std::string::npos )
            fail( "site pattern in JSON: " + json );
        Rule top;
        top.urlFilter = "^[a-z][a-z0-9.+-]*://([^/?#]*\\.)?a\\.com[:/]";
        ++checks;
        if ( !matches( top, "https://a.com/" ) || !matches( top, "https://www.a.com:8080/" )
             || matches( top, "https://b.org/?next=https://a.com/" ) || matches( top, "https://xa.com/" ) )
            fail( "site pattern matching" );
    }

    // --- list syntax ----------------------------------------------------
    {
        ABP::Stats stats;
        std::vector<Rule> r = convert( "[Adblock Plus 2.0]\n! comment\n# hosts comment\n\n"
                                       "!#if env_firefox\n||ff.com^\n!#endif\n"
                                       "!#if !env_firefox\n||notff.com^\n!#else\n||else.com^\n!#endif\n"
                                       "||dup.com^\r\n||dup.com^\n##.x\n", &stats );
        std::string urls;
        for ( size_t i = 0; i < r.size(); ++i )
            urls += r[i].action == "block" ? r[i].urlFilter.substr( r[i].urlFilter.rfind( ")?" ) + 2 ) + " " : "";
        expectEq( "preprocessor and duplicates", urls,
                  "notff\\.com[^a-zA-Z0-9_.%-] dup\\.com[^a-zA-Z0-9_.%-] " );
        expectEq( "lines, duplicates", std::to_string( stats.lines ) + " " + std::to_string( stats.duplicates ), "4 1" );
    }

    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
