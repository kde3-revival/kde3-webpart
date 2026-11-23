/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

#ifndef KWPEMAILSANITIZER_H
#define KWPEMAILSANITIZER_H

#include <string>

// Mail mode, first layer: the HTML of a message is rebuilt from a white
// list before WebKit sees it.  Plain C++ (no Qt, no WebKit): the tests
// link it on its own.
//
// Parser: Gumbo, an implementation of the WHATWG HTML parsing algorithm
// (tokenizer and tree construction, the one WebKit implements too).  The
// output is not the input with parts cut out: it is serialized from
// Gumbo's tree by us (known elements, quoted and escaped attributes,
// escaped text, no comments, no raw text but our own CSS), so WebKit
// parses a document whose tree is the one we checked.  Where Gumbo's
// tree differs from the one WebKit would have built from the original
// (Gumbo follows an older revision of the standard: <select> parsing,
// <search>, customizable <select>, declarative shadow DOM, which Gumbo
// does not know and treats as ordinary or unknown elements) the message
// looks different, but what WebKit gets is still only what we wrote.
//
// CSS (<style>, style attributes): our own tokenizer after CSS Syntax
// Level 3 finds every url(), src(), @import and the URL strings of
// image-set(), image(), cross-fade(); the rest is copied as it was.
namespace KWPEMailSanitizer
{
    enum Profile {
        // the host's document (KMail's headers, attachment list, plain
        // text parts): frames of the message's HTML parts (body-<n>.html),
        // the host's links (any scheme but script ones), images and styles
        // of the same origin (the part serves only the message's
        // directory and KDE's icons)
        Host,
        // a message's HTML: no frames, links only to http(s), mailto, ftp,
        // news and anchors; images and fonts only data: (cid: is data:
        // when the host wrote it), remote ones only if external
        Content
    };

    // A complete document (UTF-8) as WebKit shall get it.  @p external:
    // the user allowed this message's remote images, styles and fonts.
    std::string sanitizeHtml( const std::string& html, Profile profile, bool external );
    // A style sheet or the value of a style attribute.
    std::string sanitizeCss( const std::string& css, Profile profile, bool external );

    enum UrlUse { Link, Image, Style, Font };
    // Whether the URL may stay in an attribute or in CSS.
    bool allowedUrl( const std::string& url, UrlUse use, Profile profile, bool external );
}

#endif
