add_library(fh1animated STATIC
    AnimatedObjects.cpp
    AnimatedObjects.h
    AnimatedSets.cpp
    AnimatedSets.h
    Granny.cpp
    Granny.h
)
target_include_directories(fh1animated PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(fh1animated PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1animated PUBLIC fh1base fh1world glm::glm)
