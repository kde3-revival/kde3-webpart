# The part: a KDE module loaded by Konqueror (KParts::GenericFactory)
kde3_add_module(libkwpepart
  SOURCES
    wpex11platform.cpp
    kwpewidget.cpp
    kwpeclipboard.cpp
    kwpedownload.cpp
    kwpeadblock.cpp
    kwpeconfigdialog.cpp
    kwpesettings.cpp
    kwpepart.cpp
  MOC
    kwpewidget.h
    kwpedownload.h
    kwpeadblock.h
    kwpeconfigdialog.h
    kwpepart.h
  LINK
    KDE3::kparts
    KDE3::kutils
    PkgConfig::WPEWEBKIT
    PkgConfig::KWPEX11
)

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
