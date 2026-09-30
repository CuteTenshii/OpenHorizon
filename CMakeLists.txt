cmake_minimum_required(VERSION 3.21)

# vcpkg's FFmpeg find module passes empty keyword values to
# cmake_parse_arguments, which warns under the old behaviour this project's
# minimum implies. Set before project(), because vcpkg's find_package
# wrapper is defined while the toolchain loads there and keeps the policies
# in force at that point.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

project(fh1native VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# The game alone by default: tests and tools add about forty programs and
# need Catch2 and shaderc.
option(FH1N_BUILD_TESTS "Build the unit tests and the scenarios" OFF)
option(FH1N_BUILD_TOOLS "Build the command-line inspection tools" OFF)

# Every test program and tool links the whole engine with debug info, so the
# linker is a large share of a rebuild; mold does it several times faster
# than GNU ld. Builds without it fall back to the compiler's default linker.
if(NOT MSVC)
    find_program(FH1N_MOLD mold)
    if(FH1N_MOLD)
        add_link_options(-fuse-ld=mold)
    else()
        message(STATUS "mold not found: linking with the compiler's default linker")
    endif()
endif()

# Xbox 360 archive (LZX zip) and texture (xds) decoding. To build against a
# local checkout instead, configure with -DFETCHCONTENT_SOURCE_DIR_LZX=<path>
# or -DFETCHCONTENT_SOURCE_DIR_XDS=<path>.
include(FetchContent)
set(LZX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LZX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XDS_BUILD_QT_PLUGIN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lzx
    GIT_REPOSITORY https://git.tenshii.moe/Tenshii/lzx.git
    GIT_TAG 50561e5d4f2fd0be535b5b80743ffc62c426330a # v0.1.0
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(xds
    GIT_REPOSITORY https://git.tenshii.moe/Tenshii/xds.git
    GIT_TAG 4b629ce25372b98e9aa0e5923d6352ba2dc904cf
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(lzx xds)

# Dependencies come from the system on Linux (CMake packages where the
# distribution ships them, pkg-config otherwise) and from vcpkg on Windows
# (vcpkg.json, with CMAKE_TOOLCHAIN_FILE pointing at vcpkg). Each library
# is looked up as a CMake package first and through pkg-config second, and
# ends up as one target name the modules link against either way.
find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(glm REQUIRED CONFIG)
find_package(pugixml REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# vcpkg's SQLite ships its own package; CMake's module finds system ones.
find_package(unofficial-sqlite3 CONFIG QUIET)
if(TARGET unofficial::sqlite3::sqlite3)
    add_library(SQLite3::SQLite3 ALIAS unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 REQUIRED)
    # CMake 4.3 renamed the module's target to SQLite3::SQLite3 and kept
    # SQLite::SQLite3 as an alias; releases before it only define the old name.
    if(NOT TARGET SQLite3::SQLite3)
        add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
    endif()
endif()

# FFmpeg: libavcodec and libavutil for the audio decoders (XMA, MPEG),
# libavformat as well for the front end's ASF movies. FFmpeg has no CMake
# package of its own; vcpkg installs a find module that sets FFMPEG_*
# variables, system installs have pkg-config files.
find_package(FFMPEG QUIET)
if(FFMPEG_FOUND AND FFMPEG_libavformat_FOUND AND FFMPEG_libavcodec_FOUND)
    add_library(fh1n_ffmpeg INTERFACE)
    target_include_directories(fh1n_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(fh1n_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(fh1n_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
    add_library(FH1N::avcodec ALIAS fh1n_ffmpeg)
    add_library(FH1N::avformat ALIAS fh1n_ffmpeg)
else()
    if(NOT PKG_CONFIG_FOUND)
        message(FATAL_ERROR "FFmpeg not found: install its development files (with pkg-config) or use vcpkg")
    endif()
    pkg_check_modules(FH1N_AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)
    pkg_check_modules(FH1N_AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    add_library(FH1N::avcodec ALIAS PkgConfig::FH1N_AVCODEC)
    add_library(FH1N::avformat ALIAS PkgConfig::FH1N_AVFORMAT)
endif()

if(MSVC)
    # /permissive- for standard conformance, /utf-8 because the sources are
    # UTF-8 (the overlay's glyph list, the tests' names), /Zc:__cplusplus
    # so the macro reports C++20 as the headers expect.
    set(FH1N_WARNINGS /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    set(FH1N_WARNINGS -Wall -Wextra -Wpedantic -Wshadow)
endif()
if(WIN32)
    # windows.h otherwise defines min and max as macros, which break
    # std::min, std::max and numeric_limits in every file that includes it.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    # getenv, fopen and strerror are standard C++; the CRT's suggestions to
    # use its _s variants instead would make the code Windows-only.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory(src)

if(FH1N_BUILD_TESTS)
    find_package(Catch2 3 REQUIRED CONFIG)
    enable_testing()
    add_subdirectory(tests)
endif()

# Every Windows executable carries cmake/fh1-native.manifest, which makes
# UTF-8 the process code page, so text read from the command line, the
# environment or narrow-string file APIs is UTF-8 as on Linux. MSVC merges
# a .manifest source into the one its linker writes.
if(MSVC)
    function(fh1n_add_manifests directory)
        get_property(targets DIRECTORY ${directory} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "EXECUTABLE")
                target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/cmake/fh1-native.manifest)
            endif()
        endforeach()
        get_property(subdirectories DIRECTORY ${directory} PROPERTY SUBDIRECTORIES)
        foreach(subdirectory IN LISTS subdirectories)
            fh1n_add_manifests(${subdirectory})
        endforeach()
    endfunction()
    fh1n_add_manifests(${CMAKE_SOURCE_DIR}/src)
    if(FH1N_BUILD_TESTS)
        fh1n_add_manifests(${CMAKE_SOURCE_DIR}/tests)
    endif()
endif()

# Formatting and static analysis, run on demand: `cmake --build build
# --target format` rewrites the sources, `format-check` fails on any
# difference (for CI), `tidy` runs clang-tidy with .clang-tidy over every
# file in the compilation database.
find_program(CLANG_FORMAT clang-format)
find_program(RUN_CLANG_TIDY run-clang-tidy)
file(GLOB_RECURSE FH1N_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h
    ${CMAKE_SOURCE_DIR}/tools/*.cpp
    ${CMAKE_SOURCE_DIR}/tests/*.cpp ${CMAKE_SOURCE_DIR}/tests/*.h)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${FH1N_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting sources with clang-format"
        VERBATIM)
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${FH1N_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking source formatting"
        VERBATIM)
endif()
if(RUN_CLANG_TIDY)
    # Only the project's own files; fetched dependencies have their own style.
    add_custom_target(tidy
        COMMAND ${RUN_CLANG_TIDY} -quiet -p ${CMAKE_BINARY_DIR} "^${CMAKE_SOURCE_DIR}/(src|tools|tests)/"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy"
        VERBATIM)
endif()
