#pragma once

#include "AnimatedObjectShaders.h"
#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include "base/JobPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openhorizon::animated {
class AnimatedObjects;
} // namespace openhorizon::animated

namespace openhorizon::world {
struct TextureMipChain;
struct TrackWorld;
} // namespace openhorizon::world

namespace openhorizon::render {

struct FrameParams;

/// Draws the track's animated objects (animated::AnimatedObjects) with the
/// executable's PROC_ANIM_OBJ shaders (AnimatedObjectShaders.h), posed as
/// the objects' last update() left them.
///
/// As the game's draw loop (0x824449B0) does: a rigid batch's
/// InstanceMatrix is its bone's composite matrix (InverseWorld x World,
/// read from the skinning palette at 0x82EC4C64) followed by the
/// placement; a skinned batch's is the placement alone, with every bone's
/// composite matrix in a second stream its vertices index. That the
/// placement follows the composite is inferred (where it enters the matrix
/// was not traced). Each draw sets, as the loop does, pixel c0 = (1, 1, 1,
/// specular intensity), c1.w = 0.7 when its third flag is 2 and 1
/// otherwise, and ModelData.x from the batch's word at +60; UOffset stays
/// 0 (its per-part source, 0x8244544C, was not traced), and so does the
/// model's own EmissiveSwitchOnThreshold (0x82444B40), the scene's being
/// used.
///
/// Render states (0x82445640): a draw whose first flag is set culls
/// nothing, others cull as the game's CULLMODE 6 does (the opposite
/// winding of the track effects' 2); one whose second flag is set adds
/// (source alpha, one) without writing depth; other blended draws blend
/// with (source alpha, one minus source alpha) and write depth; opaque
/// draws use alpha to mask, taken on this single-sample target as keeping
/// pixels whose alpha reaches one half (as WorldRenderer does). That the
/// material words the states read are the draws' flags is inferred.
///
/// Textures come from the track's texture tables through the ids of the
/// object's set (its e list), loaded in the background; a draw waits until
/// its textures are in, and one whose diffuse texture cannot be read is not
/// drawn. Diffuse and emissive maps are sampled as sRGB, normal maps as
/// linear (inferred from their use; the files carry no gamma flag here).
///
/// The objects cast no sun shadows and are not in the screen depth the
/// shadow mask is built from: the game's depth variants exist, but the
/// passes that draw them were not traced.
class AnimatedObjectRenderer {
public:
    struct Stats {
        std::size_t instances = 0;
        std::size_t draws = 0;
        std::size_t triangles = 0;
        std::size_t texturesLoaded = 0;
    };

    /// Uploads the geometry of every object and starts loading its
    /// textures on `jobs`. `objects` and `world` must outlive the renderer.
    AnimatedObjectRenderer(const animated::AnimatedObjects& objects, const world::TrackWorld& world, JobPool& jobs);
    ~AnimatedObjectRenderer();
    AnimatedObjectRenderer(const AnimatedObjectRenderer&) = delete;
    AnimatedObjectRenderer& operator=(const AnimatedObjectRenderer&) = delete;

    /// Uploads the textures the workers finished. Call on the GL thread.
    void update();
    /// Draws what `objects.visible()` lists, into the bound scene target.
    void draw(const FrameParams& frame);
    /// The screen-space sun shadow mask (ShadowMaskSamp), 0 for none.
    void setShadowMask(GLuint texture) { m_shadowMask = texture; }

    const Stats& stats() const { return m_stats; }
    /// The instances (indices into AnimatedObjects::instances()) the last
    /// draw() drew at least one draw of.
    const std::vector<std::uint32_t>& drawnInstances() const { return m_drawnInstances; }
    /// Whether every texture the objects name has loaded or failed.
    bool texturesSettled() const;

private:
    /// Where a batch's converted vertices and indices are in the buffers.
    struct BatchRange {
        GLint baseVertex = 0;
        std::uint32_t firstIndex = 0;
        int geometry = 0;
        bool tangents = false;
    };
    struct TextureSlot {
        Texture texture;
        bool ready = false;
        bool failed = false;
    };
    struct LoadedTexture {
        std::uint32_t id = 0;
        std::unique_ptr<world::TextureMipChain> chain;
    };
    struct DrawItem {
        const Shader* program = nullptr;
        std::uint32_t block = 0;
        /// First bone of the palette, for skinned batches.
        std::uint32_t firstBone = 0;
        std::uint32_t bones = 0;
        GLint baseVertex = 0;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::array<GLuint, 3> textures{};
        bool cullNone = false;
        bool additive = false;
    };

    const Shader& program(const animatedglsl::Variant& variant);
    /// The texture of set texture index `index` of object `object`, or 0
    /// when it is not loaded (yet); `missing` says whether it never will
    /// be.
    GLuint texture(std::uint32_t object, std::int32_t index, bool& missing) const;
    void issue(const std::vector<DrawItem>& items);

    const animated::AnimatedObjects& m_objects;
    const world::TrackWorld& m_world;
    JobPool& m_jobs;
    std::shared_ptr<std::mutex> m_loadedMutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::deque<LoadedTexture>> m_loaded = std::make_shared<std::deque<LoadedTexture>>();
    std::shared_ptr<std::vector<std::uint32_t>> m_failed = std::make_shared<std::vector<std::uint32_t>>();
    std::unordered_map<std::uint32_t, TextureSlot> m_textures;
    /// Textures a draw samples as its normal map, uploaded as linear data.
    std::unordered_set<std::uint32_t> m_normalMaps;
    /// ranges[object][entry][lod][batch].
    std::vector<std::vector<std::array<std::vector<BatchRange>, 3>>> m_ranges;
    std::map<int, Shader> m_programs;
    GLuint m_vao = 0;
    GLuint m_vertices = 0;
    GLuint m_indices = 0;
    GLuint m_frameBlock = 0;
    GLuint m_drawBlocks = 0;
    GLuint m_bones = 0;
    GLint m_blockAlignment = 256;
    GLint m_boneAlignment = 16;
    GLuint m_shadowMask = 0;
    Texture m_white;
    std::vector<std::uint8_t> m_blockData;
    std::vector<glm::vec4> m_boneData;
    std::vector<DrawItem> m_opaque;
    std::vector<DrawItem> m_blended;
    std::vector<std::uint32_t> m_drawnInstances;
    Stats m_stats;
};

} // namespace openhorizon::render
