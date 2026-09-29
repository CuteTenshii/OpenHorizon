add_library(fh1base STATIC
    base/Bytes.cpp
    base/Bytes.h
    base/Error.h
    base/Host.cpp
    base/Host.h
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
if(WIN32)
    # Host.cpp: SHGetKnownFolderPath (shell32) and CoTaskMemFree (ole32).
    target_link_libraries(fh1base PRIVATE shell32 ole32)
endif()

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
    render/GlProgram.cpp
    render/GlProgram.h
)
target_compile_options(fh1platform PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1platform PUBLIC fh1base SDL3::SDL3 OpenGL::GL)
# Gl.h declares the entry points from the Khronos glcorearb.h, which Linux
# distributions ship with their GL headers and the Windows SDK lacks (vcpkg's
# opengl-registry provides it there).
find_path(FH1N_GLCOREARB_INCLUDE_DIR GL/glcorearb.h REQUIRED)
target_include_directories(fh1platform PUBLIC ${FH1N_GLCOREARB_INCLUDE_DIR})

add_executable(fh1carspec ../tools/fh1carspec.cpp)
target_compile_options(fh1carspec PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1carspec PRIVATE fh1data)

add_executable(fh1cartest ../tools/fh1cartest.cpp)
target_compile_options(fh1cartest PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1cartest PRIVATE fh1data)
add_subdirectory(world)

find_package(Freetype REQUIRED)

add_library(fh1engine STATIC
    game/ActivationFlares.cpp
    game/ActivationFlares.h
    game/Activities.cpp
    game/Activities.h
    game/ActivityProps.cpp
    game/ActivityProps.h
    game/AiDriver.cpp
    game/AiDriver.h
    game/CameraSettings.cpp
    game/CameraSettings.h
    game/DrivingLine.cpp
    game/DrivingLine.h
    game/Flyers.cpp
    game/Flyers.h
    game/Game.cpp
    game/Game.h
    game/GameObjects.cpp
    game/GameObjects.h
    game/Outposts.cpp
    game/Outposts.h
    game/Race.cpp
    game/Race.h
    game/RaceData.cpp
    game/RaceData.h
    game/Smashables.cpp
    game/Smashables.h
    game/SpeedTraps.cpp
    game/SpeedTraps.h
    game/TimeOfDay.cpp
    game/TimeOfDay.h
    game/Title.cpp
    game/Title.h
    game/TrafficConfig.cpp
    game/TrafficConfig.h
    physics/WorldCollision.cpp
    physics/WorldCollision.h
    render/CarRenderer.cpp
    render/CarRenderer.h
    render/CubeMap.cpp
    render/CubeMap.h
    render/DrivingLineRenderer.cpp
    render/DrivingLineRenderer.h
    render/FrameRenderer.cpp
    render/FrameRenderer.h
    render/FrameStats.cpp
    render/FrameStats.h
    render/PresentTarget.cpp
    render/PresentTarget.h
    render/Overlay.cpp
    render/Overlay.h
    render/ScatterPass.cpp
    render/ScatterPass.h
    render/Shader.cpp
    render/Shader.h
    render/SkyObjects.cpp
    render/SkyObjects.h
    render/Texture.cpp
    render/Texture.h
    render/TrackPrograms.cpp
    render/TrackPrograms.h
    render/WorldRenderer.cpp
    render/WorldRenderer.h
    render/XenosProgram.cpp
    render/XenosProgram.h
)
target_compile_options(fh1engine PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1engine PUBLIC fh1platform fh1data fh1world fh1collision fh1audio fh1carmodel fh1ui fh1radio fh1particles fh1profile xds::xds Freetype::Freetype)
# The overlay finds its font through fontconfig; Windows has none, and the
# overlay looks in the system font folder there instead.
if(NOT WIN32)
    find_package(Fontconfig REQUIRED)
    target_link_libraries(fh1engine PUBLIC Fontconfig::Fontconfig)
endif()

add_executable(fh1 app/main.cpp)
target_compile_options(fh1 PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1 PRIVATE fh1engine)
add_subdirectory(audio)
add_subdirectory(collision)
add_subdirectory(carmodel)
add_subdirectory(spectators)
add_subdirectory(xenos)
add_subdirectory(ui)
add_subdirectory(radio)
add_subdirectory(particles)
add_subdirectory(profile)
