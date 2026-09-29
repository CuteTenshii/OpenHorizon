add_library(fh1xenos STATIC
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
target_include_directories(fh1xenos PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(fh1xenos PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1xenos PUBLIC fh1base)

# GLSL validation through glslang (shaderc) for the tool and the tests; kept
# out of fh1xenos so the engine does not depend on shaderc.
# vcpkg's shaderc is a CMake package; system installs have pkg-config files.
find_package(unofficial-shaderc CONFIG QUIET)
if(TARGET unofficial::shaderc::shaderc)
    add_library(FH1N::shaderc ALIAS unofficial::shaderc::shaderc)
else()
    pkg_check_modules(SHADERC REQUIRED IMPORTED_TARGET shaderc)
    add_library(FH1N::shaderc ALIAS PkgConfig::SHADERC)
endif()
add_library(fh1xenoscheck STATIC
    ShaderCheck.cpp
    ShaderCheck.h
)
target_compile_options(fh1xenoscheck PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1xenoscheck PUBLIC fh1xenos FH1N::shaderc)

add_executable(fh1shader ../../tools/fh1shader.cpp)
target_compile_options(fh1shader PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1shader PRIVATE fh1xenoscheck)
