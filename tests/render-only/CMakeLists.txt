# The render-only test (-DKWPE_RENDER_ONLY_TEST=ON; nothing installed).
set(_src ${PROJECT_SOURCE_DIR}/src)

# The part again, with KWPE_MAIL_TESTING: the test may switch layers off
kde3_add_module(libkwpepart_test
  SOURCES
    ${_src}/wpex11platform.cpp
    ${_src}/kwpewidget.cpp
    ${_src}/kwpemailsanitizer.cpp
    ${_src}/kwpeclipboard.cpp
    ${_src}/kwpedialogs.cpp
    ${_src}/kwpedownload.cpp
    ${_src}/kwpeadblock.cpp
    ${_src}/kwpeconfigdialog.cpp
    ${_src}/kwpesettings.cpp
    ${_src}/kwpepart.cpp
  MOC
    ${_src}/kwpewidget.h
    ${_src}/kwpedialogs.h
    ${_src}/kwpedownload.h
    ${_src}/kwpeadblock.h
    ${_src}/kwpeconfigdialog.h
    ${_src}/kwpepart.h
  LINK
    KDE3::kparts
    KDE3::kutils
    PkgConfig::WPEWEBKIT
    PkgConfig::GUMBO
    PkgConfig::KWPEX11
  NO_INSTALL
)
target_compile_definitions(libkwpepart_test PRIVATE KWPE_MAIL_TESTING)
target_include_directories(libkwpepart_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${PROJECT_BINARY_DIR}/src ${_src})
add_dependencies(libkwpepart_test libkwpepart)

# kwpe-render-only: the driver
kde3_add_executable(kwpe-render-only
  SOURCES
    driver.cpp
  MOC
    driver.h
  LINK
    KDE3::kparts
  NO_INSTALL
)

# kwpe-sanitize: the first layer alone
kde3_add_executable(kwpe-sanitize
  SOURCES
    ${_src}/kwpemailsanitizer.cpp
    sanitize.cpp
  LINK
    PkgConfig::GUMBO
  EXCEPTIONS
  NO_INSTALL
)
target_include_directories(kwpe-sanitize PRIVATE ${_src})
