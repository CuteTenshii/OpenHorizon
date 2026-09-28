#pragma once

#include "Fsb.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fh::audio {

/// Decoded audio: interleaved float samples in [-1, 1].
struct PcmBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;
    /// Loop region in frames, end exclusive. A buffer decoded from an FSB
    /// sample takes the sample's loop points; the mixer loops over this
    /// region when a voice is told to loop.
    std::size_t loopStart = 0;
    std::size_t loopEnd = 0;

    std::size_t frames() const { return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

/// Decodes one sample of a bank to float PCM.
///
/// XMA samples go through FFmpeg's XMA2 decoder in 2048-byte packets with a
/// synthesized XMA2WAVEFORMATEX extension (one stream per channel pair); the
/// FSB XMA data needs no other framing (verified: every XMA sample on the
/// disc decodes to exactly its header's frame count). MPEG samples are split
/// into frames by their headers, skipping the zero padding FSB puts between
/// frames, and decoded by FFmpeg's MPEG audio decoders. PCM is converted
/// directly. Other codecs throw DataError.
///
/// The result has exactly `sample.sampleCount` frames: a codec that yields
/// more is trimmed (MPEG decoders never did on the disc). A codec that
/// yields fewer throws DataError, since that means the data is damaged.
PcmBuffer decodeSample(const FsbBank& bank, std::size_t index);

/// Frames a codec produced for one sample before trimming, for tools that
/// check the whole disc. Same rules and errors as decodeSample().
struct DecodeReport {
    std::size_t headerFrames = 0;
    std::size_t codecFrames = 0;
};
PcmBuffer decodeSample(const FsbBank& bank, std::size_t index, DecodeReport& report);

} // namespace fh::audio
