#pragma once

#include "base/Bytes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::audio {

/// How a sample's data is encoded, from the FSOUND_* bits of its mode word.
enum class SampleCodec {
    Pcm8,
    Pcm16,
    ImaAdpcm,
    Vag,
    GcAdpcm,
    Xma,
    Mpeg,
};

const char* codecName(SampleCodec codec);

/// A named position in a sample, from the "SYNC" chunk FMOD writes after a
/// sample header when the mode has FSOUND_SYNCPOINTS (0x80000000). The chunk
/// is "SYNC", a u32 count, then per point a u32 frame offset and a 256-byte
/// NUL-padded name (verified on every sample of the radio banks, where the
/// header sizes add up exactly and the offsets, converted to milliseconds,
/// are the values RadioSoundbankInfo_*.xml lists).
struct FsbSyncPoint {
    /// Offset from the start of the sample in sample frames.
    std::uint32_t frame = 0;
    std::string name;
};

/// One entry of an FSB4 sample header table.
struct FsbSample {
    std::string name;
    /// Decoded length in sample frames. For XMA and MPEG this is exactly what
    /// the codec produces (verified on every sample of the disc).
    std::uint32_t sampleCount = 0;
    std::uint32_t compressedSize = 0;
    /// Absolute byte offset of the sample's data in the bank file.
    std::size_t dataOffset = 0;
    std::uint32_t loopStart = 0;
    /// Inclusive, as FMOD stores it (sampleCount - 1 for a whole-sample loop).
    std::uint32_t loopEnd = 0;
    std::uint32_t mode = 0;
    SampleCodec codec = SampleCodec::Pcm16;
    /// Default playback rate in Hz.
    std::int32_t frequency = 0;
    std::uint16_t defaultVolume = 255;
    /// 0 = left, 128 = centre, 255 = right.
    std::int16_t defaultPan = 128;
    std::uint16_t defaultPriority = 128;
    std::uint16_t channels = 1;
    float minDistance = 1.0F;
    float maxDistance = 10000.0F;
    /// Cumulative decoded frame counts at the end of each 32 KiB XMA block
    /// (inferred from the values: the last entry is slightly above
    /// sampleCount, entries grow by roughly one block's worth). Empty for
    /// other codecs and for XMA samples written without the table.
    std::vector<std::uint32_t> xmaSeekTable;
    /// Named positions, in the order the header lists them. The index into
    /// this list is what FMOD reports to a sync point callback.
    std::vector<FsbSyncPoint> syncPoints;

    /// FSOUND_LOOP_NORMAL or FSOUND_LOOP_BIDI.
    bool loops() const { return (mode & 0x6U) != 0; }
    /// FSOUND_8BITS PCM stored unsigned (FSOUND_UNSIGNED).
    bool pcmUnsigned() const { return (mode & 0x80U) != 0; }
};

