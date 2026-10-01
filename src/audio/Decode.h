#pragma once

#include "Fsb.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace openhorizon::audio {

/// Decoded audio: interleaved float samples in [-1, 1].
struct PcmBuffer {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;
    /// Loop region in frames, end exclusive, from the FSB sample's loop points.
    std::size_t loopStart = 0;
    std::size_t loopEnd = 0;

    std::size_t frames() const { return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0; }
};

/// Decodes XMA (2048-byte packets, synthesized XMA2WAVEFORMATEX, one stream per channel pair; verified on
/// every disc sample), MPEG (FSB pads between frames) and PCM; other codecs throw DataError. The result has
/// exactly `sample.sampleCount` frames: extra output is trimmed, too few frames throw DataError.
PcmBuffer decodeSample(const FsbBank& bank, std::size_t index);

/// Frames a codec produced for one sample before trimming, for tools that
/// check the whole disc. Same rules and errors as decodeSample().
struct DecodeReport {
    std::size_t headerFrames = 0;
    std::size_t codecFrames = 0;
};
PcmBuffer decodeSample(const FsbBank& bank, std::size_t index, DecodeReport& report);

/// Fills `out` from `offset` and returns the bytes read (fewer only at the end of the file).
/// Called from whatever thread decodes.
using ByteSource = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;

/// Decodes one MPEG sample progressively, 64 KiB at a time, for sounds too long to hold decoded (a radio
/// track is up to eight minutes, 180 MB as float).
/// Decoding starts kPrerollFrames early because layer III frames borrow bits from earlier ones (the
/// preroll length matches a decode from the start on the radio banks). Not thread-safe.
class MpegSampleStream {
public:
    static constexpr std::size_t kPrerollFrames = 8;

    /// `sample.dataOffset` and `sample.compressedSize` locate the sample in the file `read` reads.
    /// Throws DataError unless the sample is MPEG or `startFrame` is past its end.
    MpegSampleStream(const FsbSample& sample, ByteSource read, std::string source, std::size_t startFrame = 0);
    ~MpegSampleStream();
    MpegSampleStream(MpegSampleStream&&) noexcept;
    MpegSampleStream& operator=(MpegSampleStream&&) noexcept;

    int channels() const;
    int sampleRate() const;
    /// The sample's length in frames, from its header.
    std::size_t totalFrames() const;
    /// The frame at which the next decoded audio starts.
    std::size_t position() const;
    bool finished() const;

    /// Appends at least `minFrames` interleaved frames (fewer only at the end) and returns how many.
    /// Throws DataError if the data is damaged.
    std::size_t decode(std::size_t minFrames, std::vector<float>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::audio
