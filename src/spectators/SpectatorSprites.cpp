#include "SpectatorSprites.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fh::spectators {

namespace {

constexpr float kFrontLimit = 45.0F;
constexpr float kBackLimit = 135.0F;
constexpr float kSeatedStep = 22.5F;
constexpr int kSeatedTurnedCells = 4;
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

SpriteFrame chooseSprite(const SpriteAtlasLayout& layout, int modelIndex, bool seated, const glm::vec3& facing,
    const glm::vec3& position, const glm::vec3& camera)
{
    glm::vec2 toCamera(camera.x - position.x, camera.z - position.z);
    glm::vec2 face(facing.x, facing.z);
    if (glm::length(toCamera) < 1e-4F) {
        toCamera = face;
    }
    if (glm::length(face) < 1e-4F) {
        face = {0.0F, 1.0F};
    }
    toCamera = glm::normalize(toCamera);
    face = glm::normalize(face);
    const float angle
        = std::acos(std::clamp(glm::dot(face, toCamera), -1.0F, 1.0F)) * 180.0F / std::numbers::pi_v<float>;

    // The camera looks along -toCamera; with Y up in the left-handed world
    // its right on screen is (view.z, -view.x).
    const glm::vec2 view = -toCamera;
    const glm::vec2 screenRight(view.y, -view.x);
    const bool facesLeft = glm::dot(face, screenRight) < 0.0F;

    SpriteFrame frame;
    if (seated) {
        if (angle > kBackLimit) {
            frame.view = SpriteView::SeatedBack;
        } else {
            const int step = std::min(static_cast<int>(std::lround(angle / kSeatedStep)), kSeatedTurnedCells);
            frame.view = static_cast<SpriteView>(static_cast<int>(SpriteView::SeatedFront) + step);
            frame.mirrored = step > 0 && facesLeft;
        }
    } else if (angle < kFrontLimit) {
        frame.view = SpriteView::StandingFront;
    } else if (angle > kBackLimit) {
        frame.view = SpriteView::StandingBack;
    } else {
        frame.view = SpriteView::StandingSide;
        frame.mirrored = facesLeft;
    }
    frame.cell = SpriteAtlasLayout::cell(modelIndex, frame.view);
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
