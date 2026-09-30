#include "Radio.h"

#include "RadioAssets.h"
#include "RadioText.h"

#include "audio/AudioDevice.h"
#include "audio/Dsp.h"
#include "base/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace openhorizon::radio {

namespace {

/// Frames the worker produces at a time.
constexpr std::size_t kChunkFrames = 480;
/// Frames decoded per request to a stream.
constexpr std::size_t kDecodeFrames = 4096;
/// The game's dialogue level check looks at this many samples of the DJ's
/// stream (the 256-entry wave data read at 0x82c9a1bc).
constexpr std::size_t kSpeechWindow = 256;

} // namespace

/// Decodes one of the sequencer's voices as it plays.
struct VoiceDecoder {
    std::uint64_t instance = 0;
    std::optional<audio::MpegSampleStream> stream;
    std::vector<float> buffer;
    /// Sample frame of buffer[0].
    std::size_t first = 0;
    std::size_t channels = 1;

    void reset() { *this = VoiceDecoder{}; }

    std::size_t end() const { return first + buffer.size() / channels; }

    /// One channel of one sample frame; silence outside the decoded range.
    float at(std::size_t frame, std::size_t channel)
    {
        while (stream && frame >= end() && !stream->finished()) {
            try {
                stream->decode(kDecodeFrames, buffer);
            } catch (const std::exception& e) {
                logWarning("radio: {}", e.what());
                stream.reset();
            }
        }
        if (frame < first || frame >= end()) {
            return 0.0F;
        }
        return buffer[(frame - first) * channels + std::min(channel, channels - 1)];
    }

    /// Frees audio before `frame` once enough has piled up.
    void release(std::size_t frame)
    {
        constexpr std::size_t kKeep = 8;
        constexpr std::size_t kSlack = 65536;
        if (frame > first + kKeep + kSlack) {
            const std::size_t drop = std::min(frame - kKeep - first, buffer.size() / channels);
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(drop * channels));
            first += drop;
        }
    }
};

struct Radio::Impl {
    Impl(std::shared_ptr<const RadioAssets> a, std::uint32_t seed, bool threaded)
        : assets(std::move(a))
        , sequencer(assets->catalog(), seed)
        , useWorker(threaded)
    {
    }

    std::shared_ptr<const RadioAssets> assets;
    RadioSequencer sequencer;
    bool useWorker;

    std::mutex production;
    std::array<VoiceDecoder, kRadioChannelCount> decoders;
    std::size_t sinceUpdate = 0;
    std::array<float, kRadioChannelCount> gainFrom{};
    std::array<float, kRadioChannelCount> gainTo{};
    std::array<float, kSpeechWindow> speech{};
    std::size_t speechPos = 0;

    std::mutex commandMutex;
    std::vector<std::function<void(RadioSequencer&)>> commands;

    std::mutex ringMutex;
    std::condition_variable ringCv;
    std::vector<float> ring = std::vector<float>(kLeadFrames * 2);
    std::size_t ringRead = 0;
    std::size_t ringCount = 0;
    std::atomic<std::uint64_t> mixedFrames{0};

    std::mutex eventMutex;
    std::vector<RadioEvent> events;

    std::atomic<bool> closed{false};
    std::thread worker;

    float gainOf(RadioChannel c) const
    {
        switch (c) {
        case RadioChannel::Music:
            return sequencer.musicGain();
        case RadioChannel::Ident:
            return sequencer.identGain();
        case RadioChannel::Dialogue:
            return sequencer.dialogueGain();
        }
        return 0.0F;
    }

    void syncDecoders()
    {
        for (std::size_t c = 0; c < kRadioChannelCount; ++c) {
            const RadioVoice& v = sequencer.voice(static_cast<RadioChannel>(c));
            VoiceDecoder& d = decoders[c];
            if (!v.active) {
                if (d.instance != 0) {
                    d.reset();
                }
                continue;
            }
            if (d.instance == v.instance) {
                continue;
            }
            d.reset();
            d.instance = v.instance;
            d.first = v.startFrame;
            try {
                d.stream.emplace(assets->openStream(v.bank, v.sampleIndex, v.startFrame));
                d.channels = static_cast<std::size_t>(std::max(d.stream->channels(), 1));
            } catch (const std::exception& e) {
                logWarning("radio: cannot stream {}: {}", v.fmodName, e.what());
            }
        }
    }

