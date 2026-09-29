add_library(fh1audio STATIC
    AudioDevice.cpp
    AudioDevice.h
    AudioLibrary.cpp
    AudioLibrary.h
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
target_compile_options(fh1audio PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1audio PUBLIC fh1base PkgConfig::FFMPEG SDL3::SDL3 Threads::Threads glm::glm)

# The library already owns the target name, so the tool's target has its own
# and only its output file is called fh1audio.
add_executable(fh1audio_tool ../../tools/fh1audio.cpp)
set_target_properties(fh1audio_tool PROPERTIES OUTPUT_NAME fh1audio)
target_compile_options(fh1audio_tool PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1audio_tool PRIVATE fh1audio)
