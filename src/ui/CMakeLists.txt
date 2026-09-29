add_library(fh1ui STATIC
    AnarkFormats.cpp
    AnarkFormats.h
    Hud.cpp
    Hud.h
    LoadingScreen.cpp
    LoadingScreen.h
    MenuCommon.cpp
    MenuCommon.h
    Minimap.cpp
    Minimap.h
    PauseMenu.cpp
    PauseMenu.h
    Presentation.cpp
    Presentation.h
    UiRenderer.cpp
    UiRenderer.h
    VectorFont.cpp
    VectorFont.h
    WorldMap.cpp
    WorldMap.h
)
target_compile_options(fh1ui PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1ui PUBLIC fh1base fh1platform fh1data xds::xds glm::glm)

# The library owns the target name, so the tool's target has its own and
# only its output file is called fh1ui.
add_executable(fh1ui_tool ../../tools/fh1ui.cpp)
set_target_properties(fh1ui_tool PROPERTIES OUTPUT_NAME fh1ui)
target_compile_options(fh1ui_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1ui_tool PRIVATE fh1ui)

add_executable(fh1menu ../../tools/fh1menu.cpp)
target_compile_options(fh1menu PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1menu PRIVATE fh1ui)
