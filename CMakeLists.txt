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
    GIT_TAG 5e3b4cf83ac9b841ae782c40bb9d368c884ce7c5
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(xds
    GIT_REPOSITORY https://git.tenshii.moe/Tenshii/xds.git
    GIT_TAG 4b629ce25372b98e9aa0e5923d6352ba2dc904cf
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(lzx xds)

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
