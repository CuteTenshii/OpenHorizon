add_library(openhorizon_world STATIC
    Archive.cpp
    Archive.h
    PvsFile.cpp
    PvsFile.h
    RenderMesh.cpp
    RenderMesh.h
    ScatterSet.cpp
    ScatterSet.h
    ShadedTiles.cpp
    ShadedTiles.h
    TrackPlacements.cpp
    TrackPlacements.h
    TrackSettings.cpp
    TrackSettings.h
    TrackShaders.cpp
    TrackShaders.h
    TrackTextures.cpp
    TrackTextures.h
    TrackWorld.cpp
    TrackWorld.h
    WorldIndex.cpp
    WorldIndex.h
    WorldTiles.cpp
    WorldTiles.h
    XboxCube.cpp
    XboxCube.h
    XboxTexture.cpp
    XboxTexture.h
    ZoneGrid.cpp
    ZoneGrid.h
)
target_compile_options(openhorizon_world PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_world PUBLIC openhorizon_base openhorizon_xenos xds::xds glm::glm Threads::Threads)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-world ../../tools/openhorizon-world.cpp)
    target_compile_options(openhorizon-world PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-world PRIVATE openhorizon_world)
endif()
