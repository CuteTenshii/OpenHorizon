add_library(fh1base STATIC
    base/Bytes.cpp
    base/Bytes.h
    base/Error.h
    base/JobPool.cpp
    base/JobPool.h
    base/Settings.cpp
    base/Settings.h
    base/Log.cpp
    base/Log.h
    vfs/Vfs.cpp
    vfs/Vfs.h
)
target_include_directories(fh1base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fh1base PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1base PUBLIC lzx::lzx pugixml::pugixml Threads::Threads)

add_executable(fh1vfs ../tools/fh1vfs.cpp)
target_compile_options(fh1vfs PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1vfs PRIVATE fh1base)

add_library(fh1data STATIC
    data/Database.cpp
    data/Database.h
    data/StringTable.cpp
    data/StringTable.h
    car/CarPhysics.cpp
    car/CarPhysics.h
    car/CarSpec.cpp
    car/CarSpec.h
)
target_compile_options(fh1data PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1data PUBLIC fh1base SQLite3::SQLite3 glm::glm)

add_library(fh1platform STATIC
    platform/Input.h
    platform/Window.cpp
    platform/Window.h
    render/Gl.cpp
    render/Gl.h
)
target_compile_options(fh1platform PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1platform PUBLIC fh1base SDL3::SDL3 OpenGL::GL)

add_executable(fh1carspec ../tools/fh1carspec.cpp)
target_compile_options(fh1carspec PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1carspec PRIVATE fh1data)

add_executable(fh1cartest ../tools/fh1cartest.cpp)
target_compile_options(fh1cartest PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1cartest PRIVATE fh1data)
add_subdirectory(world)

find_package(Freetype REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

add_library(fh1engine STATIC
    game/AiDriver.cpp
    game/AiDriver.h
    game/Game.cpp
    game/Game.h
    game/GameObjects.cpp
    game/GameObjects.h
    game/Race.cpp
    game/Race.h
    game/RaceData.cpp
    game/RaceData.h
    game/Smashables.cpp
    game/Smashables.h
    game/TimeOfDay.cpp
    game/TimeOfDay.h
    physics/WorldCollision.cpp
    physics/WorldCollision.h
    render/CarRenderer.cpp
    render/CarRenderer.h
    render/FrameRenderer.cpp
    render/FrameRenderer.h
    render/Overlay.cpp
    render/Overlay.h
    render/Shader.cpp
    render/Shader.h
    render/Texture.cpp
    render/Texture.h
    render/WorldRenderer.cpp
    render/WorldRenderer.h
)
target_compile_options(fh1engine PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1engine PUBLIC fh1platform fh1data fh1world fh1collision fh1audio fh1carmodel xds::xds Freetype::Freetype PkgConfig::FONTCONFIG)

add_executable(fh1 app/main.cpp)
target_compile_options(fh1 PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1 PRIVATE fh1engine)
add_subdirectory(audio)
add_subdirectory(collision)
add_subdirectory(carmodel)
