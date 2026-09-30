add_library(openhorizon_particles STATIC
    Effect.cpp
    Effect.h
    GameRandom.h
    ParticleEmitters.cpp
    ParticleEmitters.h
    ParticleManager.cpp
    ParticleManager.h
    ParticleRenderer.cpp
    ParticleRenderer.h
    ParticleShaders.cpp
    ParticleShaders.h
    ParticleSystem.cpp
    ParticleSystem.h
    SurfaceEffects.cpp
    SurfaceEffects.h
    TyreEffects.cpp
    TyreEffects.h
)
target_include_directories(openhorizon_particles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(openhorizon_particles PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_particles PUBLIC openhorizon_base openhorizon_platform openhorizon_world glm::glm)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-particles ../../tools/openhorizon-particles.cpp)
    target_compile_options(openhorizon-particles PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-particles PRIVATE openhorizon_particles openhorizon_engine xds::xds)
endif()
