#include "Decode.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace openhorizon::audio {

namespace {

constexpr std::size_t kXmaPacketSize = 2048;

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

std::string ffmpegError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

/// Appends a decoded frame to interleaved float output, whatever sample
/// format the decoder chose.
void appendFrame(const AVFrame& frame, std::vector<float>& out)
{
    const int channels = frame.ch_layout.nb_channels;
    const int count = frame.nb_samples;
    const auto format = static_cast<AVSampleFormat>(frame.format);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(count) * static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        for (int i = 0; i < count; ++i) {
            const int plane = planar ? c : 0;
            const int index = planar ? i : i * channels + c;
            float value = 0.0F;
            switch (format) {
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                value = reinterpret_cast<const float*>(frame.extended_data[plane])[index];
                break;
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                value = static_cast<float>(reinterpret_cast<const std::int16_t*>(frame.extended_data[plane])[index])
                    / 32768.0F;
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                value = static_cast<float>(reinterpret_cast<const std::int32_t*>(frame.extended_data[plane])[index])
                    / 2147483648.0F;
                break;
            default:
                throw DataError("decoder produced unsupported sample format {}", av_get_sample_fmt_name(format));
            }
            out[base + static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)]
                = value;
        }
    }
}

/// One FFmpeg decoder session fed packet by packet.
class FfmpegStream {
public:
    FfmpegStream(AVCodecID id, int sampleRate, int channels, std::span<const std::uint8_t> extradata, int blockAlign,
        std::string source)
        : m_source(std::move(source))
    {
        const AVCodec* codec = avcodec_find_decoder(id);
        if (!codec) {
            throw DataError("{}: FFmpeg has no {} decoder", m_source, avcodec_get_name(id));
        }
        m_context.reset(avcodec_alloc_context3(codec));
        m_packet.reset(av_packet_alloc());
        m_frame.reset(av_frame_alloc());
        if (!m_context || !m_packet || !m_frame) {
            throw DataError("{}: out of memory creating the decoder", m_source);
        }
        m_context->sample_rate = sampleRate;
        m_context->block_align = blockAlign;
        m_context->pkt_timebase = AVRational{1, sampleRate > 0 ? sampleRate : 1};
        av_channel_layout_default(&m_context->ch_layout, channels);
        if (!extradata.empty()) {
            // FFmpeg frees extradata with the context, so it must come from av_malloc.
            auto* copy = static_cast<std::uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!copy) {
                throw DataError("{}: out of memory for codec extradata", m_source);
            }
            std::memcpy(copy, extradata.data(), extradata.size());
            m_context->extradata = copy;
            m_context->extradata_size = static_cast<int>(extradata.size());
        }
        const int opened = avcodec_open2(m_context.get(), codec, nullptr);
        if (opened < 0) {
            throw DataError("{}: cannot open the {} decoder: {}", m_source, codec->name, ffmpegError(opened));
        }
    }

    void decode(ByteView packet)
    {
        if (av_new_packet(m_packet.get(), static_cast<int>(packet.size())) < 0) {
            throw DataError("{}: out of memory for a packet", m_source);
        }
        std::memcpy(m_packet->data, packet.data(), packet.size());
        // Without a first timestamp FFmpeg warns about the decoder delay it skips.
        m_packet->pts = m_sentPackets == 0 ? 0 : AV_NOPTS_VALUE;
        ++m_sentPackets;
        int sent = avcodec_send_packet(m_context.get(), m_packet.get());
        if (sent == AVERROR(EAGAIN)) {
            drain();
            sent = avcodec_send_packet(m_context.get(), m_packet.get());
        }
        av_packet_unref(m_packet.get());
        if (sent < 0) {
            throw DataError("{}: packet {} rejected: {}", m_source, m_sentPackets - 1, ffmpegError(sent));
        }
        drain();
    }

    void finish()
    {
        const int sent = avcodec_send_packet(m_context.get(), nullptr);
        if (sent < 0 && sent != AVERROR_EOF) {
            throw DataError("{}: flushing the decoder failed: {}", m_source, ffmpegError(sent));
        }
        drain();
    }

    std::vector<float>& samples() { return m_samples; }
    int channels() const { return m_channels; }
    int sampleRate() const { return m_sampleRate; }