    void mixVoice(RadioChannel channel, float* out, std::size_t frames)
    {
        const auto c = static_cast<std::size_t>(channel);
        const RadioVoice& v = sequencer.voice(channel);
        if (!v.active) {
            return;
        }
        VoiceDecoder& d = decoders[c];
        const auto rate = static_cast<std::uint64_t>(v.sampleRate);
        constexpr auto kOut = static_cast<std::uint64_t>(RadioSequencer::kOutputRate);
        const float from = gainFrom[c];
        const float step = (gainTo[c] - from) / static_cast<float>(Radio::kUpdateFrames);
        for (std::size_t i = 0; i < frames; ++i) {
            const std::uint64_t played = v.elapsed + i;
            float left = 0.0F;
            float right = 0.0F;
            if (rate == kOut) {
                const std::size_t frame = v.startFrame + played;
                left = d.at(frame, 0);
                right = d.at(frame, 1);
            } else {
                const std::uint64_t scaled = played * rate;
                const std::size_t frame = v.startFrame + scaled / kOut;
                const float t = static_cast<float>(scaled % kOut) / static_cast<float>(kOut);
                const std::size_t before = frame > v.startFrame ? frame - 1 : frame;
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    const float value = audio::hermite(d.at(before, ch), d.at(frame, ch), d.at(frame + 1, ch),
                        d.at(frame + 2, ch), t);
                    (ch == 0 ? left : right) = value;
                }
            }
            if (channel == RadioChannel::Dialogue) {
                speech[speechPos] = left;
                speechPos = (speechPos + 1) % kSpeechWindow;
            }
            const float gain = from + step * static_cast<float>(sinceUpdate + i);
            out[i * 2] += left * gain;
            out[i * 2 + 1] += right * gain;
        }
        d.release(v.startFrame + static_cast<std::size_t>((v.elapsed + frames) * rate / kOut));
    }

    float speechMeanSquare() const
    {
        if (!sequencer.voice(RadioChannel::Dialogue).active) {
            return 0.0F;
        }
        float sum = 0.0F;
        for (const float s : speech) {
            sum += s * s;
        }
        return sum / static_cast<float>(kSpeechWindow);
    }

    /// Runs the sequencer on and mixes `frames` stereo frames into `out`.
    void produce(float* out, std::size_t frames)
    {
        std::lock_guard lock(production);
        {
            std::vector<std::function<void(RadioSequencer&)>> pending;
            {
                std::lock_guard commandLock(commandMutex);
                pending.swap(commands);
            }
            for (auto& command : pending) {
                command(sequencer);
            }
        }
        std::fill_n(out, frames * 2, 0.0F);
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t untilEvent = sequencer.framesUntilEvent();
            if (untilEvent == 0) {
                sequencer.advance(0);
                continue;
            }
            const std::size_t step = std::min({frames - done, kUpdateFrames - sinceUpdate, untilEvent});
            syncDecoders();
            if (!sequencer.paused()) {
                for (std::size_t c = 0; c < kRadioChannelCount; ++c) {
                    mixVoice(static_cast<RadioChannel>(c), out + done * 2, step);
                }
            }
            sequencer.advance(step);
            sinceUpdate += step;
            done += step;
            if (sinceUpdate == kUpdateFrames) {
                sinceUpdate = 0;
                sequencer.update(static_cast<float>(kUpdateFrames) / RadioSequencer::kOutputRate,
                    speechMeanSquare());
                for (std::size_t c = 0; c < kRadioChannelCount; ++c) {
                    gainFrom[c] = gainTo[c];
                    gainTo[c] = gainOf(static_cast<RadioChannel>(c));
                }
            }
        }
        syncDecoders();
        std::vector<RadioEvent> produced = sequencer.takeEvents();
        if (!produced.empty()) {
            std::lock_guard eventLock(eventMutex);
            events.insert(events.end(), std::make_move_iterator(produced.begin()),
                std::make_move_iterator(produced.end()));
        }
    }

    void runWorker()
    {
        std::vector<float> chunk(kChunkFrames * 2);
        while (!closed) {
            {
                std::unique_lock lock(ringMutex);
                ringCv.wait_for(lock, std::chrono::milliseconds(5),
                    [&] { return closed || ringCount + kChunkFrames <= kLeadFrames; });
                if (closed) {
                    return;
                }
                if (ringCount + kChunkFrames > kLeadFrames) {
                    continue;
                }
            }
            produce(chunk.data(), kChunkFrames);
            std::lock_guard lock(ringMutex);
            for (std::size_t i = 0; i < kChunkFrames; ++i) {
                const std::size_t at = (ringRead + ringCount + i) % kLeadFrames;
                ring[at * 2] = chunk[i * 2];
                ring[at * 2 + 1] = chunk[i * 2 + 1];
            }
            ringCount += kChunkFrames;
        }
    }

    std::size_t read(float* out, std::size_t frames)
    {
        if (closed) {
            return 0;
        }
        if (!useWorker) {
            produce(out, frames);
            mixedFrames += frames;
            return frames;
        }
        std::size_t got = 0;
        {
            std::lock_guard lock(ringMutex);
            got = std::min(frames, ringCount);
            for (std::size_t i = 0; i < got; ++i) {
                const std::size_t at = (ringRead + i) % kLeadFrames;
                out[i * 2] = ring[at * 2];
                out[i * 2 + 1] = ring[at * 2 + 1];
            }
            ringRead = (ringRead + got) % kLeadFrames;
            ringCount -= got;
        }
        mixedFrames += got;
        ringCv.notify_one();
        return got;
    }
};

