#include "MovieRenderer.h"

#include "UiRenderer.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace openhorizon::ui {

namespace {

std::string ffmpegError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(code, text.data(), text.size());
    return text.data();
}

struct FormatDeleter {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};
struct CodecDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct IoDeleter {
    void operator()(AVIOContext* io) const
    {
        if (io) {
            av_freep(static_cast<void*>(&io->buffer));
            avio_context_free(&io);
        }
    }
};

/// A movie held in memory, for one that only an archive provides.
struct MemoryFile {
    Bytes data;
    std::int64_t position = 0;

    static int read(void* opaque, std::uint8_t* buffer, int size)
    {
        auto& file = *static_cast<MemoryFile*>(opaque);
        const std::int64_t left = static_cast<std::int64_t>(file.data.size()) - file.position;
        if (left <= 0) {
            return AVERROR_EOF;
        }
        const int count = static_cast<int>(std::min<std::int64_t>(left, size));
        std::memcpy(buffer, file.data.data() + file.position, static_cast<std::size_t>(count));
        file.position += count;
        return count;
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int whence)
    {
        auto& file = *static_cast<MemoryFile*>(opaque);
        const auto size = static_cast<std::int64_t>(file.data.size());
        switch (whence & ~AVSEEK_FORCE) {
        case AVSEEK_SIZE:
            return size;
        case SEEK_SET:
            file.position = offset;
            break;
        case SEEK_CUR:
            file.position += offset;
            break;
        case SEEK_END:
            file.position = size + offset;
            break;
        default:
            return -1;
        }
        file.position = std::clamp<std::int64_t>(file.position, 0, size);
        return file.position;
    }
};

/// BT.709 limited range to RGB, in 16.16 fixed point: Y from 16..235,
/// Cb and Cr from 16..240 around 128.
std::uint8_t clampByte(int value)
{
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void yuv420ToRgba(const AVFrame& frame, std::vector<std::uint8_t>& out)
{
    constexpr int kY = 76309;
    constexpr int kRv = 117489;
    constexpr int kGu = -13975;
    constexpr int kGv = -34925;
    constexpr int kBu = 138438;
    const int width = frame.width;
    const int height = frame.height;
    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* lumaRow = frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0];
        const std::uint8_t* cbRow = frame.data[1] + static_cast<std::ptrdiff_t>(y / 2) * frame.linesize[1];
        const std::uint8_t* crRow = frame.data[2] + static_cast<std::ptrdiff_t>(y / 2) * frame.linesize[2];
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const int luma = (static_cast<int>(lumaRow[x]) - 16) * kY;
            const int cb = static_cast<int>(cbRow[x / 2]) - 128;
            const int cr = static_cast<int>(crRow[x / 2]) - 128;
            dst[0] = clampByte((luma + kRv * cr + 32768) >> 16);
            dst[1] = clampByte((luma + kGu * cb + kGv * cr + 32768) >> 16);
            dst[2] = clampByte((luma + kBu * cb + 32768) >> 16);
            dst[3] = 255;
            dst += 4;
        }
    }
}

std::string definitionPath(std::string_view gamePath)
{
    const std::size_t dot = gamePath.rfind('.');
    return std::string(gamePath.substr(0, dot)) + ".def";
}

} // namespace

MovieStreams MovieStreams::parse(std::string_view text, std::string_view source)
{
    pugi::xml_document document;
    if (!document.load_buffer(text.data(), text.size())) {
        throw DataError("{}: not XML", source);
    }
    MovieStreams streams;
    // The files wrap the definition in an <xml> element.
    pugi::xml_node definition = document.child("xml").child("StreamDefinition");
    if (!definition) {
        definition = document.child("StreamDefinition");
    }
    for (const pugi::xml_node node : definition.children()) {
        const std::string_view name = node.name();
        if (name == "Video") {
            streams.video = node.attribute("id").as_int();
        } else if (name == "Audio") {
            streams.audio.push_back({node.attribute("id").as_int(), node.attribute("lang").as_string()});
        }
    }
    return streams;
}

struct MovieRenderer::Impl {
    std::string source;
    bool loop = false;
    std::unique_ptr<MemoryFile> memory;
    std::unique_ptr<AVIOContext, IoDeleter> io;
    std::unique_ptr<AVFormatContext, FormatDeleter> format;
    std::unique_ptr<AVCodecContext, CodecDeleter> codec;
    std::unique_ptr<AVPacket, PacketDeleter> packet;
    std::unique_ptr<AVFrame, FrameDeleter> frame;
    int stream = -1;
    double timeBase = 0.0;
    double length = 0.0;

