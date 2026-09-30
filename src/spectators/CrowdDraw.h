#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// The rules by which the game draws the crowds of its procedural crowd sets
/// (kind 3 of the track's `.pgeo` sets, class CProceduralCharacters of
/// proceduralGeometry, vtable 0x820028A4), as far as they are traced.
///
/// Every person of a visible set is drawn as a sprite (PROC_CHARACTER_
/// BILLBOARD_VS/PS) and those near the camera also as a skinned model
/// (PROC_CHARACTER_SKIN_VS/PS); each fades by its distance through its
/// FadeValues constant, so the sprites come in as the models go:
///
/// - Models: the crowd cull (0x82ECAB38) queues, set after set in load order
///   and person after person in record order, every person that is not
///   billboard-only, lies within kModelDistance of the camera and whose box
///   (kPersonBoxMin to kPersonBoxMax around the feet) is in view, until
///   kModelQueue people are queued; later ones are dropped, not swapped for
///   nearer ones (0x82ECB020). Walkers of sets within kWalkerDistance join
///   the same queue after their set's people. The prepare step (0x82EE2E80)
///   sorts the queue by distance and gives each person a level of detail
///   from kLodBands (modelLods()). FadeValues is kModelFade (walkers
///   kWalkerFade): the shader's alpha is the texture's times
///   modelFade(distance).
/// - Sprites: the billboard draw (0x8248A270) draws every instance of the
///   set, and the vertex shader collapses those whose spriteFade() is 0.
///   FadeValues is one of three by the draw's pass (0x8248A360): kSpriteFade
///   for passes 9 and 11, (-100, -90, 60, 40) for the others. Which pass is
///   the main view's was not found; kSpriteFade is the pair that takes over
///   from the models' fade (sprites in from 40 to 60 m where the models go
///   from 60 to 80 m), so it is taken as the main view's.
///
/// The thresholds are the executable's globals at 0x833A5630-0x833A5684
/// (distance scale 1, 60 m model reach plus 20 m of fade, 200 m walkers,
/// 500 queue entries), with the scale the photo camera changes left at 1.
namespace fh::spectators::draw {

/// Metres from the camera within which people are drawn as models: 60 s +
/// 20 with the distance scale s at 1 (0x833A5630, 0x833A563C, 0x833A5640).
inline constexpr float kModelDistance = 80.0F;
/// Walkers: their set and the walker itself within this many metres
/// (0x833A5684).
inline constexpr float kWalkerDistance = 200.0F;
/// Entries of the model queue (0x82ECB020).
inline constexpr std::size_t kModelQueue = 500;
/// The box the cull tests against the view around each person's feet, in
/// metres along world x, y and z (0x833A5674, 0x833A5678).
inline constexpr glm::vec2 kPersonBoxHalfWidth{0.5F, 0.5F};
inline constexpr float kPersonBoxHeight = 2.0F;

/// FadeValues of the models (60 s + 20, 60 s, 0, 0), the walkers (200, 180,
/// 0, 0) and the sprites in the main view (40, 60, 700, 650), from the
/// prepare step and the billboard draw.
inline constexpr glm::vec4 kModelFade{80.0F, 60.0F, 0.0F, 0.0F};
inline constexpr glm::vec4 kWalkerFade{200.0F, 180.0F, 0.0F, 0.0F};
inline constexpr glm::vec4 kSpriteFade{40.0F, 60.0F, 700.0F, 650.0F};

/// A model's opacity factor at `distance` metres: saturate((distance -
/// fade.x) / (fade.y - fade.x)), as PROC_CHARACTER_SKIN_VS passes it in
/// TEXCOORD7.w.
float modelFade(float distance, const glm::vec4& fade);

/// A sprite's fade at `distance` metres (PROC_CHARACTER_BILLBOARD_VS):
/// saturate((distance - x) / (y - x)) coming in and saturate((distance - z)
/// / (w - z)) going out, the smaller of the two; billboard-only people have
/// no model to take over from them and use the second alone (the shader
/// does so when IgnoreBillboardOnlyFlag.x is 0, which the draw sets
/// outside the photo camera). A sprite whose fade is 0 is collapsed to a
/// point.
float spriteFade(float distance, bool billboardOnly, const glm::vec4& fade = kSpriteFade);

/// A level of detail band of the prepare step (the table at 0x833A57F8,
/// mode 0): people up to `distance` metres get the band's level unless
/// `cap` people already have it (negative: no cap).
struct LodBand {
    float distance = 0.0F;
    int cap = -1;
};
inline constexpr std::array<LodBand, 5> kLodBands{{{10.0F, 5}, {30.0F, 10}, {50.0F, 20}, {70.0F, 30}, {90.0F, -1}}};

/// Levels of detail for queued people at `distances` metres, nearest first:
/// each takes the first band whose distance reaches it (the last band
/// otherwise), moves on while that band has reached its cap, and is
/// clamped to `lodCount` - 1 (0x82EE2E80).
std::vector<int> modelLods(std::span<const float> distances, int lodCount);

/// The sprite cell a person shows: `column` is added to the person's base
/// cell, and `mirrored` flips it left to right.
struct SpriteColumn {
    int column = 0;
    bool mirrored = false;
};

/// The billboard vertex shader's view choice for a person facing `facing`
/// (horizontal x and z of the facing direction, unit length) seen along
/// `toCamera` (horizontal x and z from the person to the camera, unit
/// length). With c the cosine between them: behind (c < -0.707) column 0;
/// with three views (every class but the first) front (c > 0.707) 1 and
/// side 2; with six views (the first class, "Sit") 1 to 5 in steps of 22.5
/// degrees from the front, the angle held at 90 degrees beyond. The angle is
/// the shader's arc cosine, the polynomial sqrt(1 - |c|) (1.5707288 -
/// 0.2121144 |c| + 0.074261 c^2 - 0.0187293 |c|^3). Side and turned columns
/// are mirrored when the camera is on the facing's positive cross side
/// (toCamera.y facing.x - toCamera.x facing.y > 0).
SpriteColumn spriteColumn(const glm::vec2& facing, const glm::vec2& toCamera, bool sixViews);

/// The shader's arc cosine (see spriteColumn()).
float shaderArcCos(float c);

/// The sprite quad's constants (0x8248A270): NumAlongNumDownWidthHeight
/// (32 cells a row, the atlas's rows, half-width 0.5 m, height 2 m) and
/// UVScaleSizeScale (0.9 of a cell's width and 0.95 of its height used, a
/// size from 0.95 to 1.05 by the person's size factor).
inline constexpr int kSpriteCellsAlong = 32;
inline constexpr float kSpriteHalfWidth = 0.5F;
inline constexpr float kSpriteHeight = 2.0F;
inline constexpr glm::vec2 kSpriteUvScale{0.9F, 0.95F};
inline constexpr float kSpriteSizeMin = 0.95F;
inline constexpr float kSpriteSizeMax = 1.05F;

/// The random generator the crowd code draws from (0x82DBE490 and its
/// wrappers): state = state * 0x41C64E6D + 0x3039, a draw the state's bits
/// 16 to 30.
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

/// The C runtime's rand() (0x82B3D360): state = state * 214013 + 2531011,
/// a draw the state's bits 16 to 30. Its state is per thread and shared
/// with the rest of the game, so the game's sequence cannot be reproduced;
/// this one starts where the runtime starts, at 1.
class CrtRandom {
public:
    std::uint32_t next();

private:
    std::uint32_t m_state = 1;
};

/// Slots of a crowd class's idle and cheer tables.
inline constexpr std::size_t kClipSlots = 50;

/// A class's 50-slot clip table, built as CCrowdPresentation builds it from
/// the class's weights (0x82EA57D0): walking the clips in order, a running
/// count gains weight / total * 50 and each whole slot it covers takes the
/// clip, the count carrying over; slots left at the end take the first
/// clip. `clips` and `weights` run in step; empty gives no table.
std::vector<std::uint32_t> clipTable(std::span<const std::uint32_t> clips, std::span<const float> weights);

} // namespace fh::spectators::draw
