#include "AudioDevice.h"

#include "base/Log.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace openhorizon::audio {

AudioDevice::AudioDevice()
    : m_mixer(kSampleRate)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        throw DataError("cannot start SDL audio: {}", SDL_GetError());
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = kChannels;
    spec.freq = kSampleRate;
    m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioDevice::feed, this);
    if (!m_stream) {
        const std::string error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw DataError("cannot open the audio device: {}", error);
    }
    if (!SDL_ResumeAudioStreamDevice(m_stream)) {
        const std::string error = SDL_GetError();
        SDL_DestroyAudioStream(m_stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw DataError("cannot start the audio device: {}", error);
    }
    logInfo("audio: playing on '{}' at {} Hz", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
        kSampleRate);
}

AudioDevice::AudioDevice(Offline)
    : m_mixer(kSampleRate)
{
}

AudioDevice::~AudioDevice()
{
    if (m_stream) {
        // Destroying the stream stops the callback before the mixer goes.
        SDL_DestroyAudioStream(m_stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

std::vector<float> AudioDevice::render(std::size_t frames)
{
    if (isLive()) {
        throw DataError("render() is only for offline audio devices");
    }
    std::vector<float> out(frames * kChannels);
    m_mixer.render(out);
    return out;
}

void AudioDevice::feed(void* self, SDL_AudioStream* stream, int additional, int /*total*/)
{
    auto* device = static_cast<AudioDevice*>(self);
    const int frameBytes = static_cast<int>(sizeof(float)) * kChannels;
    int frames = (additional + frameBytes - 1) / frameBytes;
    while (frames > 0) {
        const int chunk = std::min(frames, static_cast<int>(Mixer::kBlockFrames) * 4);
        device->m_scratch.resize(static_cast<std::size_t>(chunk) * kChannels);
        device->m_mixer.render(device->m_scratch);
        SDL_PutAudioStreamData(stream, device->m_scratch.data(), chunk * frameBytes);
        frames -= chunk;
    }
}

} // namespace openhorizon::audio
