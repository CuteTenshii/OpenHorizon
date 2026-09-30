add_library(fh1collision STATIC
    CollisionIndex.cpp
    CollisionIndex.h
    FizTile.cpp
    FizTile.h
    SurfaceTable.cpp
    SurfaceTable.h
    TrackCollision.cpp
    TrackCollision.h
)
target_compile_options(fh1collision PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1collision PUBLIC fh1base fh1world glm::glm Threads::Threads)

if(FH1N_BUILD_TOOLS)
    add_executable(fh1fiz ../../tools/fh1fiz.cpp)
    target_compile_options(fh1fiz PRIVATE ${FH1N_WARNINGS})
    target_link_libraries(fh1fiz PRIVATE fh1collision)
endif()
