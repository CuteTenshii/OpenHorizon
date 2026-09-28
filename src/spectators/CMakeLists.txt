add_library(fh1spectators STATIC
    CrowdAnimation.cpp
    CrowdAnimation.h
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
target_compile_options(fh1spectators PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1spectators PUBLIC fh1base fh1world glm::glm)

add_executable(fh1crowd ../../tools/fh1crowd.cpp)
target_compile_options(fh1crowd PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1crowd PRIVATE fh1spectators)
