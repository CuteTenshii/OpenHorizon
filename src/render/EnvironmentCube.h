#pragma once

#include "EnvironmentCubeMath.h"
#include "Gl.h"
#include "Shader.h"
#include "WorldRenderer.h"

#include <glm/glm.hpp>

#include <functional>
#include <span>

namespace openhorizon::render {

/// The dynamic environment cube the cars reflect (their envSampler), which
/// the game renders around the focus car on tracks whose TrackSettings.xml
/// sets Performance/DynamicCubemap (0x82424244; Colorado does).
///
/// Verified in the executable (render function 0x82424128, face loop
/// 0x82439E80, mip builder 0x82E8B308; see envcube for the constants):
///  - A 256 texel cube with a full mip chain holding, like the scene target,
///    the square root of colour: the faces are what the track effects write
///    (`sqrt oC0`), and the car shaders square what they fetch.
///  - Each frame renders envcube::facesPerFrame() faces, in the order of
///    envcube::kFaceOrder from where the last frame stopped, with a 90
///    degree camera centred half a metre above the focus car.
///  - Once all six faces have been drawn, and again after any face is
///    redrawn, the mip chain is rebuilt from level 0 through a lat-long
///    map (the game's BlitSphericalMap, AveragedDownSample and
///    BlitCubeFaceFromSphericalMap, ported in EnvironmentCube.cpp).
/// The IBL cubes the renderer can filter from this one
/// (BlitLightingCubeFaceFromCubeMap into its specular, blurred and ambient
/// cubes) are not made: the renderer's creation descriptor leaves them off
/// (bytes 62 to 64 of the one built at 0x824B4098, stored at 0x82E90B58-
/// 0x82E90B68), and no car shader samples them.
///
/// Formats: the game renders into a 7e3 float target (0x1A2201BF) and
/// resolves into a 7e3 texture (0x182801B6); half floats hold the same
/// values here, as they do for the scene target (FrameRenderer). The lat-
/// long map is a 16-bit target (0x1A20AA55), half floats here too.
class EnvironmentCube {
public:
    /// Needs a current GL context.
    EnvironmentCube();
    ~EnvironmentCube();
    EnvironmentCube(const EnvironmentCube&) = delete;
    EnvironmentCube& operator=(const EnvironmentCube&) = delete;

    /// Draws into one face: its layer is bound and cleared, the viewport
    /// and the scene's depth state set; `face` holds the face camera and the
    /// frame's lighting and clock.
    using DrawFace = std::function<void(const FrameParams& face)>;
    /// Draws into several faces at once: every layer of the cube is bound,
    /// the faces are cleared, and face `layers[i]` is the one whose
    /// transform, from clip space around the centre, is `transforms[i]`
    /// (see faceTransform()). `frame` holds the centre as camera position
    /// and the frame's lighting and clock.
    using DrawFaces = std::function<void(
        const FrameParams& frame, std::span<const glm::mat4> transforms, std::span<const int> layers)>;

    /// Renders this frame's faces around `centre`, each face cleared first
    /// to `clearColour` (as stored, see
    /// world::TrackSettings::cubeClearColour()) and to the far depth: with
    /// `drawFaces` all of them in one pass, then `drawFace` in each; without
    /// it, `drawFace` in each. Then rebuilds the mips when they are due.
    /// `frame` gives the lighting and clock. Leaves the default framebuffer
    /// bound.
    void update(const glm::vec3& centre, float envMapFrequencyScale, const glm::vec4& clearColour,
        const FrameParams& frame, const DrawFaces* drawFaces, const DrawFace& drawFace);

    GLuint texture() const { return m_cube; }
    const envcube::FaceSchedule& schedule() const { return m_schedule; }
    /// Faces the last update() drew.
    int facesDrawn() const { return m_facesDrawn; }
    /// Times the mip chain has been built.
    int mipBuilds() const { return m_mipBuilds; }
    /// The camera of face `face` around `centre`, with `frame`'s lighting
    /// and clock.
    static FrameParams faceFrame(int face, const glm::vec3& centre, const FrameParams& frame);
    /// Face `face`'s projection and turn, for positions already relative
    /// to the centre: faceProjection() faceView(face, c) is this times a
    /// translation by -c.
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
