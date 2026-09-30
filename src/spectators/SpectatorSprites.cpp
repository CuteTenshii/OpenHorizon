#include "SpectatorSprites.h"

#include "CrowdDraw.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace fh::spectators {

namespace {

constexpr std::uint8_t kOpaque = 128;

} // namespace

void SpriteAtlasLayout::cellRect(int cell, glm::vec2& uvMin, glm::vec2& uvMax) const
{
    const int cols = std::max(columns(), 1);
    const float x = static_cast<float>((cell % cols) * kCellWidth);
    const float y = static_cast<float>((cell / cols) * kCellHeight);
    const float w = static_cast<float>(std::max(width, 1));
    const float h = static_cast<float>(std::max(height, 1));
    uvMin = {x / w, y / h};
    uvMax = {(x + kCellWidth) / w, (y + kCellHeight) / h};
}

SpriteFrame chooseSprite(const SpriteAtlasLayout& layout, int baseCell, bool sixViews, const glm::vec3& facing,
    const glm::vec3& position, const glm::vec3& camera)
{
    glm::vec2 toCamera(camera.x - position.x, camera.z - position.z);
    glm::vec2 face(facing.x, facing.z);
    if (glm::length(face) < 1e-4F) {
        face = {0.0F, 1.0F};
    }
    if (glm::length(toCamera) < 1e-4F) {
        toCamera = face;
    }
    const draw::SpriteColumn column = draw::spriteColumn(glm::normalize(face), glm::normalize(toCamera), sixViews);
    SpriteFrame frame;
    frame.cell = baseCell + column.column;
    frame.mirrored = column.mirrored;
    const int cellInModel = frame.cell % SpriteAtlasLayout::kCellsPerModel;
    frame.view = static_cast<SpriteView>(std::clamp(cellInModel, 0, static_cast<int>(SpriteView::SeatedSide)));
    layout.cellRect(frame.cell, frame.uvMin, frame.uvMax);
    return frame;
}

SpriteMetrics measureSprites(const world::RgbaImage& atlas, const std::vector<float>& modelHeights)
{
    SpriteMetrics metrics;
    SpriteAtlasLayout layout {atlas.width, atlas.height};
    const int cols = layout.columns();
    if (cols <= 0) {
        return metrics;
    }
    double metresPerTexel = 0.0;
    double groundRow = 0.0;
    int measured = 0;
    for (std::size_t m = 0; m < modelHeights.size(); ++m) {
        if (!(modelHeights[m] > 0.0F)) {
            continue;
        }
        const int cell = SpriteAtlasLayout::cell(static_cast<int>(m), SpriteView::StandingFront);
        if (cell >= layout.cellCount()) {
            continue;
        }
        const int x0 = (cell % cols) * SpriteAtlasLayout::kCellWidth;
        const int y0 = (cell / cols) * SpriteAtlasLayout::kCellHeight;
        int top = -1;
        int bottom = -1;
        for (int y = 0; y < SpriteAtlasLayout::kCellHeight; ++y) {
            for (int x = 0; x < SpriteAtlasLayout::kCellWidth; ++x) {
                const std::size_t texel = (static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(atlas.width)
                                              + static_cast<std::size_t>(x0 + x))
                    * 4;
                if (atlas.rgba[texel + 3] >= kOpaque) {
                    top = top < 0 ? y : top;
                    bottom = y;
                    break;
                }
            }
        }
        if (top < 0) {
            continue;
        }
        metresPerTexel += modelHeights[m] / static_cast<double>(bottom - top + 1);
        groundRow += bottom + 1;
        ++measured;
    }
    if (measured > 0) {
        metrics.metresPerTexel = static_cast<float>(metresPerTexel / measured);
        metrics.groundRow = static_cast<float>(groundRow / measured);
    }
    return metrics;
}

} // namespace fh::spectators
