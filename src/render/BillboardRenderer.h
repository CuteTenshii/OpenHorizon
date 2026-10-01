#pragma once

#include "BillboardShaders.h"
#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include "base/JobPool.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace openhorizon::world {
struct TextureMipChain;
struct TrackWorld;
} // namespace openhorizon::world

namespace openhorizon::render {

struct FrameParams;

/// Draws the far tier of the procedural copies (world::ScatterSet::Billboard)
/// with the PROC_MODEL_BILLBOARD shaders. Verified: the main view skips sets
/// past their reach (0x82EE1F04), queues groups by type (slot 41, 0x82447B70)
/// and draws each queue at once (slot 40, 0x82447088) with alpha to mask on,
/// blending and culling off (0x82475878). The group test is left to the
/// shaders' fade. Not drawn: shadow, depth, cube and mirror views, the
/// normal-mapped types 2 and 3, and objTintColour (not traced).
class BillboardRenderer {
public:
    struct Stats {
        /// Sets within their reach and in view at the last draw.
        std::size_t sets = 0;
        /// Cards handed to the GPU at the last draw (their shaders fade them).
        std::size_t cards = 0;
        std::size_t draws = 0;
        std::size_t texturesLoaded = 0;
    };

    BillboardRenderer(const world::TrackWorld& world, JobPool& jobs);
    ~BillboardRenderer();
    BillboardRenderer(const BillboardRenderer&) = delete;
    BillboardRenderer& operator=(const BillboardRenderer&) = delete;

    /// Takes the textures the workers finished; once per frame on the GL thread.
    void update();
    /// Draws the cards of the sets `route` shows into the bound scene target.
    void draw(const FrameParams& frame, int route);
    /// The screen-space sun shadow mask (ShadowMaskSamp), 0 for none.
    void setShadowMask(GLuint texture) { m_shadowMask = texture; }

    const Stats& stats() const { return m_stats; }
    /// Whether the last draw sent set `set`'s loaded cards (index into scatterSets()).
    bool setDrawn(std::size_t set) const { return set < m_setDrawn.size() && m_setDrawn[set] != 0; }
    /// Whether the texture of card `card` of set `set` is loaded.
    bool cardTextureReady(std::size_t set, std::size_t card) const;
    /// Whether every texture a card names has loaded or failed.
    bool texturesSettled() const;

    /// Zoom compensation S: 1 while driving (0x8246E2D4).
    static constexpr float kZoomCompensation = 1.0F;
    /// Fade-out width before a card's far end (-FadeValues.w, 0x833A57BC).
    static constexpr float kFadeOutWidth = 50.0F;

private:
    /// Cards of one set sharing a texture and a type, contiguous in the buffer.
    struct Batch {
        std::uint32_t texture = 0;
        std::uint32_t type = 0;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };
    struct SetInfo {
        std::vector<Batch> batches;
        /// Texture id of each card, in the set's own order.
        std::vector<std::uint32_t> cardTextures;
        glm::vec3 boundsMin{0.0F};
        glm::vec3 boundsMax{0.0F};
        float reach = 0.0F;
        bool eventProp = false;
        bool tagged = false;
        std::vector<std::uint8_t> routes;
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

    Shader& program(const billboardglsl::Variant& variant);
    bool shows(const SetInfo& set, int route) const;

    const world::TrackWorld& m_world;
    JobPool& m_jobs;
    std::shared_ptr<std::mutex> m_loadedMutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::deque<LoadedTexture>> m_loaded = std::make_shared<std::deque<LoadedTexture>>();
    std::shared_ptr<std::vector<std::uint32_t>> m_failed = std::make_shared<std::vector<std::uint32_t>>();
    std::unordered_map<std::uint32_t, TextureSlot> m_textures;
    std::vector<SetInfo> m_sets;
    std::vector<std::uint8_t> m_setDrawn;
    std::unordered_map<int, Shader> m_programs;
    GLuint m_cards = 0;
    GLuint m_frameBlock = 0;
    GLuint m_vao = 0;
    GLuint m_shadowMask = 0;
    Texture m_white;
    Stats m_stats;
};

} // namespace openhorizon::render
