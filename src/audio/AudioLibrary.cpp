#include "AudioLibrary.h"

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <memory>
#include <utility>
#include <vector>

namespace openhorizon::audio {

AudioLibrary::AudioLibrary(const Vfs& vfs)
    : m_vfs(vfs)
{
}

std::shared_ptr<const FsbBank> AudioLibrary::bank(std::string_view path)
{
    const std::string key = Vfs::normalize(path);
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_banks.find(key); it != m_banks.end()) {
            it->second.lastUsed = Clock::now();
            return it->second.value;
        }
    }
    auto parsed = std::make_shared<const FsbBank>(FsbBank::parse(m_vfs.readOrThrow(key), key));
    std::lock_guard lock(m_mutex);
    return m_banks.try_emplace(key, Cached<FsbBank>{std::move(parsed), Clock::now()}).first->second.value;
}

std::shared_ptr<const FevProject> AudioLibrary::project(std::string_view path)
{
    const std::string key = Vfs::normalize(path);
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_projects.find(key); it != m_projects.end()) {
            it->second.lastUsed = Clock::now();
            return it->second.value;
        }
    }
    const Bytes data = m_vfs.readOrThrow(key);
    auto parsed = std::make_shared<const FevProject>(FevProject::parse(data, key));
    std::lock_guard lock(m_mutex);
    return m_projects.try_emplace(key, Cached<FevProject>{std::move(parsed), Clock::now()}).first->second.value;
}

std::shared_ptr<const PcmBuffer> AudioLibrary::sample(std::string_view bankPath, std::size_t index)
{
    return sampleChannel(bankPath, index, -1);
}

std::shared_ptr<const PcmBuffer> AudioLibrary::sampleChannel(std::string_view bankPath, std::size_t index, int channel)
{
    std::tuple<std::string, std::size_t, int> key{Vfs::normalize(bankPath), index, channel};
    {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_samples.find(key); it != m_samples.end()) {
            it->second.lastUsed = Clock::now();
            return it->second.value;
        }
    }
    std::shared_ptr<const PcmBuffer> result;
    if (channel < 0) {
        const std::shared_ptr<const FsbBank> source = bank(std::get<0>(key));
        result = std::make_shared<const PcmBuffer>(decodeSample(*source, index));
    } else {
        const std::shared_ptr<const PcmBuffer> whole = sampleChannel(bankPath, index, -1);
        if (channel >= whole->channels) {
            throw DataError("{}: sample {} has {} channels, not {}", std::get<0>(key), index, whole->channels,
                channel + 1);
        }
        auto mono = std::make_shared<PcmBuffer>();
        mono->sampleRate = whole->sampleRate;
        mono->channels = 1;
        mono->loopStart = whole->loopStart;
        mono->loopEnd = whole->loopEnd;
        const std::size_t frames = whole->frames();
        mono->samples.resize(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            mono->samples[i] = whole->samples[i * static_cast<std::size_t>(whole->channels)
                + static_cast<std::size_t>(channel)];
        }
        result = std::move(mono);
    }
    std::lock_guard lock(m_mutex);
    return m_samples.try_emplace(std::move(key), Cached<PcmBuffer>{std::move(result), Clock::now()})
        .first->second.value;
}

std::string AudioLibrary::bankPathFor(std::string_view projectPath, std::string_view bankName)
{
    const std::string normalized = Vfs::normalize(projectPath);
    const std::size_t slash = normalized.find_last_of('/');
    const std::string folder = slash == std::string::npos ? std::string{} : normalized.substr(0, slash + 1);
    return Vfs::normalize(folder + std::string(bankName) + ".fsb");
}

std::size_t AudioLibrary::resolveWaveform(const FsbBank& bank, const FevWaveform& waveform) const
{
    if (waveform.type != FevWaveform::Type::Wavetable) {
        throw DataError("{}: waveform is not a wavetable and has no sample", bank.source());
    }
    if (waveform.sampleIndex < bank.size()) {
        if (bank.sampleHasName(waveform.sampleIndex, waveform.fileName)) {
            return waveform.sampleIndex;
        }
        const std::optional<std::size_t> byName = bank.findSample(waveform.fileName);
        if (!byName) {
            return waveform.sampleIndex;
        }
        logDebug("audio: {} index {} holds '{}', using '{}' at {}", bank.source(), waveform.sampleIndex,
            bank.samples()[waveform.sampleIndex].name, waveform.fileName, *byName);
        return *byName;
    }
    if (const std::optional<std::size_t> byName = bank.findSample(waveform.fileName)) {
        return *byName;
    }
    throw DataError("{}: waveform '{}' index {} is past the bank's {} samples", bank.source(), waveform.fileName,
        waveform.sampleIndex, bank.size());
}

std::shared_ptr<const PcmBuffer> AudioLibrary::waveform(std::string_view projectPath, const FevWaveform& waveform)
{
    const std::string bankPath = bankPathFor(projectPath, waveform.bankName);
    const std::shared_ptr<const FsbBank> source = bank(bankPath);
    return sample(bankPath, resolveWaveform(*source, waveform));
}

void AudioLibrary::clearSamples()
{
    std::lock_guard lock(m_mutex);
    m_samples.clear();
}

namespace {

std::size_t pcmBytes(const PcmBuffer& buffer)
{
    return buffer.samples.size() * sizeof(float);
}

/// Moves the idle entries only `cache` holds and `releasable` allows into `dropped`.
template <typename Map, typename Releasable, typename Size> void takeUnused(Map& cache,
    AudioLibrary::Clock::time_point before, Releasable releasable, Size size,
    std::vector<std::shared_ptr<const void>>& dropped, std::size_t& released)
{
    for (auto it = cache.begin(); it != cache.end();) {
        auto& entry = it->second;
        if (entry.value.use_count() == 1 && entry.lastUsed < before && releasable(*entry.value)) {
            released += size(*entry.value);
            dropped.push_back(std::move(entry.value));
            it = cache.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

std::size_t AudioLibrary::releaseUnused(Clock::time_point now, Clock::duration idle)
{
    const Clock::time_point before = now - idle;
    std::size_t released = 0;
    // Destroyed after the lock, so a waiting decode does not wait on the free.
    std::vector<std::shared_ptr<const void>> dropped;
    {
        std::lock_guard lock(m_mutex);
        takeUnused(m_samples, before, [](const PcmBuffer& pcm) { return pcmBytes(pcm) >= kKeptSampleBytes; }, pcmBytes,
            dropped, released);
        takeUnused(m_banks, before, [](const FsbBank&) { return true; },
            [](const FsbBank& bank) { return bank.fileBytes(); }, dropped, released);
        takeUnused(m_projects, before, [](const FevProject&) { return true; },
            [](const FevProject&) { return std::size_t{0}; }, dropped, released);
    }
    return released;
}

std::size_t AudioLibrary::decodedBytes() const
{
    std::lock_guard lock(m_mutex);
    std::size_t bytes = 0;
    for (const auto& [key, entry] : m_samples) {
        bytes += pcmBytes(*entry.value);
    }
    return bytes;
}

} // namespace openhorizon::audio
