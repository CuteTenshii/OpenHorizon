add_library(fh1carmodel STATIC
    CarBin.cpp
    CarBin.h
    CarEffect.cpp
    CarEffect.h
    CarMaterials.cpp
    CarMaterials.h
    CarModel.cpp
    CarModel.h
)
target_compile_options(fh1carmodel PRIVATE ${FH1N_WARNINGS})
# fh1world provides the texture mip chains the renderer uploads, fh1data the
# database the stock wheel and tyre sizes come from, fh1xenos the car
# effects translated to GLSL.
target_link_libraries(fh1carmodel PUBLIC fh1base fh1world fh1data fh1xenos xds::xds glm::glm)

if(FH1N_BUILD_TOOLS)
    # The library owns the target name, so the tool's target has its own and only
    # its output file is called fh1carmodel.
    add_executable(fh1carmodel_tool ../../tools/fh1carmodel.cpp)
    set_target_properties(fh1carmodel_tool PROPERTIES OUTPUT_NAME fh1carmodel)
    target_compile_options(fh1carmodel_tool PRIVATE ${FH1N_WARNINGS})
    # The tool's --render draws the car with the engine's renderers.
    target_link_libraries(fh1carmodel_tool PRIVATE fh1carmodel fh1engine)
endif()
