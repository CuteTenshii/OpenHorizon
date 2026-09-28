cmake_minimum_required(VERSION 3.21)

project(fh1native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(FH1N_BUILD_TESTS "Build the unit tests" ON)

# The archive and texture libraries live next to this project by default; any
# checkout can be used instead.
set(FH1N_LZX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lzx" CACHE PATH "Source directory of the lzx library")
set(FH1N_XDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xds" CACHE PATH "Source directory of the xds library")

set(LZX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LZX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_QT_PLUGIN OFF CACHE BOOL "" FORCE)
add_subdirectory(${FH1N_LZX_DIR} ${CMAKE_BINARY_DIR}/_deps/lzx EXCLUDE_FROM_ALL)
add_subdirectory(${FH1N_XDS_DIR} ${CMAKE_BINARY_DIR}/_deps/xds EXCLUDE_FROM_ALL)

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(glm REQUIRED CONFIG)
find_package(pugixml REQUIRED CONFIG)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswresample)

set(FH1N_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)

add_subdirectory(src)

if(FH1N_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
