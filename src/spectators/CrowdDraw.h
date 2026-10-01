#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// How the game draws its procedural crowds (`.pgeo` kind 3, CProceduralCharacters, vtable 0x820028A4): every
/// person as a sprite (PROC_CHARACTER_BILLBOARD), near ones also as a skinned model (PROC_CHARACTER_SKIN).
///
/// - Models: the cull (0x82ECAB38) queues people in load and record order until kModelQueue is full; later ones
///   are dropped, not swapped for nearer ones (0x82ECB020). Walkers join after their set's people. The prepare
///   step (0x82EE2E80) sorts by distance and assigns levels of detail (modelLods()).
/// - Sprites: the billboard draw (0x8248A270) draws every instance; the shader collapses those with a fade of 0.
///   FadeValues depends on the pass (0x8248A360): kSpriteFade for passes 9 and 11, (-100, -90, 60, 40) otherwise.
///   Inferred: the main view uses kSpriteFade, the one that takes over from the models' fade.
///
/// Thresholds are globals at 0x833A5630-0x833A5684, with the photo camera's distance scale at 1.
namespace openhorizon::spectators::draw {

/// Metres: 60 s + 20 with distance scale s = 1 (0x833A5630, 0x833A563C, 0x833A5640).
inline constexpr float kModelDistance = 80.0F;
/// Metres; applies to the walker and its set (0x833A5684).
inline constexpr float kWalkerDistance = 200.0F;
/// Entries of the model queue (0x82ECB020).
inline constexpr std::size_t kModelQueue = 500;
/// The culled box around each person's feet, metres along world x, y, z (0x833A5674, 0x833A5678).
inline constexpr glm::vec2 kPersonBoxHalfWidth{0.5F, 0.5F};
inline constexpr float kPersonBoxHeight = 2.0F;

/// FadeValues from the prepare step and the billboard draw; the models' are (60 s + 20, 60 s, 0, 0).
inline constexpr glm::vec4 kModelFade{80.0F, 60.0F, 0.0F, 0.0F};
inline constexpr glm::vec4 kWalkerFade{200.0F, 180.0F, 0.0F, 0.0F};
inline constexpr glm::vec4 kSpriteFade{40.0F, 60.0F, 700.0F, 650.0F};

/// saturate((distance - fade.x) / (fade.y - fade.x)), PROC_CHARACTER_SKIN_VS's TEXCOORD7.w.
float modelFade(float distance, const glm::vec4& fade);

/// PROC_CHARACTER_BILLBOARD_VS: min(saturate((d - x) / (y - x)), saturate((d - z) / (w - z))); billboard-only
/// people use the second alone (IgnoreBillboardOnlyFlag.x is 0 outside the photo camera).
float spriteFade(float distance, bool billboardOnly, const glm::vec4& fade = kSpriteFade);

/// The prepare step's LOD table (0x833A57F8, mode 0): people up to `distance` metres, at most `cap` (negative:
/// no cap).
struct LodBand {
    float distance = 0.0F;
    int cap = -1;
};
inline constexpr std::array<LodBand, 5> kLodBands{{{10.0F, 5}, {30.0F, 10}, {50.0F, 20}, {70.0F, 30}, {90.0F, -1}}};

/// `distances` nearest first; a person skips bands that are full, and the level is clamped to `lodCount` - 1
/// (0x82EE2E80).
std::vector<int> modelLods(std::span<const float> distances, int lodCount);

/// `column` is added to the person's base cell; `mirrored` flips it left to right.
struct SpriteColumn {
    int column = 0;
    bool mirrored = false;
};

/// The billboard shader's view choice; both vectors are unit horizontal x, z. With c their cosine: behind
/// (c < -0.707) 0; three views: front (c > 0.707) 1, side 2; six views ("Sit"): 1 to 5 in 22.5 degree steps,
/// held at 90. Side and turned columns mirror when toCamera.y facing.x - toCamera.x facing.y > 0.
SpriteColumn spriteColumn(const glm::vec2& facing, const glm::vec2& toCamera, bool sixViews);

/// sqrt(1 - |c|) (1.5707288 - 0.2121144 |c| + 0.074261 c^2 - 0.0187293 |c|^3), as the shader computes it.
float shaderArcCos(float c);

/// NumAlongNumDownWidthHeight and UVScaleSizeScale (0x8248A270); sizes in metres, the size range by the
/// person's size factor.
inline constexpr int kSpriteCellsAlong = 32;
inline constexpr float kSpriteHalfWidth = 0.5F;
inline constexpr float kSpriteHeight = 2.0F;
inline constexpr glm::vec2 kSpriteUvScale{0.9F, 0.95F};
inline constexpr float kSpriteSizeMin = 0.95F;
inline constexpr float kSpriteSizeMax = 1.05F;

/// The crowd code's generator (0x82DBE490): state * 0x41C64E6D + 0x3039, a draw is bits 16 to 30.
class CrowdRandom {
public:
    explicit CrowdRandom(std::uint32_t seed = 0)
        : m_state(seed)
    {
    }
    /// 0 to 32767 (0x82DBE490).
    std::uint32_t next();
    /// `lo` + (`hi` - `lo`) * next() / 32767, both ends included (0x82DBE4F0).
    float range(float lo, float hi);
    std::uint32_t state() const { return m_state; }

private:
    std::uint32_t m_state;
};

/// rand() (0x82B3D360): state * 214013 + 2531011, bits 16 to 30. The game shares it per thread, so its
/// sequence can't be reproduced; this starts at the runtime's 1.
class CrtRandom {
public:
    std::uint32_t next();

private:
    std::uint32_t m_state = 1;
};

/// Slots of a crowd class's idle and cheer tables.
inline constexpr std::size_t kClipSlots = 50;

/// CCrowdPresentation's table from the weights (0x82EA57D0): each clip fills weight / total * 50 slots, the
/// fraction carrying over; leftover slots take the first clip. Empty input gives no table.
std::vector<std::uint32_t> clipTable(std::span<const std::uint32_t> clips, std::span<const float> weights);

} // namespace openhorizon::spectators::draw