/// The radio's stream voice in the mixer. It holds the radio's state, so a
/// mixer that still has the voice after the Radio is gone reads silence.
class Radio::Output final : public audio::PcmStream {
public:
    explicit Output(std::shared_ptr<Impl> impl)
        : m_impl(std::move(impl))
    {
    }

    int channels() const override { return 2; }
    std::size_t read(float* out, std::size_t frames) override { return m_impl->read(out, frames); }
    bool finished() const override { return m_impl->closed; }

private:
    std::shared_ptr<Impl> m_impl;
};

Radio::Radio(const Vfs& vfs, audio::AudioDevice& device, const Options& options)
    : m_assets(RadioAssets::load(vfs, options.voiceLanguage))
    , m_mixer(device.mixer())
{
    const RadioSystemData& system = m_assets->catalog()->system;
    m_offStation = system.offStation();
    const RadioText text(vfs, options.textLanguage, m_offStation + 1, m_offStation);
    m_radioOff = text.radioOff();
    for (std::size_t i = 0; i < system.stations.size(); ++i) {
        RadioStationInfo info;
        info.name = system.stations[i].name;
        info.displayName = text.stationName(i);
        info.tunable = i <= m_offStation;
        info.off = i == m_offStation;
        m_stations.push_back(std::move(info));
    }
    m_impl = std::make_shared<Impl>(m_assets, options.seed, options.worker);
    m_nowPlaying.station = 0;
    m_nowPlaying.off = m_offStation == 0;
    m_nowPlaying.stationName = m_stations.empty() ? std::string{} : m_stations[0].displayName;
    if (options.worker) {
        m_impl->worker = std::thread([impl = m_impl.get()] { impl->runWorker(); });
    }
    audio::VoiceSettings settings;
    settings.bus = options.bus;
    m_voice = m_mixer.playStream(std::make_shared<Output>(m_impl), settings);
}

Radio::~Radio()
{
    m_impl->closed = true;
    m_impl->ringCv.notify_all();
    if (m_impl->worker.joinable()) {
        m_impl->worker.join();
    }
    m_mixer.stop(m_voice, 0.0F);
}

void Radio::post(std::function<void(RadioSequencer&)> command)
{
    std::lock_guard lock(m_impl->commandMutex);
    m_impl->commands.push_back(std::move(command));
}

void Radio::startFreeRoam()
{
    post([](RadioSequencer& s) { s.startFreeRoam(); });
}

void Radio::startForEvent(std::string_view trackName, float secondsToEventStart)
{
    post([name = std::string(trackName), secondsToEventStart](RadioSequencer& s) {
        s.startForEvent(name, secondsToEventStart);
    });
}

void Radio::stop(float delaySeconds)
{
    post([delaySeconds](RadioSequencer& s) { s.stop(delaySeconds); });
}

void Radio::selectStation(std::size_t index, bool fade)
{
    post([index, fade](RadioSequencer& s) { s.selectStation(index, fade); });
}

void Radio::setPlayerStation(std::size_t index)
{
    post([index](RadioSequencer& s) { s.setPlayerStation(index); });
}

void Radio::nextStation()
{
    post([](RadioSequencer& s) { s.nextStation(); });
}

