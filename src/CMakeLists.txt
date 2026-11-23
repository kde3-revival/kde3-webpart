# The scriptlet library, as a C string literal for kwpeadblock.cpp
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scriptlets.js)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/scriptlets.js KWPE_SCRIPTLETS_JS)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/scriptlets.js.h.new "R\"KWPEJS(${KWPE_SCRIPTLETS_JS})KWPEJS\"\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/scriptlets.js.h.new ${CMAKE_CURRENT_BINARY_DIR}/scriptlets.js.h COPYONLY)

# The part: a KDE module loaded by Konqueror (KParts::GenericFactory)
kde3_add_module(libkwpepart
  SOURCES
    wpex11platform.cpp
    kwpewidget.cpp
    kwpemailsanitizer.cpp
    kwpeclipboard.cpp
    kwpedialogs.cpp
    kwpedownload.cpp
    kwpeadblock.cpp
    kwpeconfigdialog.cpp
    kwpesettings.cpp
    kwpepart.cpp
  MOC
    kwpewidget.h
    kwpedialogs.h
    kwpedownload.h
    kwpeadblock.h
    kwpeconfigdialog.h
    kwpepart.h
  LINK
    KDE3::kparts
    KDE3::kutils
    PkgConfig::WPEWEBKIT
    PkgConfig::GUMBO
    PkgConfig::KWPEX11
)
target_include_directories(libkwpepart PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

install(FILES kwpepart.desktop DESTINATION ${KDE3_SERVICES_INSTALL_DIR})
install(FILES kwpepart.rc DESTINATION ${KDE3_DATA_INSTALL_DIR}/kwpepart)

# kwpeview URL: the part in a window of its own, for debugging
kde3_add_executable(kwpeview
  SOURCES
    kwpeview.cpp
  LINK
    KDE3::kparts
)

# kwpe-filterconv: filter lists -> compiled WebKit content filters, run
# by the part in the background (no Qt, no KDE)
kde3_add_executable(kwpe-filterconv
  SOURCES
    abpconverter.cpp
    kwpe-filterconv.cpp
  LINK
    PkgConfig::WPEWEBKIT
)

# abptest: the converter on tricky filters (not installed; run it from
# the build directory)
kde3_add_executable(abptest
  SOURCES
    abpconverter.cpp
    ../tests/adblock/abptest.cpp
  EXCEPTIONS
  NO_INSTALL
)
target_include_directories(abptest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# kwpe-render-only: the mail mode's render-only test driver
# (tests/render-only; not installed)
kde3_add_executable(kwpe-render-only
  SOURCES
    ../tests/render-only/driver.cpp
  MOC
    ../tests/render-only/driver.h
  LINK
    KDE3::kparts
  NO_INSTALL
)

# kwpe-sanitize: the mail mode's sanitizer on stdin (tests/render-only)
kde3_add_executable(kwpe-sanitize
  SOURCES
    kwpemailsanitizer.cpp
    ../tests/render-only/sanitize.cpp
  LINK
    PkgConfig::GUMBO
  EXCEPTIONS
  NO_INSTALL
)
target_include_directories(kwpe-sanitize PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
