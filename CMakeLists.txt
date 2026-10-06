cmake_minimum_required(VERSION 3.16)
project(wpe-url-extractor C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

find_package(PkgConfig REQUIRED)

pkg_check_modules(WPE_WEBKIT REQUIRED IMPORTED_TARGET wpe-webkit-2.0)
pkg_check_modules(WPE_FDO REQUIRED IMPORTED_TARGET wpebackend-fdo-1.0)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-1.0)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)

set(SOURCES
    src/main.cpp
    src/browser.cpp
    src/extractor.cpp
)

set(HEADERS
    src/browser.hpp
    src/extractor.hpp
)

add_executable(wpe-url-extractor ${SOURCES} ${HEADERS})

target_link_libraries(wpe-url-extractor PRIVATE
    PkgConfig::WPE_WEBKIT
    PkgConfig::WPE_FDO
    PkgConfig::WPE
    PkgConfig::GLIB
    PkgConfig::SOUP
)

target_include_directories(wpe-url-extractor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
