add_library(fh1particles STATIC
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
target_include_directories(fh1particles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(fh1particles PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1particles PUBLIC fh1base fh1platform fh1world glm::glm)

add_executable(fh1particles_tool ../../tools/fh1particles.cpp)
set_target_properties(fh1particles_tool PROPERTIES OUTPUT_NAME fh1particles)
target_compile_options(fh1particles_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1particles_tool PRIVATE fh1particles fh1engine xds::xds)