private:
    void drain()
    {
        while (true) {
            const int received = avcodec_receive_frame(m_context.get(), m_frame.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                return;
            }
            if (received < 0) {
                throw DataError("{}: decoding failed: {}", m_source, ffmpegError(received));
            }
            if (m_channels == 0) {
                m_channels = m_frame->ch_layout.nb_channels;
                m_sampleRate = m_frame->sample_rate;
            } else if (m_frame->ch_layout.nb_channels != m_channels) {
                throw DataError("{}: channel count changed from {} to {} mid-stream", m_source, m_channels,
                    m_frame->ch_layout.nb_channels);
            }
            appendFrame(*m_frame, m_samples);
            av_frame_unref(m_frame.get());
        }
    }

    std::string m_source;
    std::unique_ptr<AVCodecContext, CodecContextDeleter> m_context;
    std::unique_ptr<AVPacket, PacketDeleter> m_packet;
    std::unique_ptr<AVFrame, FrameDeleter> m_frame;
    std::vector<float> m_samples;
    int m_channels = 0;
    int m_sampleRate = 0;
    std::size_t m_sentPackets = 0;
};

/// The 34-byte XMA2WAVEFORMATEX extension FFmpeg's XMA2 decoder reads:
/// u16 stream count, u32 channel mask, then fields it ignores for decoding.
std::array<std::uint8_t, 34> xma2Extradata(const FsbSample& sample)
{
    std::array<std::uint8_t, 34> extra{};
    const auto put16 = [&](std::size_t at, std::uint32_t v) {
        extra[at] = static_cast<std::uint8_t>(v);
        extra[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        put16(at, v & 0xFFFFU);
        put16(at + 2, v >> 16);
    };
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, sample.channels);
    const std::uint64_t mask = layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
    av_channel_layout_uninit(&layout);
    put16(0, (sample.channels + 1U) / 2U);
    put32(2, static_cast<std::uint32_t>(mask));
    put32(6, sample.sampleCount);
    put32(10, 0x8000);
    put32(18, sample.sampleCount);
    put16(32, static_cast<std::uint32_t>((sample.compressedSize + 0x7FFFU) / 0x8000U));
    return extra;
}

std::vector<float> decodeXma(const FsbBank& bank, std::size_t index, int& channels)
{
    const FsbSample& sample = bank.sample(index);
    const ByteView data = bank.sampleData(index);
    const std::string source = std::format("{} sample {} ('{}')", bank.source(), index, sample.name);
    if (data.size() % kXmaPacketSize != 0) {
        throw DataError("{}: XMA data of {} bytes is not whole 2048-byte packets", source, data.size());
    }
    const auto extra = xma2Extradata(sample);
    FfmpegStream stream(AV_CODEC_ID_XMA2, sample.frequency, sample.channels, extra, static_cast<int>(kXmaPacketSize),
        source);
    for (std::size_t offset = 0; offset < data.size(); offset += kXmaPacketSize) {
        stream.decode(data.subspan(offset, kXmaPacketSize));
    }
    stream.finish();
    channels = stream.channels();
    return std::move(stream.samples());
}

struct MpegFrame {
    AVCodecID codec = AV_CODEC_ID_NONE;
    std::size_t size = 0;
    /// Decoded frames per MPEG frame (ISO 11172-3 / 13818-3).
    std::size_t samples = 0;
};

