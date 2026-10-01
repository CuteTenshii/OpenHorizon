#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include "world/WorldIndex.h"
#include "world/WorldTiles.h"

#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace openhorizon::render {

struct FrameParams;

/// `count` indices from `first` of a mesh's index list.
struct IndexRange {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

/// The index ranges of the chunks of `batches` that `drawn` marks (indexed
/// like the world index's chunks), adjacent ones merged: batch b's are
/// `ranges[start[b]]` up to `ranges[start[b + 1]]`.
template <typename Batch> void selectRanges(const std::vector<Batch>& batches, const std::vector<std::uint8_t>& drawn,
    std::vector<IndexRange>& ranges, std::vector<std::uint32_t>& start)
{
    ranges.clear();
    start.assign(1, 0);
    for (const Batch& batch : batches) {
        for (const world::ChunkRun& run : batch.runs) {
            if (run.chunk >= drawn.size() || drawn[run.chunk] == 0) {
                continue;
            }
            if (ranges.size() > start.back() && ranges.back().first + ranges.back().count == run.firstIndex) {
                ranges.back().count += run.indexCount;
            } else {
                ranges.push_back({run.firstIndex, run.indexCount});
            }
        }
        start.push_back(static_cast<std::uint32_t>(ranges.size()));
    }
}

/// Every index of every batch, one range each: all chunks drawn.
template <typename Batch>
void wholeRanges(const std::vector<Batch>& batches, std::vector<IndexRange>& ranges, std::vector<std::uint32_t>& start)
{
    ranges.clear();
    start.assign(1, 0);
    for (const Batch& batch : batches) {
        if (batch.indexCount > 0) {
            ranges.push_back({batch.firstIndex, batch.indexCount});
        }
        start.push_back(static_cast<std::uint32_t>(ranges.size()));
    }
}

/// Draws the procedural copies of models (world::ScatterSet) with the
/// renderer's own shading, not the track effects: the game's draw (from
/// 0x82448060) sets per-instance FadeValues (0x82EE1D00),
/// SurfaceNormalAndShadowPower (0x835A57C8) and an ambColor term that are
/// not recovered, and without FadeValues.z the foliage effects draw nothing.
class ScatterPass {
public:
    struct Mesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ibo = 0;
        std::vector<world::TileMesh::Batch> batches;
        /// What draw() draws of each batch (see selectRanges()).
        std::vector<IndexRange> ranges;
        std::vector<std::uint32_t> rangeStart;
        /// Less the chunks out of view; drawn instead when `clipped` is set.
        std::vector<IndexRange> viewRanges;
        std::vector<std::uint32_t> viewRangeStart;
        bool clipped = false;
        ~Mesh();
    };

    ScatterPass();

    static std::shared_ptr<Mesh> upload(const world::TileMesh& mesh);

    /// Binds the program and sets the frame's uniforms.
    void begin(const FrameParams& frame);
    /// Places what the next draws show; identity for the static world.
    void setModel(const glm::mat4& model);
    /// Draws the selected ranges of the opaque batches of `mesh`; `texture`
    /// returns a loaded texture or nullptr. Returns the triangles drawn.
    std::size_t draw(const Mesh& mesh, const std::function<const Texture*(std::uint32_t)>& texture,
        const world::WorldIndex& index, int cameraZone);

private:
    Shader m_shader;
    Texture m_white;
};

} // namespace openhorizon::render
