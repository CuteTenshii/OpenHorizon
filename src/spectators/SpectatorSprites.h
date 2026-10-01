#pragma once

#include "world/XboxTexture.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace openhorizon::spectators {

/// A cell of `media/spectators/sprites.xds` (DXT1, 2048x768, 1-bit alpha): 64x128 cells, row-major, 9 per model
/// in spectators.xml's <models> order (verified). A model's cells, read off the atlas:
///
///     0 standing, back     3 seated, back
///     1 standing, front    4 seated, front
///     2 standing, side     5, 6, 7 seated, turned 22.5, 45, 67.5 degrees
///                          8 seated, side
///
/// Side and turned cells face the image's right; the other side is mirrored.
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
    /// Top-left and bottom-right; v grows downwards, as the rows are stored.
    void cellRect(int cell, glm::vec2& uvMin, glm::vec2& uvMax) const;
};

/// How big a cell is in the world, measured from the atlas at load.
struct SpriteMetrics {
    /// From the standing front cells' height against the standing meshes'.
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

/// `baseCell` as Spectator::spriteCell; the column comes from draw::spriteColumn(), as the billboard shader picks
/// it.
SpriteFrame chooseSprite(const SpriteAtlasLayout& layout, int baseCell, bool sixViews, const glm::vec3& facing,
    const glm::vec3& position, const glm::vec3& camera);

/// `atlas` is the decoded top level, `modelHeights` metres in <models> order (0: unknown). Returns the defaults
/// when no standing front cell has opaque texels.
SpriteMetrics measureSprites(const world::RgbaImage& atlas, const std::vector<float>& modelHeights);

} // namespace openhorizon::spectators
