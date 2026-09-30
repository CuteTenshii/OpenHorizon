#include "Wav.h"

#include "base/Host.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <string_view>

namespace openhorizon::audio {

namespace {

void put16(Bytes& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(Bytes& out, std::uint32_t v)
{
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16);
}

void putTag(Bytes& out, std::string_view tag)
{
    out.insert(out.end(), tag.begin(), tag.end());
}

} // namespace

Bytes encodeWav(const PcmBuffer& pcm, WavFormat format)
{
    const std::uint32_t bytesPerSample = format == WavFormat::Pcm16 ? 2 : 4;
    const std::uint32_t channels = static_cast<std::uint32_t>(pcm.channels);
    const std::uint32_t rate = static_cast<std::uint32_t>(pcm.sampleRate);
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.samples.size()) * bytesPerSample;

    Bytes out;
    out.reserve(44 + dataBytes);
    putTag(out, "RIFF");
    put32(out, 36 + dataBytes);
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    put32(out, 16);
    put16(out, format == WavFormat::Pcm16 ? 1 : 3);
    put16(out, channels);
    put32(out, rate);
    put32(out, rate * channels * bytesPerSample);
    put16(out, channels * bytesPerSample);
    put16(out, bytesPerSample * 8);
    putTag(out, "data");
    put32(out, dataBytes);
    for (const float sample : pcm.samples) {
        if (format == WavFormat::Pcm16) {
            const float clamped = std::clamp(sample, -1.0F, 1.0F);
            const auto value = static_cast<std::int16_t>(std::lround(clamped * 32767.0F));
            put16(out, static_cast<std::uint16_t>(value));
        } else {
            put32(out, std::bit_cast<std::uint32_t>(sample));
        }
    }
    return out;
}

void writeWav(const std::filesystem::path& path, const PcmBuffer& pcm, WavFormat format)
{
    const Bytes data = encodeWav(pcm, format);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) {
        throw DataError("cannot write {}", pathToUtf8(path));
    }
}

} // namespace openhorizon::audio
