#pragma once

#include "Fsb.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
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

/// Fills `out` with the bytes of a file starting at `offset` and returns how
/// many it could read (fewer only at the end of the file). Called from
/// whatever thread decodes.
using ByteSource = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;

/// Decodes one MPEG sample of a bank progressively, for sounds too long to
/// hold decoded (a radio track is up to eight minutes, 180 MB as float) or to
/// decode before they start. The encoded bytes are read through a ByteSource
/// as needed, 64 KiB at a time, so the bank never has to be in memory.
///
/// Framing and decoding follow decodeSample(): the output of a stream started
/// at frame 0 is the same audio. A stream can start at any frame: MPEG
/// frames before it are only scanned for their headers, and decoding starts
/// kPrerollFrames MPEG frames early, with that output dropped, because a
/// layer III frame borrows bits from the frames before it and overlaps its
/// predecessor's output (the preroll length is a choice that makes the
/// output match a decode from the start; verified on the radio banks).
///
/// Not thread-safe; one thread at a time may use a stream.
class MpegSampleStream {
public:
    static constexpr std::size_t kPrerollFrames = 8;

    /// `sample.dataOffset` and `sample.compressedSize` locate the sample in
    /// the file `read` reads. Throws DataError unless the sample is MPEG or
    /// `startFrame` is past its end.
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

    /// Appends at least `minFrames` decoded frames (interleaved float) to
    /// `out`, fewer only when the sample ends, and returns how many it
    /// appended. Throws DataError if the data is damaged.
    std::size_t decode(std::size_t minFrames, std::vector<float>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fh::audio