/// The frame starting at `p`, if the four bytes there are a valid MPEG audio
/// frame header (ISO 11172-3 / 13818-3 layout).
std::optional<MpegFrame> mpegFrameAt(const std::uint8_t* p)
{
    static constexpr std::array<std::array<int, 16>, 5> kBitrates{{
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    }};
    static constexpr std::array<int, 3> kRates{44100, 48000, 32000};
    const std::uint32_t h = loadBe32(p);
    if ((h & 0xFFE00000U) != 0xFFE00000U) {
        return std::nullopt;
    }
    const std::uint32_t versionBits = (h >> 19) & 3U;
    const std::uint32_t layerBits = (h >> 17) & 3U;
    const std::uint32_t bitrateIndex = (h >> 12) & 0xFU;
    const std::uint32_t rateIndex = (h >> 10) & 3U;
    if (versionBits == 1 || layerBits == 0 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) {
        return std::nullopt;
    }
    const bool mpeg1 = versionBits == 3;
    const int layer = 4 - static_cast<int>(layerBits);
    const int table = mpeg1 ? layer - 1 : (layer == 1 ? 3 : 4);
    const long bitrate = kBitrates[static_cast<std::size_t>(table)][bitrateIndex] * 1000L;
    const int divisor = versionBits == 3 ? 1 : (versionBits == 2 ? 2 : 4);
    const long rate = kRates[rateIndex] / divisor;
    const long padding = (h >> 9) & 1U;
    MpegFrame frame;
    if (layer == 1) {
        frame.size = static_cast<std::size_t>((12 * bitrate / rate + padding) * 4);
        frame.codec = AV_CODEC_ID_MP1;
        frame.samples = 384;
    } else if (layer == 2) {
        frame.size = static_cast<std::size_t>(144 * bitrate / rate + padding);
        frame.codec = AV_CODEC_ID_MP2;
        frame.samples = 1152;
    } else {
        frame.size = static_cast<std::size_t>((mpeg1 ? 144 : 72) * bitrate / rate + padding);
        frame.codec = AV_CODEC_ID_MP3;
        frame.samples = mpeg1 ? 1152 : 576;
    }
    return frame;
}

std::vector<float> decodeMpeg(const FsbBank& bank, std::size_t index, int& channels)
{
    const FsbSample& sample = bank.sample(index);
    const ByteView data = bank.sampleData(index);
    const std::string source = std::format("{} sample {} ('{}')", bank.source(), index, sample.name);
    std::optional<FfmpegStream> stream;
    std::size_t pos = 0;
    while (pos + 4 <= data.size()) {
        const std::optional<MpegFrame> frame = mpegFrameAt(data.data() + pos);
        if (!frame || pos + frame->size > data.size()) {
            ++pos;
            continue;
        }
        if (!stream) {
            stream.emplace(frame->codec, sample.frequency, sample.channels, std::span<const std::uint8_t>{}, 0, source);
        }
        stream->decode(data.subspan(pos, frame->size));
        pos += frame->size;
    }
    if (!stream) {
        throw DataError("{}: no MPEG frame in {} bytes", source, data.size());
    }
    stream->finish();
    channels = stream->channels();
    return std::move(stream->samples());
}

std::vector<float> decodePcm(const FsbBank& bank, std::size_t index)
{
    const FsbSample& sample = bank.sample(index);
    const ByteView data = bank.sampleData(index);
    const std::size_t values = static_cast<std::size_t>(sample.sampleCount) * sample.channels;
    std::vector<float> out(values);
    if (sample.codec == SampleCodec::Pcm8) {
        if (data.size() < values) {
            throw DataError("{}: sample {} needs {} bytes of 8-bit PCM, has {}", bank.source(), index, values,
                data.size());
        }
        for (std::size_t i = 0; i < values; ++i) {
            const int v = sample.pcmUnsigned() ? static_cast<int>(data[i]) - 128 : static_cast<std::int8_t>(data[i]);
            out[i] = static_cast<float>(v) / 128.0F;
        }
        return out;
    }
    if (data.size() < values * 2) {
        throw DataError("{}: sample {} needs {} bytes of 16-bit PCM, has {}", bank.source(), index, values * 2,
            data.size());
    }
    for (std::size_t i = 0; i < values; ++i) {
        const std::uint16_t raw = bank.bigEndianPcm() ? loadBe16(data.data() + i * 2) : loadLe16(data.data() + i * 2);
        out[i] = static_cast<float>(static_cast<std::int16_t>(raw)) / 32768.0F;
    }
    return out;
}

} // namespace

