add_library(openhorizon_collision STATIC
    CollisionIndex.cpp
    CollisionIndex.h
    FizTile.cpp
    FizTile.h
    SurfaceTable.cpp
    SurfaceTable.h
    TrackCollision.cpp
    TrackCollision.h
)
target_compile_options(openhorizon_collision PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_collision PUBLIC openhorizon_base openhorizon_world glm::glm Threads::Threads)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-fiz ../../tools/openhorizon-fiz.cpp)
    target_compile_options(openhorizon-fiz PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-fiz PRIVATE openhorizon_collision)
endif()
