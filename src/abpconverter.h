/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef ABPCONVERTER_H
#define ABPCONVERTER_H

/*
 * Adblock Plus filter lists (EasyList syntax, the uBlock Origin subset
 * that has a meaning without scripts) -> WebKit content blocker JSON
 * (the "content rule list" format WebKitUserContentFilterStore compiles).
 *
 * Plain C++ (no Qt, no GLib): kwpe-filterconv runs it outside the
 * browser.  What WebKit cannot express is dropped and counted, never
 * approximated by something that blocks more: a filter list that blocks
 * too much breaks sites.
 *
 * WebKit applies "ignore-previous-rules" only within one rule list, so
 * every list written here carries all the exceptions that may cancel its
 * rules; lists stay below WebKit's limit of 150000 rules.
 */

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ABP
{

struct Stats
{
    Stats();

    unsigned lines;              // filter lines read (no comments, no blank lines)
    unsigned duplicates;         // the same filter twice (also across lists)
    unsigned badfiltered;        // disabled by a $badfilter filter (both counted)
    unsigned networkBlock;       // network rules written (block)
    unsigned networkException;   // network rules written (exceptions)
    unsigned cosmeticGeneric;    // css-display-none rules on every site
    unsigned cosmeticSpecific;   // css-display-none rules for some sites
    unsigned cosmeticExceptions; // #@# filters applied
    unsigned regexConverted;     // /regex/ filters WebKit understands after rewriting
    unsigned scriptlets;         // scriptlet injections written
    unsigned scriptletExceptions; // #@#+js filters applied

    // dropped, by reason
    std::map<std::string, unsigned> dropped;

    unsigned droppedTotal() const;
};

class Converter
{
public:
    Converter();

    // One rule, already in WebKit terms.
    struct Rule
    {
        std::string urlFilter;
        bool caseSensitive;
        std::vector<std::string> resourceTypes;   // empty: any
        std::string loadType;                      // "", "first-party", "third-party"
        std::vector<std::string> ifDomain;       // sites (and their subdomains)
        std::vector<std::string> unlessDomain;
        std::vector<std::string> ifTopURL;
        std::string requestMethod;
        std::string action;                        // block, ignore-previous-rules, css-display-none
        std::string selector;
        Rule() : caseSensitive( false ) {}
    };

    /**
     * The text of a filter list ("!#include name": see setIncludeLoader()).
     * @p trusted: a list of ours (uBlock's own lists, from their fixed
     * URLs), whose "trusted-" scriptlets run; never a list of the user's.
     */
    void addList( const std::string& text, bool trusted = false );

    /**
     * A scriptlet ("sites##+js(name, args)") the part runs in the pages
     * of those sites, before their own scripts.  Sites: domains (and
     * their subdomains) or uBlock entities ("example.*").
     */
    struct Scriptlet
    {
        std::string name;                  // canonical: "set-constant", not "set" or "set.js"
        std::vector<std::string> args;
        std::vector<std::string> include;  // never empty
        std::vector<std::string> exclude;  // sites below them, "#@#+js" exceptions
    };
    /** The scriptlets, after ruleLists(); scriptletsText() writes them. */
    const std::vector<Scriptlet>& scriptlets() const { return m_scriptlets; }

    /**
     * The rule lists, each at most @p maxRules rules (WebKit's limit is
     * 150000); toJSON() makes the JSON.  Call once, after all addList().
     */
    std::vector<std::vector<Rule> > ruleLists( unsigned maxRules = 50000 );

    /**
     * Included files (uBlock's "!#include"): called with the name, returns
     * the text or false.  Without it includes are dropped.
     */
    typedef bool ( *IncludeLoader )( const std::string& name, std::string& text, void* data );
    void setIncludeLoader( IncludeLoader loader, void* data );

    Stats stats;
    /** Report every dropped filter on stderr ("drop REASON FILTER"). */
    bool verbose;

private:
    void addText( const std::string& text, int depth );
    void addLine( const std::string& line, int depth );
    bool active() const;
    void convertNetwork( const std::string& filter );
    void convertCosmetic( const std::string& domains, const std::string& selector, bool exception );
    void convertScriptlet( const std::string& domains, const std::string& body, bool exception, bool trusted );
    void drop( const char* reason );

    IncludeLoader m_includeLoader;
    void* m_includeData;

    std::vector<std::string> m_filters;     // in order, unique
    std::vector<bool> m_filterTrusted;      // ... from a trusted list
    std::map<std::string, size_t> m_seen;   // filter -> index
    bool m_trusted;                         // the list being read

    // results
    std::vector<Rule> m_block;              // network blocks
    std::vector<Rule> m_important;          // $important blocks: after the exceptions
    std::vector<Rule> m_importantCarveOuts; // ~sub.site of an $important block: after it
    std::vector<Rule> m_exceptions;         // network exceptions (any list)
    std::vector<Rule> m_documentExceptions; // $document: whole pages, cancels everything
    std::vector<Rule> m_elemhideExceptions; // $elemhide: no cosmetic filtering on the page
    std::vector<Rule> m_generichideExceptions; // $generichide: no generic cosmetic filtering
    std::vector<Rule> m_specifichideExceptions; // $specifichide: no site specific cosmetic filtering

    // One "sites##selector" filter (several filters may share a selector).
    struct Cosmetic
    {
        std::string selector;
        std::vector<std::string> include;   // empty: every site
        std::vector<std::string> exclude;
    };
    std::vector<Cosmetic> m_cosmetic;
    std::map<std::string, std::set<std::string> > m_cosmeticExceptions; // selector -> sites ("" = all)
    std::set<std::string> m_badfilters;
    std::vector<Scriptlet> m_scriptlets;
    std::map<std::string, std::set<std::string> > m_scriptletExceptions; // name+args ("" = all) -> sites ("" = all)
    std::string m_current;            // the filter being converted (verbose)
    std::vector<bool> m_ifStack;      // !#if ... !#endif: active?
};

/**
 * An ABP URL pattern (the part before "$", without "@@") as a WebKit
 * url-filter regular expression; false if WebKit cannot express it.
 * Exposed for the tests.
 */
bool patternToURLFilter( const std::string& pattern, std::string& urlFilter, std::string& whyNot );

/**
 * A "/regex/" filter as WebKit url-filters (alternatives expanded).
 * @p widen (exceptions): \b is left out, a long {n,m} becomes + or *.
 */
bool regexToURLFilters( const std::string& regex, std::vector<std::string>& urlFilters, std::string& whyNot,
                        bool widen = false );

/** The url-filter can match an empty string (WebKit: every URL). */
bool canMatchEmpty( const std::string& urlFilter );

/**
 * The body of a scriptlet filter ("+js(name, arg, ...)") as its canonical
 * name (aliases resolved) and arguments (uBlock's quoting and "\\,"
 * escapes); false if it is not one we run (whyNot: why).  @p trusted:
 * from a trusted list ("trusted-" scriptlets are refused otherwise).
 */
bool parseScriptlet( const std::string& body, bool trusted, std::string& name, std::vector<std::string>& args,
                     std::string& whyNot );

/**
 * The scriptlets for the part: a first line "kwpe-scriptlets 1", then
 * one line per scriptlet: sites TAB excluded sites TAB a JSON array
 * [name, args...] (sites comma-separated).
 */
std::string scriptletsText( const std::vector<Converter::Scriptlet>& scriptlets );

/** The rules as a WebKit JSON array. */
std::string toJSON( const std::vector<Converter::Rule>& rules );

}

#endif