    UiTexture texture;
    std::vector<std::uint8_t> rgba;
    int frameWidth = 0;
    int frameHeight = 0;
    double clock = 0.0;
    /// Timestamp of the frame shown, and of the one decoded next if any.
    double shownTime = -1.0;
    bool pending = false;
    double pendingTime = 0.0;
    bool endOfStream = false;
    bool finished = false;
    long decoded = 0;

    void open(const Vfs& vfs, std::string_view gamePath);
    /// Decodes the next frame into `frame`; false at the end of the stream.
    bool decodeNext();
    double frameTime() const;
    void rewind();
    /// Converts the decoded frame to RGBA.
    void convert();
    void upload();
};

void MovieRenderer::Impl::open(const Vfs& vfs, std::string_view gamePath)
{
    AVFormatContext* context = avformat_alloc_context();
    if (!context) {
        throw DataError("{}: out of memory", source);
    }
    int opened = 0;
    if (const std::optional<std::filesystem::path> host = vfs.hostPath(gamePath)) {
        opened = avformat_open_input(&context, host->string().c_str(), nullptr, nullptr);
    } else {
        memory = std::make_unique<MemoryFile>();
        memory->data = vfs.readOrThrow(gamePath);
        constexpr int kBufferSize = 1 << 16;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(kBufferSize));
        if (!buffer) {
            avformat_free_context(context);
            throw DataError("{}: out of memory", source);
        }
        io.reset(
            avio_alloc_context(buffer, kBufferSize, 0, memory.get(), &MemoryFile::read, nullptr, &MemoryFile::seek));
        if (!io) {
            av_free(buffer);
            avformat_free_context(context);
            throw DataError("{}: out of memory", source);
        }
        context->pb = io.get();
        opened = avformat_open_input(&context, nullptr, nullptr, nullptr);
    }
    if (opened < 0) {
        // avformat_open_input frees the context on failure.
        throw DataError("{}: {}", source, ffmpegError(opened));
    }
    format.reset(context);
    if (const int found = avformat_find_stream_info(format.get(), nullptr); found < 0) {
        throw DataError("{}: {}", source, ffmpegError(found));
    }

    std::optional<int> wanted;
    if (const std::optional<std::string> def = vfs.readText(definitionPath(gamePath))) {
        wanted = MovieStreams::parse(*def, definitionPath(gamePath)).video;
    }
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream& candidate = *format->streams[i];
        if (candidate.codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        // The ASF demuxer numbers its streams as the file does, which is
        // what the .def's ids refer to.
        if (!wanted || candidate.id == *wanted) {
            stream = static_cast<int>(i);
            break;
        }
    }
    if (stream < 0) {
        throw DataError("{}: no video stream{}", source, wanted ? std::format(" with id {}", *wanted) : "");
    }
    const AVStream& video = *format->streams[static_cast<std::size_t>(stream)];
    timeBase = av_q2d(video.time_base);
    length = format->duration > 0 ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;

    const AVCodec* decoder = avcodec_find_decoder(video.codecpar->codec_id);
    if (!decoder) {
        throw DataError("{}: FFmpeg has no {} decoder", source, avcodec_get_name(video.codecpar->codec_id));
    }
    codec.reset(avcodec_alloc_context3(decoder));
    packet.reset(av_packet_alloc());
    frame.reset(av_frame_alloc());
    if (!codec || !packet || !frame) {
        throw DataError("{}: out of memory creating the decoder", source);
    }
    if (const int copied = avcodec_parameters_to_context(codec.get(), video.codecpar); copied < 0) {
        throw DataError("{}: {}", source, ffmpegError(copied));
    }
    codec->pkt_timebase = video.time_base;
    if (const int ready = avcodec_open2(codec.get(), decoder, nullptr); ready < 0) {
        throw DataError("{}: cannot open the {} decoder: {}", source, decoder->name, ffmpegError(ready));
    }
}

bool MovieRenderer::Impl::decodeNext()
{
    for (;;) {
        const int received = avcodec_receive_frame(codec.get(), frame.get());
        if (received == 0) {
            ++decoded;
            return true;
        }
        if (received == AVERROR_EOF) {
            return false;
        }
        if (received != AVERROR(EAGAIN)) {
            throw DataError("{}: {}", source, ffmpegError(received));
        }
        if (endOfStream) {
            return false;
        }
        const int read = av_read_frame(format.get(), packet.get());
        if (read == AVERROR_EOF) {
            endOfStream = true;
            avcodec_send_packet(codec.get(), nullptr);
            continue;
        }
        if (read < 0) {
            throw DataError("{}: {}", source, ffmpegError(read));
        }
        if (packet->stream_index == stream) {
            const int sent = avcodec_send_packet(codec.get(), packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                av_packet_unref(packet.get());
                throw DataError("{}: {}", source, ffmpegError(sent));
            }
        }
        av_packet_unref(packet.get());
    }
}