struct MpegSampleStream::Impl {
    static constexpr std::size_t kReadSize = 64 * 1024;

    FsbSample sample;
    ByteSource read;
    std::string source;
    std::optional<FfmpegStream> decoder;
    /// Bytes of the sample's data from `bufferStart` on.
    Bytes buffer;
    std::size_t bufferStart = 0;
    /// Scan position in the sample's data.
    std::size_t pos = 0;
    std::size_t position = 0;
    /// Decoded frames still to drop before `position` is reached.
    std::size_t discard = 0;
    bool ended = false;

    /// Makes bytes [at, at + count) of the sample's data available and
    /// returns a pointer to them, or nullptr past the end of the data.
    const std::uint8_t* bytesAt(std::size_t at, std::size_t count)
    {
        if (at + count > sample.compressedSize) {
            return nullptr;
        }
        if (at < bufferStart || at + count > bufferStart + buffer.size()) {
            const std::size_t length = std::min<std::size_t>(std::max(kReadSize, count), sample.compressedSize - at);
            buffer.resize(length);
            const std::size_t got = read(sample.dataOffset + at, buffer);
            if (got < count) {
                throw DataError("{}: the file ends inside the sample's data (offset 0x{:x})", source,
                    sample.dataOffset + at);
            }
            buffer.resize(got);
            bufferStart = at;
        }
        return buffer.data() + (at - bufferStart);
    }

    /// The next MPEG frame at or after `pos`, skipping the padding FSB puts
    /// between frames, as decodeMpeg() finds them.
    std::optional<MpegFrame> nextFrame()
    {
        while (const std::uint8_t* header = bytesAt(pos, 4)) {
            const std::optional<MpegFrame> frame = mpegFrameAt(header);
            if (frame && pos + frame->size <= sample.compressedSize) {
                return frame;
            }
            ++pos;
        }
        return std::nullopt;
    }
};

MpegSampleStream::MpegSampleStream(const FsbSample& sample, ByteSource read, std::string source,
    std::size_t startFrame)
    : m_impl(std::make_unique<Impl>())
{
    Impl& s = *m_impl;
    s.sample = sample;
    s.read = std::move(read);
    s.source = std::move(source);
    if (sample.codec != SampleCodec::Mpeg) {
        throw DataError("{}: streaming decodes MPEG samples only, this one is {}", s.source, codecName(sample.codec));
    }
    if (startFrame > sample.sampleCount) {
        throw DataError("{}: cannot start at frame {} of {}", s.source, startFrame, sample.sampleCount);
    }
    s.position = startFrame;
    // Only headers are read up to the MPEG frame that decoding starts at.
    std::size_t decodedStart = 0;
    while (true) {
        const std::optional<MpegFrame> frame = s.nextFrame();
        if (!frame) {
            break;
        }
        const std::size_t firstNeeded = startFrame / frame->samples;
        const std::size_t decodeFrom = firstNeeded > kPrerollFrames ? firstNeeded - kPrerollFrames : 0;
        if (decodedStart / frame->samples >= decodeFrom) {
            break;
        }
        decodedStart += frame->samples;
        s.pos += frame->size;
    }
    s.discard = startFrame - decodedStart;
    s.ended = startFrame >= sample.sampleCount;
}

MpegSampleStream::~MpegSampleStream() = default;
MpegSampleStream::MpegSampleStream(MpegSampleStream&&) noexcept = default;
MpegSampleStream& MpegSampleStream::operator=(MpegSampleStream&&) noexcept = default;

int MpegSampleStream::channels() const
{
    return m_impl->sample.channels;
}

int MpegSampleStream::sampleRate() const
{
    return m_impl->sample.frequency;
}

std::size_t MpegSampleStream::totalFrames() const
{
    return m_impl->sample.sampleCount;
}

std::size_t MpegSampleStream::position() const
{
    return m_impl->position;
}

bool MpegSampleStream::finished() const
{
    return m_impl->ended;
}

