#pragma once

#include "render/Gl.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::ui {

/// The streams a movie's .def file (media/ui/videos/<name>.def, read by the
/// scene resource loader through its "StreamDefinition" and ".def" strings)
/// selects: the video stream and, per language, an audio stream, each by
/// its ASF stream number. PressStart.def lists only <Video id="2" />, so
/// that movie plays without sound; FMV_01.def lists six audio languages
/// and video 7.
struct MovieStreams {
    std::optional<int> video;
    struct Audio {
        int id = 0;
        std::string language;
    };
    std::vector<Audio> audio;

    /// Parses a .def file's text. Throws DataError when it is not XML.
    static MovieStreams parse(std::string_view text, std::string_view source);
};

/// What probing a movie finds, without an OpenGL context.
struct MovieInfo {
    /// The ASF stream number of the video stream chosen.
    int streamId = 0;
    std::string codec;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    /// Timestamp of the first frame decoded.
    double firstFrameTime = 0.0;
};

/// The picture of a movie a scene's CUSTREND_MOVIERENDERER contract plays
/// (VIDEOPATH, VIDEOLOOP, VIDEOAUTOPLAY): the disc's movies are ASF files
/// with a WMV3 (VC-1 Main profile) video stream, decoded here with FFmpeg,
/// frame by frame at their timestamps, into an RGBA texture the UI renderer
/// samples in place of the viewport model's texture.
///
/// Colour: the movie renderer's pixel shader takes a flag choosing BT.709
/// decoding (the "videodecode709" setting, byte 0x8338f058, 1 unless the
/// command line clears it, passed to the shader at 0x82f29aec); the
/// standard BT.709 limited-range matrix is used here, the shader's own
/// constants not having been read. The contract's VIDEOBRIGHTNESS,
/// VIDEOCONTRAST (0.5), VIDEOSATURATION (1) and VIDEOBIAS* (0) values are
/// taken as leaving the picture unchanged, which was not traced. The
/// movies' audio is not played: the title movie selects none.
///
/// Construct with a current OpenGL 4.5 context.
class MovieRenderer {
public:
    /// Opens `gamePath` (such as "GAME:\Media\UI\Videos\PressStart.wmv")
    /// and its .def. Throws DataError when the file is missing or FFmpeg
    /// cannot read it or has no decoder for its video.
    MovieRenderer(const Vfs& vfs, std::string_view gamePath, bool loop);
    ~MovieRenderer();
    MovieRenderer(const MovieRenderer&) = delete;
    MovieRenderer& operator=(const MovieRenderer&) = delete;

    /// Opens a movie as the constructor does and decodes its first frame,
    /// with no texture. Throws DataError as the constructor does.
    static MovieInfo probe(const Vfs& vfs, std::string_view gamePath);

    /// Starts from the first frame.
    void restart();
    /// Advances the movie clock and decodes the frames it has reached;
    /// uploads the latest one.
    void update(float dt);

    /// The texture holding the current frame, 0 before the first one.
    GLuint texture() const;
    int width() const;
    int height() const;
    /// Seconds of the movie shown, since the start of this loop.
    double time() const;
    /// Length of the movie in seconds.
    double duration() const;
    /// Frames decoded since construction, loops included.
    long framesDecoded() const;
    /// A non-looping movie that has shown its last frame.
    bool finished() const;
    /// RGBA of the current frame, row-major from the top, for tests.
    const std::vector<std::uint8_t>& pixels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::ui
