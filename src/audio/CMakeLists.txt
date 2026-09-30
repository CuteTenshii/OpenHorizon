add_library(openhorizon_audio STATIC
    AudioDevice.cpp
    AudioDevice.h
    AudioLibrary.cpp
    AudioLibrary.h
    CollisionSounds.cpp
    CollisionSounds.h
    CrashMix.cpp
    CrashMix.h
    Decode.cpp
    Decode.h
    Dsp.cpp
    Dsp.h
    EngineSound.cpp
    EngineSound.h
    EngineTuning.cpp
    EngineTuning.h
    EventPlayer.cpp
    EventPlayer.h
    Fev.cpp
    Fev.h
    Fsb.cpp
    Fsb.h
    LeReader.h
    Mixer.cpp
    Mixer.h
    MixSnapshots.cpp
    MixSnapshots.h
    TyreSound.cpp
    TyreSound.h
    Wav.cpp
    Wav.h
)
target_compile_options(openhorizon_audio PRIVATE ${OPENHORIZON_WARNINGS})
target_link_libraries(openhorizon_audio PUBLIC openhorizon_base OpenHorizon::avcodec SDL3::SDL3 Threads::Threads glm::glm)

if(OPENHORIZON_BUILD_TOOLS)
    add_executable(openhorizon-audio ../../tools/openhorizon-audio.cpp)
    target_compile_options(openhorizon-audio PRIVATE ${OPENHORIZON_WARNINGS})
    target_link_libraries(openhorizon-audio PRIVATE openhorizon_audio)
endif()
