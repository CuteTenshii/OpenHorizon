#pragma once

#include "Decode.h"
#include "Fev.h"
#include "Fsb.h"

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

private:
    const Vfs& m_vfs;
    std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<const FsbBank>, std::less<>> m_banks;
    std::map<std::string, std::shared_ptr<const FevProject>, std::less<>> m_projects;
    /// Decoded samples by bank path, index and channel (-1 for all).
    std::map<std::tuple<std::string, std::size_t, int>, std::shared_ptr<const PcmBuffer>> m_samples;
};

} // namespace openhorizon::audio
