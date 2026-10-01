#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>

namespace openhorizon::render::envcube {

/// Cube edge in texels, with a full mip chain. Verified: default 256
/// (0x82562CCC, stored at 0x825632E8, read at 0x824B40B0-0x824B40EC); only
/// debug options change it. Mips at 0x82E861CC (flag 0x82E90B48).
inline constexpr int kSize = 256;
inline constexpr int kLevels = 9;

/// The lat-long map the mips are built through (0x82E8B4C4-0x82E8B4D8).
inline constexpr int kSphereWidth = 768;
inline constexpr int kSphereHeight = 384;
inline constexpr int kSphereLevels = 8;

/// The cube camera's planes in metres (0x82E862B8-0x82E86310).
inline constexpr float kNear = 0.1F;
inline constexpr float kFar = 5000.0F;

/// Height of the cube's centre above the focus car's origin. Verified:
/// 0x82E7C758, constant 0x82000D38.
inline constexpr float kCentreHeight = 0.5F;

/// Face render order as Direct3D face indices (table at 0x82253E38, indexed
/// by (cursor + i) mod 6 at 0x82439FD4-0x82439FF4).
inline constexpr std::array<int, 6> kFaceOrder{0, 4, 2, 1, 3, 5};

/// Faces per frame for EnvMapFrequencyScale (0x82429B30): 6 x scale
/// truncated, within 1 and 6; 0 when the scale is not above 0 and the update
/// is skipped (0x82424174). Verified: DynamicRenderSettings slot 30
/// (0x8224F74C, 0x82E3FF98, 0x82424060); Horizon_Race (0x824BF2E0) sets 1.0.
/// Not applied: the view flags and camera queries that force six or one.
int facesPerFrame(float envMapFrequencyScale);

/// The face loop's (0x82439E80) and its caller's (0x82424128) cube state
/// word (renderer +8252).
class FaceSchedule {
public:
    /// This frame's faces in order; advances the cursor (0x8243A1AC-0x8243A1C0)
    /// and clears the mip bit (0x8243A198-0x8243A1A4).
    struct Frame {
        std::array<int, 6> faces{};
        int count = 0;
    };
    Frame next(float envMapFrequencyScale);
    /// Mips are built only once this holds (0x82424374-0x82424390).
    bool complete() const { return (m_drawn & 0x3F) == 0x3F; }
    /// 0x82424368-0x824243D0.
    bool mipsDue() const { return complete() && !m_mipsBuilt; }
    void markMipsBuilt() { m_mipsBuilt = true; }
    /// Forgets the drawn faces (0x82424230).
    void reset();
    int cursor() const { return m_cursor; }

private:
    int m_cursor = 0;
    std::uint32_t m_drawn = 0;
    bool m_mipsBuilt = false;
};

/// `body` maps car space to world.
glm::vec3 centre(const glm::mat4& body);

/// World to eye space of cube face `face` for GL's cube addressing (eye
/// x = sc, y = tc, z = ma; OpenGL 4.5, table 8.19), with the game's
/// directions (z negated, see CarRenderer). Direct3D addresses faces alike.
glm::mat4 faceView(int face, const glm::vec3& centre);

/// 90 degrees, reversed zero-to-one depth (1 at kNear, 0 at kFar).
glm::mat4 faceProjection();

/// The unit direction, in the game's coordinates, that GL addresses at
/// texel centre (s, t) of face `face`, s and t in 0..1.
glm::vec3 faceDirection(int face, float s, float t);

/// The lat-long map's direction at (u, v) in 0..1, as BlitSphericalMap
/// (0x82CFD308, blob 0xC89C of 0x821B9918) samples it; v = 0 is up.
glm::vec3 sphereDirection(float u, float v);

/// Where BlitCubeFaceFromSphericalMap (0x82CFD0D8, blob 0xCD98) reads the
/// lat-long map for the unit direction `d`.
glm::vec2 sphereCoordinates(const glm::vec3& d);

/// atan2 and acos as BlitCubeFaceFromSphericalMap's polynomials compute
/// them. Coefficients verified in the microcode; arrangement inferred.
float gameAtan2(float y, float x);
float gameAcos(float x);

} // namespace openhorizon::render::envcube
