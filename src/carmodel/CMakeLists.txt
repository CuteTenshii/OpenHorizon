add_library(fh1carmodel STATIC
    CarBin.cpp
    CarBin.h
    CarMaterials.cpp
    CarMaterials.h
    CarModel.cpp
    CarModel.h
)
target_compile_options(fh1carmodel PRIVATE ${FH1N_WARNINGS})
# fh1world provides the texture mip chains the renderer uploads, fh1data the
# database the stock wheel and tyre sizes come from.
target_link_libraries(fh1carmodel PUBLIC fh1base fh1world fh1data xds::xds glm::glm)

# The library owns the target name, so the tool's target has its own and only
# its output file is called fh1carmodel.
add_executable(fh1carmodel_tool ../../tools/fh1carmodel.cpp)
set_target_properties(fh1carmodel_tool PROPERTIES OUTPUT_NAME fh1carmodel)
target_compile_options(fh1carmodel_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1carmodel_tool PRIVATE fh1carmodel)
