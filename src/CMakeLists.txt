add_library(openhorizon_base STATIC
    base/Bytes.cpp
    base/Bytes.h
    base/Crash.cpp
    base/Crash.h
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
target_include_directories(openhorizon_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(openhorizon_base PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_base PUBLIC lzx::lzx pugixml::pugixml Threads::Threads)
if(WIN32)
    # Host.cpp: SHGetKnownFolderPath and CommandLineToArgvW (shell32),
    # CoTaskMemFree (ole32). Crash.cpp: the stack walk, symbols and
    # minidump (dbghelp).
    target_link_libraries(openhorizon_base PRIVATE shell32 ole32 dbghelp)
endif()

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-vfs ../tools/openhorizon-vfs.cpp)
    target_compile_options(openhorizon-vfs PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-vfs PRIVATE openhorizon_base)
endif()

add_library(openhorizon_data STATIC
    data/Database.cpp
    data/Database.h
    data/StringTable.cpp
    data/StringTable.h
    car/CarPhysics.cpp
    car/CarPhysics.h
    car/CarSpec.cpp
    car/CarSpec.h
)
target_compile_options(openhorizon_data PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_data PUBLIC openhorizon_base SQLite3::SQLite3 glm::glm)

add_library(openhorizon_platform STATIC
    platform/ErrorDialog.cpp
    platform/ErrorDialog.h
    platform/Input.h
    platform/PadInput.cpp
    platform/PadInput.h
    platform/Window.cpp
    platform/Window.h
    render/Gl.cpp
    render/Gl.h
    render/GlProgram.cpp
    render/GlProgram.h
)
target_compile_options(openhorizon_platform PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_platform PUBLIC openhorizon_base SDL3::SDL3 OpenGL::GL)
# Gl.h declares the entry points from the Khronos glcorearb.h, which Linux
# distributions ship with their GL headers and the Windows SDK lacks (vcpkg's
# opengl-registry provides it there).
find_path(OPENHORIZON_GLCOREARB_INCLUDE_DIR GL/glcorearb.h REQUIRED)
target_include_directories(openhorizon_platform PUBLIC ${OPENHORIZON_GLCOREARB_INCLUDE_DIR})

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-carspec ../tools/openhorizon-carspec.cpp)
    target_compile_options(openhorizon-carspec PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-carspec PRIVATE openhorizon_data)

    add_executable(openhorizon-cartest ../tools/openhorizon-cartest.cpp)
    target_compile_options(openhorizon-cartest PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-cartest PRIVATE openhorizon_data)
endif()
add_subdirectory(world)

find_package(Freetype REQUIRED)

add_library(openhorizon_engine STATIC
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
    game/Cutscenes.cpp
    game/Cutscenes.h
    game/DrivingLine.cpp
    game/DrivingLine.h
    game/Flyers.cpp
    game/Flyers.h
    game/Game.cpp
    game/Game.h
    game/GameObjects.cpp
    game/GameObjects.h
    game/GameplaySounds.cpp
    game/GameplaySounds.h
    game/Outposts.cpp
    game/Outposts.h
    game/PostProcessing.cpp
    game/PostProcessing.h
    game/Race.cpp
    game/Race.h
    game/RaceData.cpp
    game/RaceData.h
    game/RenderScenario.cpp
    game/RenderScenario.h
    game/ScreenFade.cpp
    game/ScreenFade.h
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
    render/AnimatedObjectRenderer.cpp
    render/AnimatedObjectRenderer.h
    render/AnimatedObjectShaders.cpp
    render/AnimatedObjectShaders.h
    render/CarRenderer.cpp
    render/CarRenderer.h
    render/CloudDefs.cpp
    render/CloudDefs.h
    render/Clouds.cpp
    render/Clouds.h
    render/CrowdRenderer.cpp
    render/CrowdRenderer.h
    render/CubeMap.cpp
    render/CubeMap.h
    render/DrivingLineRenderer.cpp
    render/DrivingLineRenderer.h
    render/EnvironmentCube.cpp
    render/EnvironmentCube.h
    render/EnvironmentCubeMath.cpp
    render/EnvironmentCubeMath.h
    render/FrameRenderer.cpp
    render/FrameRenderer.h
    render/FrameStats.cpp
    render/FrameStats.h
    render/PresentTarget.cpp
    render/PresentTarget.h
    render/Overlay.cpp
    render/Overlay.h
    render/PostMath.cpp
    render/PostMath.h
    render/PostProcess.cpp
    render/PostProcess.h
    render/ScatterPass.cpp
    render/ScatterPass.h
    render/ShadowCascades.cpp
    render/ShadowCascades.h
    render/ShadowPass.cpp
    render/ShadowPass.h
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
target_compile_options(openhorizon_engine PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_engine PUBLIC openhorizon_platform openhorizon_data openhorizon_world openhorizon_collision openhorizon_audio openhorizon_carmodel openhorizon_ui openhorizon_radio openhorizon_particles openhorizon_profile openhorizon_animated openhorizon_spectators xds::xds Freetype::Freetype)
# The overlay finds its font through fontconfig; Windows has none, and the
# overlay looks in the system font folder there instead.
if(NOT WIN32)
    find_package(Fontconfig REQUIRED)
    target_link_libraries(openhorizon_engine PUBLIC Fontconfig::Fontconfig)
endif()

add_executable(openhorizon app/main.cpp)
target_compile_options(openhorizon PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon PRIVATE openhorizon_engine)
add_subdirectory(audio)
add_subdirectory(collision)
add_subdirectory(carmodel)
add_subdirectory(spectators)
add_subdirectory(xenos)
add_subdirectory(ui)
add_subdirectory(radio)
add_subdirectory(particles)
add_subdirectory(profile)
add_subdirectory(animated)