std::size_t MpegSampleStream::decode(std::size_t minFrames, std::vector<float>& out)
{
    Impl& s = *m_impl;
    const auto channels = static_cast<std::size_t>(s.sample.channels);
    std::size_t appended = 0;
    while (!s.ended && appended < minFrames) {
        const std::optional<MpegFrame> frame = s.nextFrame();
        if (!frame && !s.decoder) {
            throw DataError("{}: no MPEG frame in {} bytes", s.source, s.sample.compressedSize);
        }
        if (frame) {
            if (!s.decoder) {
                s.decoder.emplace(frame->codec, s.sample.frequency, s.sample.channels, std::span<const std::uint8_t>{},
                    0, s.source);
            }
            s.decoder->decode(ByteView(s.bytesAt(s.pos, frame->size), frame->size));
            s.pos += frame->size;
        } else {
            s.decoder->finish();
        }
        if (s.decoder->channels() != 0 && static_cast<std::size_t>(s.decoder->channels()) != channels) {
            throw DataError("{}: decoded to {} channels, the header says {}", s.source, s.decoder->channels(),
                channels);
        }
        std::vector<float>& decoded = s.decoder->samples();
        const std::size_t frames = decoded.size() / channels;
        const std::size_t dropped = std::min(frames, s.discard);
        s.discard -= dropped;
        const std::size_t keep = std::min(frames - dropped, s.sample.sampleCount - s.position);
        out.insert(out.end(), decoded.begin() + static_cast<std::ptrdiff_t>(dropped * channels),
            decoded.begin() + static_cast<std::ptrdiff_t>((dropped + keep) * channels));
        decoded.clear();
        s.position += keep;
        appended += keep;
        if (s.position >= s.sample.sampleCount) {
            s.ended = true;
        } else if (!frame) {
            throw DataError("{}: the data ends at frame {}, the header says {}", s.source, s.position,
                s.sample.sampleCount);
        }
    }
    return appended;
}

PcmBuffer decodeSample(const FsbBank& bank, std::size_t index)
{
    DecodeReport report;
    return decodeSample(bank, index, report);
}

PcmBuffer decodeSample(const FsbBank& bank, std::size_t index, DecodeReport& report)
{
    const FsbSample& sample = bank.sample(index);
    PcmBuffer pcm;
    pcm.sampleRate = sample.frequency;
    pcm.channels = sample.channels;
    int channels = sample.channels;
    switch (sample.codec) {
    case SampleCodec::Xma:
        pcm.samples = decodeXma(bank, index, channels);
        break;
    case SampleCodec::Mpeg:
        pcm.samples = decodeMpeg(bank, index, channels);
        break;
    case SampleCodec::Pcm8:
    case SampleCodec::Pcm16:
        pcm.samples = decodePcm(bank, index);
        break;
    case SampleCodec::ImaAdpcm:
    case SampleCodec::Vag:
    case SampleCodec::GcAdpcm:
        throw DataError("{}: sample {} ('{}') uses {}, which no bank on the disc uses and this reader does not decode",
            bank.source(), index, sample.name, codecName(sample.codec));
    }
    if (channels != sample.channels) {
        throw DataError("{}: sample {} ('{}') decoded to {} channels, the header says {}", bank.source(), index,
            sample.name, channels, sample.channels);
    }
    report.headerFrames = sample.sampleCount;
    report.codecFrames = pcm.frames();
    if (report.codecFrames < report.headerFrames) {
        throw DataError("{}: sample {} ('{}') decoded to {} frames, the header says {}", bank.source(), index,
            sample.name, report.codecFrames, report.headerFrames);
    }
    pcm.samples.resize(static_cast<std::size_t>(sample.sampleCount) * sample.channels);
    pcm.loopStart = std::min<std::size_t>(sample.loopStart, sample.sampleCount);
    pcm.loopEnd = std::clamp<std::size_t>(static_cast<std::size_t>(sample.loopEnd) + 1, pcm.loopStart,
        sample.sampleCount);
    if (pcm.loopEnd <= pcm.loopStart) {
        pcm.loopStart = 0;
        pcm.loopEnd = sample.sampleCount;
    }
    return pcm;
}

} // namespace openhorizon::audio
