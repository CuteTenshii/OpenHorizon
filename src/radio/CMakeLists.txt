add_library(openhorizon_radio STATIC
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
target_compile_options(openhorizon_radio PRIVATE ${OPENHORIZON_WARNINGS})
# openhorizon_data provides the string tables the station names come from.
target_link_libraries(openhorizon_radio PUBLIC openhorizon_audio openhorizon_base openhorizon_data pugixml::pugixml Threads::Threads)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-radio ../../tools/openhorizon-radio.cpp)
    target_compile_options(openhorizon-radio PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-radio PRIVATE openhorizon_radio)
endif()
