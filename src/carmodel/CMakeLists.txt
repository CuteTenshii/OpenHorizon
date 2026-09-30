add_library(openhorizon_carmodel STATIC
    CarBin.cpp
    CarBin.h
    CarEffect.cpp
    CarEffect.h
    CarMaterials.cpp
    CarMaterials.h
    CarModel.cpp
    CarModel.h
)
target_compile_options(openhorizon_carmodel PRIVATE ${OPENHORIZON_WARNINGS})
# openhorizon_world provides the texture mip chains the renderer uploads, openhorizon_data the
# database the stock wheel and tyre sizes come from, openhorizon_xenos the car
# effects translated to GLSL.
target_link_libraries(openhorizon_carmodel PUBLIC openhorizon_base openhorizon_world openhorizon_data openhorizon_xenos xds::xds glm::glm)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-carmodel ../../tools/openhorizon-carmodel.cpp)
    target_compile_options(openhorizon-carmodel PRIVATE ${OPENHORIZON_WARNINGS})
    # The tool's --render draws the car with the engine's renderers.
    target_link_libraries(openhorizon-carmodel PRIVATE openhorizon_carmodel openhorizon_engine)
endif()
