add_library(openhorizon_xenos STATIC
    Disassembler.cpp
    Disassembler.h
    Effect.cpp
    Effect.h
    GlslTranslator.cpp
    GlslTranslator.h
    Microcode.cpp
    Microcode.h
    ShaderProgram.cpp
    ShaderProgram.h
)
target_include_directories(openhorizon_xenos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(openhorizon_xenos PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_xenos PUBLIC openhorizon_base)

# GLSL validation through glslang (shaderc), for the tool and the tests only,
# so neither the engine nor a game-only build needs shaderc.
# vcpkg's shaderc is a CMake package; system installs have pkg-config files.
if(OPENHORIZON_BUILD_TOOLS OR OPENHORIZON_BUILD_TESTS)
    find_package(unofficial-shaderc CONFIG QUIET)
    if(TARGET unofficial::shaderc::shaderc)
        add_library(OpenHorizon::shaderc ALIAS unofficial::shaderc::shaderc)
    else()
        pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
        add_library(OpenHorizon::shaderc ALIAS PkgConfig::SHADERC)
    endif()
    add_library(openhorizon_xenoscheck STATIC
        ShaderCheck.cpp
        ShaderCheck.h
    )
    target_compile_options(openhorizon_xenoscheck PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon_xenoscheck PUBLIC openhorizon_xenos OpenHorizon::shaderc)
endif()

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-shader ../../tools/openhorizon-shader.cpp)
    target_compile_options(openhorizon-shader PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-shader PRIVATE openhorizon_xenoscheck)
endif()
