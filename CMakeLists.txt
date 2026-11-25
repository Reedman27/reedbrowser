# SPDX-License-Identifier: AGPL-3.0-only
cmake_minimum_required(VERSION 3.20)
project(ReedBrowser VERSION 0.5.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(SQLite3 REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkitgtk-6.0)
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)

# Embed the in-page browser.* implementation as a C++ raw string.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/api.js REED_API_JS)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/ext/api_js.h
  "// Generated from src/ext/api.js - do not edit.\n#pragma once\nstatic const char kReedApiJs[] = R\"REEDJS(${REED_API_JS})REEDJS\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/api.js)

add_executable(reedbrowser src/main.cpp src/extensions.cpp)
target_include_directories(reedbrowser PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(reedbrowser PRIVATE PkgConfig::GTK4 PkgConfig::WEBKIT PkgConfig::JSONGLIB PkgConfig::LIBARCHIVE PkgConfig::CURL SQLite::SQLite3)
target_compile_options(reedbrowser PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
target_compile_definitions(reedbrowser PRIVATE REEDBROWSER_VERSION="${PROJECT_VERSION}" REEDBROWSER_EXT_VERSION="${PROJECT_VERSION}")

# Unit tests (ctest). Both are standalone programs; the DNR one also asks WebKit to compile the rules.
include(CTest)
if(BUILD_TESTING)
  add_executable(test-dnr tools/test-dnr.cpp)
  target_include_directories(test-dnr PRIVATE src)
  target_link_libraries(test-dnr PRIVATE PkgConfig::WEBKIT PkgConfig::JSONGLIB)
  add_test(NAME release-validation COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate-release.py WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME dnr-conversion COMMAND test-dnr)
  # tools/test-importer.cpp needs sample Chromium/Firefox/HTML bookmark files passed as argv[1]; not registered yet.
endif()

install(TARGETS reedbrowser RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/reedbrowser)
install(FILES packaging/reedbrowser.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
