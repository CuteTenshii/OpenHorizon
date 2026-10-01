#pragma once

#include "Decode.h"
#include "Fev.h"
#include "Fsb.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

/// Loads sound banks and event projects through the game's file system and
/// keeps them, and the samples decoded from them, for reuse. Thread-safe.
class AudioLibrary {
public:
    explicit AudioLibrary(const Vfs& vfs);

    const Vfs& vfs() const { return m_vfs; }

    /// The bank at a game path, read and parsed on first use. Throws
    /// DataError if it is missing or malformed.
    std::shared_ptr<const FsbBank> bank(std::string_view path);
    /// The event project at a game path, read and parsed on first use.
    std::shared_ptr<const FevProject> project(std::string_view path);
    /// One sample of a bank, decoded on first use.
    std::shared_ptr<const PcmBuffer> sample(std::string_view bankPath, std::size_t index);
    /// One channel of a sample as a mono buffer, or the whole sample for
    /// `channel` -1. Throws DataError for a channel the sample lacks.
    std::shared_ptr<const PcmBuffer> sampleChannel(std::string_view bankPath, std::size_t index, int channel);

    /// Where a project's bank lives: the .fev's folder, the bank name, ".fsb"
    /// (verified for every project on the disc, loose and in track archives).
    static std::string bankPathFor(std::string_view projectPath, std::string_view bankName);

    /// The bank index a wavetable waveform plays. The waveform's own index is
    /// used when the sample there carries the waveform's file name; banks
    /// rebuilt after their project moved some samples, so otherwise the name
    /// is looked up, and failing that the index is trusted. Throws DataError
    /// for other waveform types and for indices past the bank's end.
    std::size_t resolveWaveform(const FsbBank& bank, const FevWaveform& waveform) const;

    /// The decoded sample a waveform of the project at `projectPath` plays.
    std::shared_ptr<const PcmBuffer> waveform(std::string_view projectPath, const FevWaveform& waveform);

    /// Drops every decoded sample (banks and projects stay).
    void clearSamples();

    using Clock = std::chrono::steady_clock;
    /// Drops what only the library holds and nobody asked for within `idle`,
    /// except samples under kKeptSampleBytes; returns the bytes released.
    /// Call it off the audio thread. An engine choice: the game's FMOD
    /// unload rules were not traced.
    std::size_t releaseUnused(Clock::time_point now, Clock::duration idle);
    static constexpr std::size_t kKeptSampleBytes = 256 * 1024;
    /// Bytes of decoded audio held, whether in use or not.
    std::size_t decodedBytes() const;

private:
    template <typename T> struct Cached {
        std::shared_ptr<const T> value;
        Clock::time_point lastUsed;
    };

    const Vfs& m_vfs;
    mutable std::mutex m_mutex;
    std::map<std::string, Cached<FsbBank>, std::less<>> m_banks;
    std::map<std::string, Cached<FevProject>, std::less<>> m_projects;
    /// Decoded samples by bank path, index and channel (-1 for all).
    std::map<std::tuple<std::string, std::size_t, int>, Cached<PcmBuffer>> m_samples;
};

} // namespace openhorizon::audio
