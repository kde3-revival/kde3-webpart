#    kde3-wpepart: a WPE WebKit browser part for Konqueror (KDE 3.5)
#
#    This library is free software; you can redistribute it and/or modify
#    it under the terms of the GNU Lesser General Public License as
#    published by the Free Software Foundation; either version 2.1 of the
#    License, or (at your option) any later version.

cmake_minimum_required(VERSION 3.20...4.4)

project(kde3-wpepart VERSION 0.1.0 LANGUAGES C CXX)

# kdelibs: the CMake package, install prefix, compiler settings, RUNPATH
include(cmake/KDELibs.cmake)

find_package(PkgConfig REQUIRED)
# WebKit with the WPEPlatform API (WebKitWebView "display" property);
# 2.54 is what this was written against, older 2.4x lack parts of it.
pkg_check_modules(WPEWEBKIT REQUIRED IMPORTED_TARGET wpe-webkit-2.0>=2.50 wpe-platform-2.0>=2.50)
# Mail mode: the HTML sanitizer's parser (the WHATWG algorithm)
pkg_check_modules(GUMBO REQUIRED IMPORTED_TARGET gumbo>=0.10)
# X11 presentation: MIT-SHM (fallback), DRI3 + Present (DMA-BUF, zero copy)
pkg_check_modules(KWPEX11 REQUIRED IMPORTED_TARGET
  x11 x11-xcb xext xfixes xpresent xrandr xcb xcb-dri3 libdrm)

add_subdirectory(src)

# tests/render-only: its driver, the sanitizer alone and a build of the
# part (libkwpepart_test) whose mail mode layers can be switched off
# (KWPE_MAIL_TEST_DISABLE_LAYERS); nothing of it is installed.
option(KWPE_RENDER_ONLY_TEST "Build the render-only test (driver, test module; not installed)" OFF)
if(KWPE_RENDER_ONLY_TEST)
  add_subdirectory(tests/render-only)
endif()
