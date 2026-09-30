#pragma once

#include "world/XboxTexture.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace fh::spectators {

/// Which picture of a person a sprite cell holds.
///
/// `media/spectators/sprites.xds` (DXT1, 2048x768, 1-bit alpha) is an atlas
/// of 64x128 cells, row-major, 9 per model in the order of spectators.xml's
/// <models> list (verified: the clothes of each group of cells match the
/// model's texture in that order). The game draws them for the people past
/// the skinned meshes' reach (see CrowdDraw.h). Within a model's 9 cells
/// (read off the atlas):
///
///     0 standing, back     3 seated, back
///     1 standing, front    4 seated, front
///     2 standing, side     5, 6, 7 seated, turned 22.5, 45, 67.5 degrees
///                          8 seated, side
///
/// Side and turned cells show the person facing the image's right; the other
/// side is the same cell mirrored (bSpritesBothSides is false).
enum class SpriteView : std::uint8_t {
    StandingBack,
    StandingFront,
    StandingSide,
    SeatedBack,
    SeatedFront,
    SeatedTurned22,
    SeatedTurned45,
    SeatedTurned67,
    SeatedSide,
};

/// Cell size and grid of the sprite atlas.
struct SpriteAtlasLayout {
    static constexpr int kCellWidth = 64;
    static constexpr int kCellHeight = 128;
    static constexpr int kCellsPerModel = 9;

    int width = 0;
    int height = 0;

    int columns() const { return width / kCellWidth; }
    int rows() const { return height / kCellHeight; }
    int cellCount() const { return columns() * rows(); }
    /// The cell of `view` of the model at `modelIndex` in <models> order.
    static int cell(int modelIndex, SpriteView view) { return modelIndex * kCellsPerModel + static_cast<int>(view); }
    /// Texture coordinates of a cell's top-left and bottom-right corners (v
    /// grows downwards, as the texture's rows are stored).
    void cellRect(int cell, glm::vec2& uvMin, glm::vec2& uvMax) const;
};

/// How big a cell is in the world, measured from the atlas at load.
struct SpriteMetrics {
    /// Metres per texel, from the standing front cells' height against the
    /// standing meshes' height.
    float metresPerTexel = 1.0F / 64.0F;
    /// Texel rows from the top of a cell to the ground the person stands on.
    float groundRow = SpriteAtlasLayout::kCellHeight;

    float cellWidthMetres() const { return metresPerTexel * SpriteAtlasLayout::kCellWidth; }
    float cellHeightMetres() const { return metresPerTexel * SpriteAtlasLayout::kCellHeight; }
};

/// What to draw for one person seen from a camera.
struct SpriteFrame {
    int cell = 0;
    SpriteView view = SpriteView::StandingFront;
    glm::vec2 uvMin {0.0F};
    glm::vec2 uvMax {0.0F};
    /// Draw the cell flipped left to right.
    bool mirrored = false;
};

/// Chooses the cell for a person whose cells start at `baseCell` (the
/// model's first cell, plus three for six-view classes; see
/// Spectator::spriteCell), at `position` facing `facing` (horizontal, world
/// coordinates), seen from `camera`, by the game's billboard vertex shader
/// (draw::spriteColumn()): the view is the cell `baseCell` + column, and a
/// mirrored cell is drawn flipped left to right.
SpriteFrame chooseSprite(const SpriteAtlasLayout& layout, int baseCell, bool sixViews, const glm::vec3& facing,
    const glm::vec3& position, const glm::vec3& camera);

/// Measures the atlas: `atlas` is its decoded top level, `modelHeights` the
/// standing height of each model in <models> order (metres, 0 when unknown).
/// Returns the defaults when no standing front cell has opaque texels.
SpriteMetrics measureSprites(const world::RgbaImage& atlas, const std::vector<float>& modelHeights);

} // namespace fh::spectators
