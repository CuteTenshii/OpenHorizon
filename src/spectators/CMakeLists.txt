add_library(openhorizon_spectators STATIC
    CrowdAnimation.cpp
    CrowdAnimation.h
    CrowdDraw.cpp
    CrowdDraw.h
    CrowdPlacement.cpp
    CrowdPlacement.h
    CrowdSet.cpp
    CrowdSet.h
    SkinnedModel.cpp
    SkinnedModel.h
    SpectatorConfig.cpp
    SpectatorConfig.h
    SpectatorSprites.cpp
    SpectatorSprites.h
)
target_compile_options(openhorizon_spectators PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_spectators PUBLIC openhorizon_base openhorizon_world glm::glm)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-crowd ../../tools/openhorizon-crowd.cpp)
    target_compile_options(openhorizon-crowd PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-crowd PRIVATE openhorizon_spectators)
endif()