/// An FMOD sound bank in the FSB4 layout, the only FSB version on the disc
/// (990 loose banks plus 16 in track archives; verified).
///
/// Everything is little-endian, including on the Xbox 360 (verified):
///
///     0x00 char[4] "FSB4"
///     0x04 s32     sample count
///     0x08 s32     size of the sample header table
///     0x0c s32     size of the sample data
///     0x10 u32     version, 0x00040000 on every bank
///     0x14 u32     bank flags: 0x02 basic headers, 0x08 big-endian PCM,
///                  0x20/0x40 MPEG frames padded to 2/4 bytes
///     0x18 u8[8]   hash; FEV bank tables store the same 8 bytes (verified)
///     0x20 u8[16]  unknown (differs per bank, presumably a GUID)
///     0x30         sample headers, then sample data
///
/// A full sample header (the first one always, all of them unless the bank
/// has basic headers) is:
///
///     0x00 u16      size of this header including the extra bytes
///     0x02 char[30] name, NUL padded (long names are truncated)
///     0x20 u32      length in sample frames
///     0x24 u32      compressed size in bytes
///     0x28 u32      loop start, 0x2c u32 loop end (inclusive)
///     0x30 u32      mode (FSOUND_* bits: 0x2 loop, 0x8 8-bit, 0x10 16-bit,
///                   0x20 mono, 0x40 stereo, 0x80 unsigned, 0x200 MPEG,
///                   0x400000 IMA ADPCM, 0x800000 VAG, 0x1000000 XMA,
///                   0x2000000 GC ADPCM, 0x10000000 MPEG layer 3,
///                   0x80000000 sync points present)
///     0x34 s32      default frequency
///     0x38 u16 volume, 0x3a s16 pan, 0x3c u16 priority, 0x3e u16 channels
///     0x40 f32 min distance, 0x44 f32 max distance
///     0x48 s32      variation frequency (holds the header size on this
///                   disc, so it is not used), 0x4c u16/s16 variation
///                   volume and pan
///     0x50          extra bytes: for XMA 12 zero bytes, a u32 byte count
///                   and that many bytes holding u32 1, u32 entries + 1,
///                   then the seek table (inferred layout, consistent on
///                   every XMA sample); for MPEG with sync points a "SYNC"
///                   chunk (see FsbSyncPoint)
///
/// A basic header is just the frame length and compressed size; the other
/// fields repeat the first sample's.
///
/// Sample data starts right after the header table; each sample's data
/// starts on a 32-byte boundary counted from there (verified: without the
/// rounding 6937 MPEG samples would not start on a frame sync).
class FsbBank {
public:
    /// Parses the header of a bank the object then owns. Throws DataError
    /// naming `source` if the layout is not FSB4 or offsets run past the end.
    static FsbBank parse(Bytes data, std::string source);
    /// Parses only the headers of a bank too large to hold in memory, such
    /// as the 680 MB radio music bank that is streamed. `headers` must hold
    /// at least the 0x30-byte header and the sample header table
    /// (headersSize() tells how much that is); `fileSize` is the size of the
    /// whole file, against which the sample offsets are checked. The bank
    /// has no sample data: sampleData() throws, and the samples are read
    /// from the file at their dataOffset instead.
    static FsbBank parseHeaders(Bytes headers, std::uint64_t fileSize, std::string source);
    /// The number of bytes parseHeaders() needs, from the first 0x30 bytes of
    /// a bank. Throws DataError if they are not an FSB4 header.
    static std::size_t headersSize(ByteView firstBytes, const std::string& source);
    static constexpr std::size_t kFileHeaderSize = 0x30;

    const std::string& source() const { return m_source; }
    std::uint32_t version() const { return m_version; }
    std::uint32_t flags() const { return m_flags; }
    /// The 8 bytes at 0x18 that FEV projects use to check they match a bank.
    std::uint64_t hash() const { return m_hash; }
    bool bigEndianPcm() const { return (m_flags & 0x8U) != 0; }

    const std::vector<FsbSample>& samples() const { return m_samples; }
    std::size_t size() const { return m_samples.size(); }
    const FsbSample& sample(std::size_t index) const;
    /// The encoded bytes of one sample. Throws DataError for a bank made by
    /// parseHeaders().
    ByteView sampleData(std::size_t index) const;
    /// False for a bank made by parseHeaders().
    bool hasData() const { return m_hasData; }
    /// Bytes of the file the bank holds (headers only after parseHeaders()).
    std::size_t fileBytes() const { return m_data.size(); }

    /// The index of the sample with this name, compared without case. Names
    /// are matched the way FEV waveform file names relate to FSB names: with
    /// or without a folder and a ".wav" extension, and against the 29
    /// characters an FSB header keeps.
    std::optional<std::size_t> findSample(std::string_view name) const;
    /// Whether the sample at `index` has `name`, by findSample()'s rules.
    bool sampleHasName(std::size_t index, std::string_view name) const;

private:
    static FsbBank parseImpl(Bytes data, std::uint64_t fileSize, bool hasData, std::string source);

    Bytes m_data;
    bool m_hasData = true;
    std::string m_source;
    std::uint32_t m_version = 0;
    std::uint32_t m_flags = 0;
    std::uint64_t m_hash = 0;
    std::vector<FsbSample> m_samples;
};

/// The codec an FSOUND_* mode word selects.
SampleCodec codecFromMode(std::uint32_t mode);

} // namespace openhorizon::audio
