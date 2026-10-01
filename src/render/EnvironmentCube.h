#pragma once

#include "EnvironmentCubeMath.h"
#include "Gl.h"
#include "Shader.h"
#include "WorldRenderer.h"

#include <glm/glm.hpp>

#include <functional>
#include <span>

namespace openhorizon::render {

/// The dynamic environment cube the cars reflect (envSampler), rendered
/// around the focus car when TrackSettings.xml sets
/// Performance/DynamicCubemap (0x82424244).
///
/// Verified (render 0x82424128, face loop 0x82439E80, mips 0x82E8B308): a
/// 256 texel cube of square-root colour, envcube::facesPerFrame() faces a
/// frame, mips rebuilt through a lat-long map once all faces are drawn.
/// The IBL cubes are not made: the renderer's descriptor (0x824B4098,
/// stored at 0x82E90B58-0x82E90B68) leaves them off and no car shader samples them.
///
/// Formats: half floats stand in for the game's 7e3 (0x1A2201BF,
/// 0x182801B6) and 16-bit (0x1A20AA55) targets.
class EnvironmentCube {
public:
    /// Needs a current GL context.
    EnvironmentCube();
    ~EnvironmentCube();
    EnvironmentCube(const EnvironmentCube&) = delete;
    EnvironmentCube& operator=(const EnvironmentCube&) = delete;

    /// Draws into one face, already bound and cleared.
    using DrawFace = std::function<void(const FrameParams& face)>;
    /// Draws into several faces at once: face `layers[i]` has transform
    /// `transforms[i]` (see faceTransform()); `frame`'s camera is the centre.
    using DrawFaces = std::function<void(
        const FrameParams& frame, std::span<const glm::mat4> transforms, std::span<const int> layers)>;

    /// Renders this frame's faces around `centre` (with `drawFaces` first
    /// when given, then `drawFace` in each) and rebuilds the mips when due.
    /// `clearColour` as stored (TrackSettings::cubeClearColour()). Leaves the
    /// default framebuffer bound.
    void update(const glm::vec3& centre, float envMapFrequencyScale, const glm::vec4& clearColour,
        const FrameParams& frame, const DrawFaces* drawFaces, const DrawFace& drawFace);

    GLuint texture() const { return m_cube; }
    const envcube::FaceSchedule& schedule() const { return m_schedule; }
    /// Faces the last update() drew.
    int facesDrawn() const { return m_facesDrawn; }
    /// Times the mip chain has been built.
    int mipBuilds() const { return m_mipBuilds; }
    /// The camera of face `face` around `centre`.
    static FrameParams faceFrame(int face, const glm::vec3& centre, const FrameParams& frame);
    /// faceProjection() faceView(face, c) is this times a translation by -c.
    static glm::mat4 faceTransform(int face);

private:
    void buildMips();

    GLuint m_cube = 0;
    GLuint m_depth = 0;
    GLuint m_sphere = 0;
    GLuint m_framebuffer = 0;
    GLuint m_emptyVao = 0;
    Shader m_toSphere;
    Shader m_downsample;
    Shader m_toFace;
    envcube::FaceSchedule m_schedule;
    int m_facesDrawn = 0;
    int m_mipBuilds = 0;
};

} // namespace openhorizon::render
