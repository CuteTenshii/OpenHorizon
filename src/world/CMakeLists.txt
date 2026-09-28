add_library(fh1world STATIC
    Archive.cpp
    Archive.h
    PvsFile.cpp
    PvsFile.h
    RenderMesh.cpp
    RenderMesh.h
    ScatterSet.cpp
    ScatterSet.h
    TrackPlacements.cpp
    TrackPlacements.h
    TrackTextures.cpp
    TrackTextures.h
    TrackWorld.cpp
    TrackWorld.h
    WorldIndex.cpp
    WorldIndex.h
    WorldTiles.cpp
    WorldTiles.h
    XboxTexture.cpp
    XboxTexture.h
    ZoneGrid.cpp
    ZoneGrid.h
)
target_compile_options(fh1world PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1world PUBLIC fh1base xds::xds glm::glm Threads::Threads)

# The library already owns the target name, so the tool's target has its own
# and only its output file is called fh1world.
add_executable(fh1world_tool ../../tools/fh1world.cpp)
set_target_properties(fh1world_tool PROPERTIES OUTPUT_NAME fh1world)
target_compile_options(fh1world_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1world_tool PRIVATE fh1world)
