#include "Fsb.h"

#include "LeReader.h"

#include <algorithm>

namespace fh::audio {

namespace {

constexpr std::size_t kHeaderSize = 0x30;
constexpr std::size_t kSampleHeaderSize = 0x50;
constexpr std::size_t kNameLength = 30;
constexpr std::size_t kDataAlignment = 32;
constexpr std::uint32_t kBasicHeaders = 0x2;
constexpr std::uint32_t kModeXma = 0x01000000;

std::string_view baseName(std::string_view name)
{
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    return name;
}

/// The FSB name a waveform file name becomes when it fits: no folder and no
/// ".wav".
std::string_view stemOf(std::string_view name)
{
    name = baseName(name);
    if (endsWithNoCase(name, ".wav")) {
        name.remove_suffix(4);
    }
    return name;
}

void readXmaExtra(LeReader& in, std::size_t end, FsbSample& sample)
{
    // Twelve bytes precede the table size; they were zero on every sample.
    if (end - in.pos() < 16) {
        return;
    }
    in.skip(12);
    const std::uint32_t tableBytes = in.u32();
    if (tableBytes < 8 || tableBytes > end - in.pos()) {
        return;
    }
    in.skip(4);
    const std::uint32_t count = in.u32();
    const std::uint32_t entries = std::min<std::uint32_t>(count > 0 ? count - 1 : 0, (tableBytes - 8) / 4);
    sample.xmaSeekTable.reserve(entries);
    for (std::uint32_t i = 0; i < entries; ++i) {
        sample.xmaSeekTable.push_back(in.u32());
    }
}

} // namespace

const char* codecName(SampleCodec codec)
{
    switch (codec) {
    case SampleCodec::Pcm8:
        return "pcm8";
    case SampleCodec::Pcm16:
        return "pcm16";
    case SampleCodec::ImaAdpcm:
        return "ima-adpcm";
    case SampleCodec::Vag:
        return "vag";
    case SampleCodec::GcAdpcm:
        return "gc-adpcm";
    case SampleCodec::Xma:
        return "xma";
    case SampleCodec::Mpeg:
        return "mpeg";
    }
    return "unknown";
}

SampleCodec codecFromMode(std::uint32_t mode)
{
    if (mode & kModeXma) {
        return SampleCodec::Xma;
    }
    if (mode & 0x200U) {
        return SampleCodec::Mpeg;
    }
    if (mode & 0x00400000U) {
        return SampleCodec::ImaAdpcm;
    }
    if (mode & 0x00800000U) {
        return SampleCodec::Vag;
    }
    if (mode & 0x02000000U) {
        return SampleCodec::GcAdpcm;
    }
    return (mode & 0x8U) ? SampleCodec::Pcm8 : SampleCodec::Pcm16;
}

FsbBank FsbBank::parse(Bytes data, std::string source)
{
    FsbBank bank;
    bank.m_data = std::move(data);
    bank.m_source = std::move(source);
    LeReader in(bank.m_data, bank.m_source);

    in.need(kHeaderSize);
    if (in.fixedString(4) != "FSB4") {
        in.seek(0);
        in.fail("not an FSB4 sound bank");
    }
    const std::int32_t count = in.s32();
    const std::int32_t headersSize = in.s32();
    const std::int32_t dataSize = in.s32();
    bank.m_version = in.u32();
    bank.m_flags = in.u32();
    bank.m_hash = in.u64();
    in.skip(16);
    if (count < 0 || headersSize < 0 || dataSize < 0) {
        in.fail(std::format("negative sizes ({} samples, {} header bytes, {} data bytes)", count, headersSize,
            dataSize));
    }
    // Even a basic header takes 8 bytes, which bounds the count before it
    // sizes any allocation.
    if (count > headersSize / 8) {
        in.fail(std::format("{} samples cannot fit in {} header bytes", count, headersSize));
    }
    const std::size_t dataStart = kHeaderSize + static_cast<std::size_t>(headersSize);
    if (dataStart + static_cast<std::size_t>(dataSize) > bank.m_data.size()) {
        in.fail(std::format("header and data need 0x{:x} bytes, the file has 0x{:x}",
            dataStart + static_cast<std::size_t>(dataSize), bank.m_data.size()));
    }

    const bool basic = (bank.m_flags & kBasicHeaders) != 0;
    std::size_t dataCursor = 0;
    bank.m_samples.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        FsbSample sample;
        if (basic && i > 0) {
            sample = bank.m_samples.front();
            sample.xmaSeekTable.clear();
            sample.sampleCount = in.u32();
            sample.compressedSize = in.u32();
            sample.loopStart = 0;
            sample.loopEnd = sample.sampleCount > 0 ? sample.sampleCount - 1 : 0;
        } else {
            const std::size_t start = in.pos();
            const std::uint16_t size = in.u16();
            if (size < kSampleHeaderSize) {
                in.fail(std::format("sample {} header is {} bytes, less than 0x50", i, size));
            }
            sample.name = in.fixedString(kNameLength);
            sample.sampleCount = in.u32();
            sample.compressedSize = in.u32();
            sample.loopStart = in.u32();
            sample.loopEnd = in.u32();
            sample.mode = in.u32();
            sample.frequency = in.s32();
            sample.defaultVolume = in.u16();
            sample.defaultPan = in.s16();
            sample.defaultPriority = in.u16();
            sample.channels = in.u16();
            sample.minDistance = in.f32();
            sample.maxDistance = in.f32();
            in.skip(8);
            sample.codec = codecFromMode(sample.mode);
            const std::size_t end = start + size;
            in.need(end - in.pos());
            if (sample.codec == SampleCodec::Xma) {
                readXmaExtra(in, end, sample);
            }
            in.seek(end);
        }
        if (sample.channels == 0) {
            in.fail(std::format("sample {} ('{}') has no channels", i, sample.name));
        }
        dataCursor = (dataCursor + kDataAlignment - 1) / kDataAlignment * kDataAlignment;
        sample.dataOffset = dataStart + dataCursor;
        if (dataCursor + sample.compressedSize > static_cast<std::size_t>(dataSize)) {
            in.fail(std::format("sample {} ('{}') data 0x{:x}+0x{:x} runs past the data block (0x{:x})", i,
                sample.name, dataCursor, sample.compressedSize, dataSize));
        }
        dataCursor += sample.compressedSize;
        bank.m_samples.push_back(std::move(sample));
    }
    if (in.pos() > dataStart) {
        in.fail(std::format("sample headers end at 0x{:x}, past their declared end 0x{:x}", in.pos(), dataStart));
    }
    return bank;
}

const FsbSample& FsbBank::sample(std::size_t index) const
{
    if (index >= m_samples.size()) {
        throw DataError("{}: sample {} out of range ({} samples)", m_source, index, m_samples.size());
    }
    return m_samples[index];
}

ByteView FsbBank::sampleData(std::size_t index) const
{
    const FsbSample& s = sample(index);
    return ByteView(m_data).subspan(s.dataOffset, s.compressedSize);
}

std::optional<std::size_t> FsbBank::findSample(std::string_view name) const
{
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        if (sampleHasName(i, name)) {
            return i;
        }
    }
    return std::nullopt;
}

bool FsbBank::sampleHasName(std::size_t index, std::string_view name) const
{
    const std::string& have = sample(index).name;
    if (iequals(stemOf(have), stemOf(name))) {
        return true;
    }
    // A stored name that filled the header is a prefix of the full name.
    return have.size() >= kNameLength - 1 && startsWithNoCase(baseName(name), have);
}

} // namespace fh::audio
