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

namespace fh::render {

struct FrameParams;

/// Draws the procedural copies of models (world::ScatterSet: the trees,
/// bushes, rocks and fences of the procedural sets) with the renderer's own
/// shading instead of the track effects.
///
/// The game draws every procedural copy with per-instance constants that
/// WorldRenderer cannot supply yet: its procedural-model draw (from
/// 0x82448060) sets FadeValues from a fade byte the visibility code updates
/// every frame and a minimum distance kept per group at run time,
/// SurfaceNormalAndShadowPower from the instance's ground normal and the
/// global at 0x835A57C8, and adds a per-instance term to ambColor. The
/// foliage effects multiply their opacity by FadeValues.z, so without those
/// values the copies would not show at all. Until they are recovered, this
/// pass keeps drawing them as the renderer did before it used the track
/// effects: one diffuse texture, alpha-tested at one half, lit by the sun and
/// the hemisphere ambient, fogged like the world.
class ScatterPass {
public:
    struct Mesh {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ibo = 0;
        std::vector<world::TileMesh::Batch> batches;
        ~Mesh();
    };

    ScatterPass();

    static std::shared_ptr<Mesh> upload(const world::TileMesh& mesh);

    /// Binds the program and sets the frame's uniforms.
    void begin(const FrameParams& frame);
    /// Places what the next draws show; identity for the static world.
    void setModel(const glm::mat4& model);
    /// Draws the opaque batches of `mesh`; `texture` returns a loaded texture
    /// or nullptr. Returns the triangles drawn.
    std::size_t draw(const Mesh& mesh, const std::function<const Texture*(std::uint32_t)>& texture,
        const world::WorldIndex& index, int cameraZone);

private:
    Shader m_shader;
    Texture m_white;
};

} // namespace fh::render
