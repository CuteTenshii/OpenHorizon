#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace openhorizon::render::envcube {

/// Edge of the dynamic environment cube in texels. Verified: the renderer's
/// creation descriptor takes it from the game options (0x824B40B0-
/// 0x824B40EC), whose default is 256 (0x82562CCC, stored at 0x825632E8);
/// only the `highrescubemap` (512) and `gensh` (128) debug options change
/// it. The cube is made with a full mip chain (0x82E861CC: levels 0 when the
/// descriptor's mip flag, 0x82E90B48, is set, which it is unless `gensh`).
inline constexpr int kSize = 256;
inline constexpr int kLevels = 9;

/// The lat-long map the mip chain is built through: 768 x 384 with 8
/// levels, down to 6 x 3 (0x82E8B4C4-0x82E8B4D8).
inline constexpr int kSphereWidth = 768;
inline constexpr int kSphereHeight = 384;
inline constexpr int kSphereLevels = 8;

/// The cube camera (0x82E862B8-0x82E86310): a 90 degree field of view, a
/// square aspect, near and far planes at 0.1 and 5000 metres.
inline constexpr float kNear = 0.1F;
inline constexpr float kFar = 5000.0F;

/// How far above the focus car's origin the cube is centred. Verified: the
/// render thread's copy of the frame (0x82E7C758) takes the translation row
/// of the focus car's matrix and adds 0.5 (0x82000D38) to its y.
inline constexpr float kCentreHeight = 0.5F;

/// The order in which faces are rendered, as Direct3D cube face indices
/// (+X, -X, +Y, -Y, +Z, -Z = 0..5), from the table at 0x82253E38 that the
/// face loop (0x82439FD4-0x82439FF4) indexes with (cursor + i) mod 6.
inline constexpr std::array<int, 6> kFaceOrder{0, 4, 2, 1, 3, 5};

/// Faces rendered per frame for a render scenario's EnvMapFrequencyScale
/// (0x82429B30): 6 times the scale truncated towards zero, kept within 1
/// and 6. The whole cube update is skipped while the scale is not above 0
/// (0x82424174), which facesPerFrame() reports as 0.
///
/// Verified: the value is the scenario setting at index 30 of the
/// DynamicRenderSettings table (its record, 0x8224F74C, answers 30 at
/// 0x82E3FF98, and 0x82424060 reads slot 30), and the game in play loads
/// Horizon_Race (0x824BF2E0; PhotoMode in photo mode), whose
/// EnvMapFrequencyScale is 1.0, so all six faces are drawn each frame.
/// Not applied: the view flags +457, +479 and +480 that force six faces,
/// and the three camera queries (its vtable slots 116, 120 and 124) that
/// force one; which cameras answer them was not traced, and the chase
/// camera is taken to answer none.
int facesPerFrame(float envMapFrequencyScale);

/// Which faces a frame renders and when the cube's mip chain is rebuilt,
/// as the renderer's face loop (0x82439E80) and its caller (0x82424128)
/// track them in the cube state word (+8252 of the renderer): a bit per
/// face drawn since the cube was last reset, and a bit for mips built.
class FaceSchedule {
public:
    /// The faces to render this frame, in order, and moves the cursor on by
    /// as many (0x8243A1AC-0x8243A1C0). Sets each face's bit and clears the
    /// mip bit (0x8243A198-0x8243A1A4).
    struct Frame {
        std::array<int, 6> faces{};
        int count = 0;
    };
    Frame next(float envMapFrequencyScale);
    /// Whether all six faces have been drawn since the last reset: the mips
    /// are built only then (0x82424374-0x82424390).
    bool complete() const { return (m_drawn & 0x3F) == 0x3F; }
    /// Whether the mip chain should be rebuilt now: every face drawn and no
    /// rebuild since the last face (0x82424368-0x824243D0).
    bool mipsDue() const { return complete() && !m_mipsBuilt; }
    void markMipsBuilt() { m_mipsBuilt = true; }
    /// Forgets every drawn face, as the renderer does when the presentation
    /// asks for the cube again (0x82424230).
    void reset();
    int cursor() const { return m_cursor; }

private:
    int m_cursor = 0;
    std::uint32_t m_drawn = 0;
    bool m_mipsBuilt = false;
};

/// The centre of the cube for a car whose body matrix (car space to world)
/// is `body`.
glm::vec3 centre(const glm::mat4& body);

/// World to eye space of cube face `face` (0..5, +X, -X, +Y, -Y, +Z, -Z)
/// around `centre`, for this renderer's world (the game's with z negated,
/// see CarRenderer) and GL's cube map addressing: the car shaders sample
/// the cube with a direction in the game's coordinates, and a GL cube map
/// returns for direction d the texel of face `face` at s = (sc / |ma| + 1)
/// / 2, t = (tc / |ma| + 1) / 2 (OpenGL 4.5, table 8.19). The eye space
/// has x = sc, y = tc and z = ma, so a projection that divides by z puts
/// every point where its direction is addressed; drawn into the face's
/// layer, framebuffer row 0 is texel row t = 0. Direct3D addresses its
/// cube faces the same way, so the game's faces hold the same image.
glm::mat4 faceView(int face, const glm::vec3& centre);

/// The cube camera's projection for faceView()'s eye space: x and y divided
/// by the depth along the face's axis (a 90 degree field of view), depth
/// reversed as the scene's is (1 at kNear, 0 at kFar) with a zero-to-one
/// clip range.
glm::mat4 faceProjection();

/// The unit direction, in the game's coordinates, that GL addresses at
/// texel centre (s, t) of face `face`, s and t in 0..1.
glm::vec3 faceDirection(int face, float s, float t);

/// The direction the lat-long map holds at (u, v), both 0..1: BlitSphericalMap
/// (0x82CFD308, pixel shader blob 0xC89C of the post effect at 0x821B9918)
/// samples the cube along (sin t cos p, cos t, sin t sin p) with
/// p = 2 pi u - pi and t = pi v, so v = 0 is straight up and u = 0 and 1
/// meet towards -x.
glm::vec3 sphereDirection(float u, float v);

/// Where BlitCubeFaceFromSphericalMap (0x82CFD0D8, blob 0xCD98) reads the
/// lat-long map for the unit direction `d`: (atan2(z, x) / 2 pi + 0.5,
/// acos(y) / pi), with atan2 and acos as the shader's polynomials compute
/// them (gameAtan2(), gameAcos()).
glm::vec2 sphereCoordinates(const glm::vec3& d);

/// atan2 and acos as the HLSL compiler expands them into the microcode of
/// BlitCubeFaceFromSphericalMap: odd polynomials in the ratio of the
/// smaller to the larger of |x| and |y| (coefficients 0.0208351,
/// -0.085133, 0.180141, -0.3302995, 0.999866) folded into the quadrants,
/// and (1.5707288 - 0.2121144 |x| + 0.074261 x^2 - 0.0187293 |x|^3)
/// sqrt(1 - |x|) mirrored for negative x. The coefficients are verified in
/// the microcode; the arrangement is the compiler's usual expansion
/// (inferred). EnvironmentCube's shader computes the same.
float gameAtan2(float y, float x);
float gameAcos(float x);

} // namespace openhorizon::render::envcube
