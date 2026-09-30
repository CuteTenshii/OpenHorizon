add_library(openhorizon_ui STATIC
    AnarkFormats.cpp
    AnarkFormats.h
    EventScreens.cpp
    EventScreens.h
    Hud.cpp
    Hud.h
    LoadingScreen.cpp
    LoadingScreen.h
    MapCoverage.cpp
    MapCoverage.h
    MenuCommon.cpp
    MenuCommon.h
    Minimap.cpp
    Minimap.h
    MovieRenderer.cpp
    MovieRenderer.h
    Navigation.cpp
    Navigation.h
    PauseMenu.cpp
    PauseMenu.h
    PlayerBar.cpp
    PlayerBar.h
    Presentation.cpp
    Presentation.h
    SuperStacker.cpp
    SuperStacker.h
    TitleScreen.cpp
    TitleScreen.h
    UiSounds.cpp
    UiSounds.h
    UiRenderer.cpp
    UiRenderer.h
    VectorFont.cpp
    VectorFont.h
    WorldMap.cpp
    WorldMap.h
)
target_compile_options(openhorizon_ui PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_ui PUBLIC openhorizon_base openhorizon_platform openhorizon_data xds::xds glm::glm)
# The front end's movies are ASF files, which libavformat demuxes.
target_link_libraries(openhorizon_ui PUBLIC openhorizon_audio OpenHorizon::avformat OpenHorizon::avcodec)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-ui ../../tools/openhorizon-ui.cpp)
    target_compile_options(openhorizon-ui PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-ui PRIVATE openhorizon_ui)

    add_executable(openhorizon-menu ../../tools/openhorizon-menu.cpp)
    target_compile_options(openhorizon-menu PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-menu PRIVATE openhorizon_ui)
endif()
