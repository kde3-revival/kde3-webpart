# The part: a KDE module loaded by Konqueror (KParts::GenericFactory)
kde3_add_module(libkwpepart
  SOURCES
    wpex11platform.cpp
    kwpewidget.cpp
    kwpepart.cpp
  MOC
    kwpewidget.h
    kwpepart.h
  LINK
    KDE3::kparts
    PkgConfig::WPEWEBKIT
    PkgConfig::KWPEX11
)

install(FILES kwpepart.desktop DESTINATION ${KDE3_SERVICES_INSTALL_DIR})

# kwpeview URL: the part in a window of its own, for debugging
kde3_add_executable(kwpeview
  SOURCES
    kwpeview.cpp
  LINK
    KDE3::kparts
)