double MovieRenderer::Impl::frameTime() const
{
    const std::int64_t pts = frame->best_effort_timestamp;
    return pts == AV_NOPTS_VALUE ? shownTime + 1.0 / 30.0 : static_cast<double>(pts) * timeBase;
}

void MovieRenderer::Impl::rewind()
{
    const int sought = av_seek_frame(format.get(), stream, 0, AVSEEK_FLAG_BACKWARD);
    if (sought < 0) {
        throw DataError("{}: cannot rewind: {}", source, ffmpegError(sought));
    }
    avcodec_flush_buffers(codec.get());
    endOfStream = false;
    pending = false;
    shownTime = -1.0;
    clock = 0.0;
}

void MovieRenderer::Impl::convert()
{
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
        throw DataError("{}: unsupported pixel format {}", source, frame->format);
    }
    yuv420ToRgba(*frame, rgba);
    frameWidth = frame->width;
    frameHeight = frame->height;
}

void MovieRenderer::Impl::upload()
{
    if (texture.id() == 0 || texture.width() != frameWidth || texture.height() != frameHeight) {
        texture = UiTexture::fromRgba(frameWidth, frameHeight, rgba.data(), false);
        glTextureParameteri(texture.id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(texture.id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(texture.id(), 0, 0, 0, frameWidth, frameHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

MovieRenderer::MovieRenderer(const Vfs& vfs, std::string_view gamePath, bool loop)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->source = std::string(gamePath);
    m_impl->loop = loop;
    m_impl->open(vfs, gamePath);
    // The first frame shows at once, as an autoplaying movie starts.
    update(0.0F);
}

MovieRenderer::~MovieRenderer() = default;

MovieInfo MovieRenderer::probe(const Vfs& vfs, std::string_view gamePath)
{
    Impl impl;
    impl.source = std::string(gamePath);
    impl.open(vfs, gamePath);
    MovieInfo info;
    const AVStream& video = *impl.format->streams[static_cast<std::size_t>(impl.stream)];
    info.streamId = video.id;
    info.codec = avcodec_get_name(video.codecpar->codec_id);
    info.width = video.codecpar->width;
    info.height = video.codecpar->height;
    info.duration = impl.length;
    if (!impl.decodeNext()) {
        throw DataError("{}: no frame", impl.source);
    }
    info.firstFrameTime = impl.frameTime();
    return info;
}

void MovieRenderer::restart()
{
    m_impl->rewind();
    m_impl->finished = false;
    update(0.0F);
}

void MovieRenderer::update(float dt)
{
    Impl& impl = *m_impl;
    if (impl.finished) {
        return;
    }
    impl.clock += static_cast<double>(dt);
    bool changed = false;
    // Frames are shown when the clock reaches their timestamp; of several
    // reached in one update only the last is uploaded.
    for (;;) {
        if (!impl.pending) {
            if (!impl.decodeNext()) {
                if (!impl.loop) {
                    impl.finished = true;
                    break;
                }
                const double overshoot = std::max(0.0, impl.clock - std::max(impl.length, impl.shownTime));
                impl.rewind();
                impl.clock = overshoot;
                if (!impl.decodeNext()) {
                    impl.finished = true;
                    break;
                }
            }
            impl.pending = true;
            impl.pendingTime = impl.frameTime();
        }
        if (impl.shownTime >= 0.0 && impl.pendingTime > impl.clock) {
            break;
        }
        impl.shownTime = impl.pendingTime;
        impl.pending = false;
        impl.convert();
        changed = true;
    }
    if (changed) {
        impl.upload();
    }
}

GLuint MovieRenderer::texture() const
{
    return m_impl->texture.id();
}

int MovieRenderer::width() const
{
    return m_impl->texture.width();
}

int MovieRenderer::height() const
{
    return m_impl->texture.height();
}

double MovieRenderer::time() const
{
    return std::max(m_impl->shownTime, 0.0);
}

double MovieRenderer::duration() const
{
    return m_impl->length;
}

long MovieRenderer::framesDecoded() const
{
    return m_impl->decoded;
}

bool MovieRenderer::finished() const
{
    return m_impl->finished;
}

const std::vector<std::uint8_t>& MovieRenderer::pixels() const
{
    return m_impl->rgba;
}

} // namespace openhorizon::ui
