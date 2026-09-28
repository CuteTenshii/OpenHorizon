add_library(fh1radio STATIC
    Radio.cpp
    Radio.h
    RadioAssets.cpp
    RadioAssets.h
    RadioData.cpp
    RadioData.h
    RadioRandom.h
    RadioSequencer.cpp
    RadioSequencer.h
    RadioText.cpp
    RadioText.h
)
target_compile_options(fh1radio PRIVATE ${FH1N_WARNINGS})
# fh1data provides the string tables the station names come from.
target_link_libraries(fh1radio PUBLIC fh1audio fh1base fh1data pugixml::pugixml Threads::Threads)

# The library already owns the target name, so the tool's target has its own
# and only its output file is called fh1radio.
add_executable(fh1radio_tool ../../tools/fh1radio.cpp)
set_target_properties(fh1radio_tool PROPERTIES OUTPUT_NAME fh1radio)
target_compile_options(fh1radio_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1radio_tool PRIVATE fh1radio)
