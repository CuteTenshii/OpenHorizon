#pragma once

#include "Mixer.h"

#include <cstddef>
#include <vector>

struct SDL_AudioStream;

namespace fh::audio {

/// The game's audio output: a Mixer at 48 kHz stereo float, either playing
/// through the default SDL3 playback device or rendered on demand without
/// any device (for tools and tests).
class AudioDevice {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

    struct Offline {};

    /// Opens the default playback device and starts pulling audio from the
    /// mixer on SDL's audio thread. Throws DataError if SDL cannot open it.
    AudioDevice();
    /// A device with no output; call render() to advance it.
    explicit AudioDevice(Offline);
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    Mixer& mixer() { return m_mixer; }
    const Mixer& mixer() const { return m_mixer; }
    bool isLive() const { return m_stream != nullptr; }

    /// Mixes `frames` stereo frames and returns them interleaved. Only for
    /// offline devices; a live device's output belongs to SDL, so this
    /// throws DataError there.
    std::vector<float> render(std::size_t frames);

private:
    static void feed(void* self, SDL_AudioStream* stream, int additional, int total);

    Mixer m_mixer;
    SDL_AudioStream* m_stream = nullptr;
    std::vector<float> m_scratch;
};

} // namespace fh::audio