void Radio::previousStation()
{
    post([](RadioSequencer& s) { s.previousStation(); });
}

void Radio::turnOff()
{
    selectStation(m_offStation);
}

void Radio::setTimeOfDay(TimeOfDay timeOfDay)
{
    post([timeOfDay](RadioSequencer& s) { s.setTimeOfDay(timeOfDay); });
}

void Radio::setVolume(float volume)
{
    post([volume](RadioSequencer& s) { s.setUserVolume(volume); });
}

void Radio::setPaused(bool paused)
{
    post([paused](RadioSequencer& s) { s.setPaused(paused); });
}

void Radio::setStationIdentsEnabled(bool enabled)
{
    post([enabled](RadioSequencer& s) { s.setStationIdentsEnabled(enabled); });
}

void Radio::setDjDialogueEnabled(bool enabled)
{
    post([enabled](RadioSequencer& s) { s.setDjDialogueEnabled(enabled); });
}

void Radio::setStationSwitchingLocked(bool locked)
{
    post([locked](RadioSequencer& s) { s.setStationSwitchingLocked(locked); });
}

void Radio::setDialogueGroupEnabled(int group, bool enabled)
{
    post([group, enabled](RadioSequencer& s) { s.setDialogueGroupEnabled(group, enabled); });
}

void Radio::apply(const RadioEvent& e)
{
    switch (e.kind) {
    case RadioEvent::Kind::StationChanged:
        m_nowPlaying.station = e.station;
        m_nowPlaying.off = e.station == m_offStation;
        m_nowPlaying.stationName = e.station < m_stations.size() ? m_stations[e.station].displayName : "";
        m_nowPlaying.title.clear();
        m_nowPlaying.artist.clear();
        m_nowPlaying.djSpeaking = false;
        break;
    case RadioEvent::Kind::TrackStarted:
        if (e.station != m_offStation) {
            m_nowPlaying.title = e.title;
            m_nowPlaying.artist = e.artist;
        }
        break;
    case RadioEvent::Kind::TrackEnded:
        m_nowPlaying.title.clear();
        m_nowPlaying.artist.clear();
        break;
    case RadioEvent::Kind::Stopped:
        m_nowPlaying.title.clear();
        m_nowPlaying.artist.clear();
        m_nowPlaying.djSpeaking = false;
        break;
    case RadioEvent::Kind::DialogueStarted:
        m_nowPlaying.djSpeaking = true;
        break;
    case RadioEvent::Kind::DialogueEnded:
        m_nowPlaying.djSpeaking = false;
        break;
    case RadioEvent::Kind::HudShow: {
        RadioNowPlaying hud = m_nowPlaying;
        hud.station = e.station;
        hud.off = e.station == m_offStation;
        hud.stationName = e.station < m_stations.size() ? m_stations[e.station].displayName : "";
        hud.title = e.title;
        hud.artist = e.artist;
        m_hudPending = std::move(hud);
        m_hudTimer = e.delaySeconds;
        break;
    }
    case RadioEvent::Kind::IdentStarted:
    case RadioEvent::Kind::Paused:
    case RadioEvent::Kind::Resumed:
        break;
    }
}

void Radio::update(float dt)
{
    const std::uint64_t mixed = m_impl->mixedFrames;
    std::vector<RadioEvent> ready;
    {
        std::lock_guard lock(m_impl->eventMutex);
        auto split = std::find_if(m_impl->events.begin(), m_impl->events.end(),
            [mixed](const RadioEvent& e) { return e.frame > mixed; });
        ready.assign(std::make_move_iterator(m_impl->events.begin()), std::make_move_iterator(split));
        m_impl->events.erase(m_impl->events.begin(), split);
    }
    for (const RadioEvent& e : ready) {
        apply(e);
    }
    m_released.insert(m_released.end(), std::make_move_iterator(ready.begin()), std::make_move_iterator(ready.end()));
    if (m_hudPending) {
        m_hudTimer -= dt;
        if (m_hudTimer <= 0.0F) {
            m_hudRequest = std::move(m_hudPending);
            m_hudPending.reset();
        }
    }
}

std::optional<RadioNowPlaying> Radio::takeHudRequest()
{
    return std::exchange(m_hudRequest, std::nullopt);
}

std::vector<RadioEvent> Radio::takeEvents()
{
    return std::exchange(m_released, {});
}

} // namespace openhorizon::radio
