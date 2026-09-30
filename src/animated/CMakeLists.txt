add_library(openhorizon_animated STATIC
    AnimatedObjects.cpp
    AnimatedObjects.h
    AnimatedSets.cpp
    AnimatedSets.h
    Granny.cpp
    Granny.h
)
target_include_directories(openhorizon_animated PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(openhorizon_animated PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_animated PUBLIC openhorizon_base openhorizon_world glm::glm)
