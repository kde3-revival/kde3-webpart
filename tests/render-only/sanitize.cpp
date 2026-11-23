/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

// kwpe-sanitize [html|css] [content|host] [render-only|external] < in > out:
// the mail mode's first layer alone (what WebKit gets of a message).

#include <iostream>
#include <iterator>
#include <string>

#include "kwpemailsanitizer.h"

int main( int argc, char** argv )
{
    std::string kind = argc > 1 ? argv[1] : "html";
    std::string profile = argc > 2 ? argv[2] : "content";
    std::string mode = argc > 3 ? argv[3] : "render-only";
    std::string in( ( std::istreambuf_iterator<char>( std::cin ) ), std::istreambuf_iterator<char>() );
    KWPEMailSanitizer::Profile p = profile == "host" ? KWPEMailSanitizer::Host : KWPEMailSanitizer::Content;
    bool external = mode == "external";
    std::cout << ( kind == "css" ? KWPEMailSanitizer::sanitizeCss( in, p, external )
                                 : KWPEMailSanitizer::sanitizeHtml( in, p, external ) );
    return 0;
}
