#include "BillboardRenderer.h"

#include "TrackPrograms.h"
#include "WorldRenderer.h"

#include "base/Log.h"
#include "world/ScatterSet.h"
#include "world/TrackPlacements.h"
#include "world/TrackTextures.h"
#include "world/TrackWorld.h"

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <tuple>

namespace openhorizon::render {

namespace {

/// A card as the shaders read it (see BillboardShaders.cpp).
struct GpuCard {
    glm::vec4 position;
    glm::vec4 up;
    glm::vec4 across;
    glm::vec4 groundNormal;
    glm::vec4 uv;
    glm::vec4 colour;
    glm::vec4 baked;
    glm::vec4 extra;
};
static_assert(sizeof(GpuCard) == billboardglsl::kCardVec4s * sizeof(glm::vec4));

/// Vertices a card is drawn with: the quad as two triangles.
constexpr GLint kVerticesPerCard = 6;

/// Colours with w at or below this take objTintColour (c198, set by
/// SetObjectTintColour 0x82E87A78 from untraced callers); those cards are left out.
constexpr float kPlainColour = 0.5F;

/// The shaders of each card type in the main view (0x82ED0648); types 2 and
/// 3, the normal-mapped ones, are not drawn.
std::optional<billboardglsl::Variant> variantOf(std::uint32_t type)
{
    billboardglsl::Variant variant;
    switch (type) {
    case 0:
        return variant;
    case 1:
        variant.fourSided = true;
        return variant;
    case 4:
        variant.nonFacing = true;
        return variant;
    case 5:
        variant.nonFacing = true;
        variant.twoSided = true;
        return variant;
    default:
        return std::nullopt;
    }
}

int variantKey(const billboardglsl::Variant& variant)
{
    return (variant.nonFacing ? 1 : 0) | (variant.fourSided ? 2 : 0) | (variant.twoSided ? 4 : 0);
}

/// MinNDotLAndShadowPower (c162) x: 0 for types 1, 3, 4 and 5, else 0.4
/// (0x833A57C4); y: 1 (0x833A57C8), both set at 0x82447A50.
float minNDotLOf(std::uint32_t type)
{
    constexpr float kMinNDotL = 0.4F;
    return type == 1 || type == 3 || type == 4 || type == 5 ? 0.0F : kMinNDotL;
}
constexpr float kNDotLPower = 1.0F;

/// Whether any part of the box is inside the view's sides and in front of the eye.
bool boxInView(const glm::mat4& viewProjection, const glm::vec3& lo, const glm::vec3& hi)
{
    for (int plane = 0; plane < 5; ++plane) {
        int beyond = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec4 p = viewProjection
                * glm::vec4((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1.0F);
            const float value = plane == 0 ? p.x + p.w
                : plane == 1               ? p.w - p.x
                : plane == 2               ? p.y + p.w
                : plane == 3               ? p.w - p.y
                                           : p.w;
            beyond += value < 0.0F ? 1 : 0;
        }
        if (beyond == 8) {
            return false;
        }
    }
    return true;
}

} // namespace

BillboardRenderer::BillboardRenderer(const world::TrackWorld& world, JobPool& jobs)
    : m_world(world)
    , m_jobs(jobs)
    , m_white(Texture::solid(255, 255, 255, 255))
{
    std::vector<GpuCard> cards;
    if (m_world.placements && m_world.textures) {
        const std::vector<world::ScatterSet>& sets = m_world.placements->scatterSets();
        m_sets.resize(sets.size());
        for (std::size_t s = 0; s < sets.size(); ++s) {
            const world::ScatterSet& set = sets[s];
            SetInfo& info = m_sets[s];
            info.boundsMin = set.boundsMin;
            info.boundsMax = set.boundsMax;
            info.reach = set.billboardReach;
            info.eventProp = set.eventProp;
            info.tagged = !set.eventTag.empty();
            info.routes = set.eventRoutes;
            info.cardTextures.assign(set.billboards.size(), 0);
            std::vector<std::uint32_t> order(set.billboards.size());
            std::iota(order.begin(), order.end(), 0U);
            for (std::size_t c = 0; c < set.billboards.size(); ++c) {
                const std::uint32_t record = set.textureRecords[set.billboards[c].texture];
                info.cardTextures[c] = m_world.textures->recordTexture(record).value_or(0);
            }
            std::ranges::stable_sort(order, [&](std::uint32_t a, std::uint32_t b) {
                return std::tie(info.cardTextures[a], set.billboards[a].type)
                    < std::tie(info.cardTextures[b], set.billboards[b].type);
            });
            for (const std::uint32_t c : order) {
                const world::ScatterSet::Billboard& card = set.billboards[c];
                const std::uint32_t texture = info.cardTextures[c];
                if (!(card.colour.w > kPlainColour)) {
                    continue;
                }
                if (info.batches.empty() || info.batches.back().texture != texture
                    || info.batches.back().type != card.type) {
                    info.batches.push_back({texture, card.type, static_cast<std::uint32_t>(cards.size()), 0});
                }
                ++info.batches.back().count;
                cards.push_back({glm::vec4(card.position, card.fadeStart), glm::vec4(card.up, card.halfWidth),
                    glm::vec4(card.across, card.height), glm::vec4(card.groundNormal, card.nearEnd),
                    glm::vec4(card.uPlus, card.uMinus, card.vFoot, card.vTop), card.colour,
                    glm::vec4(card.bakedLight, card.farEnd), glm::vec4(card.uStep, 0.0F, 0.0F, 0.0F)});
            }
        }
    }
    m_setDrawn.assign(m_sets.size(), 0);
    glCreateBuffers(1, &m_cards);
    glNamedBufferStorage(m_cards, static_cast<GLsizeiptr>(std::max<std::size_t>(cards.size(), 1) * sizeof(GpuCard)),
        cards.empty() ? nullptr : cards.data(), 0);
    glCreateBuffers(1, &m_frameBlock);
    glNamedBufferStorage(m_frameBlock, sizeof(TrackFrameConstants), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateVertexArrays(1, &m_vao);

    // Every texture a card names, loaded in the background.
    for (const SetInfo& set : m_sets) {
        for (const std::uint32_t id : set.cardTextures) {
            if (m_textures.contains(id)) {
                continue;
            }
            m_textures.emplace(id, TextureSlot{});
            m_jobs.submit(0.0F,
                [id, textures = &*m_world.textures, archive = m_world.archive, mutex = m_loadedMutex, loaded = m_loaded,
                    failed = m_failed] {
                try {
                    auto chain = std::make_unique<world::TextureMipChain>(textures->loadTexture(*archive, id));
                    const std::lock_guard lock(*mutex);
                    loaded->push_back({id, std::move(chain)});
                } catch (const std::exception& e) {
                    logWarning("billboards: texture {}: {}", id, e.what());
                    const std::lock_guard lock(*mutex);
                    failed->push_back(id);
                }
            });
        }
    }
    logInfo("billboards: {} cards in {} sets, {} textures", cards.size(), m_sets.size(), m_textures.size());
}

BillboardRenderer::~BillboardRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_cards);
    glDeleteBuffers(1, &m_frameBlock);
}

void BillboardRenderer::update()
{
    std::deque<LoadedTexture> loaded;
    std::vector<std::uint32_t> failed;
    {
        const std::lock_guard lock(*m_loadedMutex);
        loaded.swap(*m_loaded);
        failed.swap(*m_failed);
    }
    for (LoadedTexture& texture : loaded) {
        TextureSlot& slot = m_textures[texture.id];
        slot.texture = Texture::fromMipChain(*texture.chain, true);
        slot.ready = true;
        ++m_stats.texturesLoaded;
    }
    for (const std::uint32_t id : failed) {
        m_textures[id].failed = true;
    }
}

bool BillboardRenderer::texturesSettled() const
{
    return std::ranges::all_of(m_textures, [](const auto& entry) { return entry.second.ready || entry.second.failed; });
}

bool BillboardRenderer::cardTextureReady(std::size_t set, std::size_t card) const
{
    if (set >= m_sets.size() || card >= m_sets[set].cardTextures.size()) {
        return false;
    }
    const auto found = m_textures.find(m_sets[set].cardTextures[card]);
    return found != m_textures.end() && found->second.ready;
}

bool BillboardRenderer::shows(const SetInfo& set, int route) const
{
    if (!set.eventProp) {
        return true;
    }
    return route >= 0 && !set.tagged
        && (set.routes.empty() || std::ranges::find(set.routes, static_cast<std::uint8_t>(route)) != set.routes.end());
}

Shader& BillboardRenderer::program(const billboardglsl::Variant& variant)
{
    const int key = variantKey(variant);
    const auto found = m_programs.find(key);
    if (found != m_programs.end()) {
        return found->second;
    }
    return m_programs
        .emplace(key,
            Shader(std::format("billboard {}", key), billboardglsl::vertexSource(variant),
                billboardglsl::fragmentSource(variant)))
        .first->second;
}

void BillboardRenderer::draw(const FrameParams& frame, int route)
{
    m_stats.sets = 0;
    m_stats.cards = 0;
    m_stats.draws = 0;
    std::fill(m_setDrawn.begin(), m_setDrawn.end(), 0);
    const glm::mat4 viewProjection = frame.projection * frame.view;
    const glm::vec3& camera = frame.cameraPosition;

    // Runs by (type, texture), so each program and texture binds once.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::vector<GLint>, std::vector<GLsizei>>> runs;
    for (std::size_t s = 0; s < m_sets.size(); ++s) {
        const SetInfo& set = m_sets[s];
        if (set.batches.empty() || !shows(set, route)) {
            continue;
        }
        const glm::vec3 offset = glm::clamp(camera, set.boundsMin, set.boundsMax) - camera;
        const float reach = set.reach * kZoomCompensation;
        if (glm::dot(offset, offset) > reach * reach || !boxInView(viewProjection, set.boundsMin, set.boundsMax)) {
            continue;
        }
        ++m_stats.sets;
        bool drawn = false;
        for (const Batch& batch : set.batches) {
            if (!variantOf(batch.type)) {
                continue;
            }
            const auto texture = m_textures.find(batch.texture);
            if (texture == m_textures.end() || !texture->second.ready) {
                continue;
            }
            auto& [firsts, counts] = runs[{batch.type, batch.texture}];
            firsts.push_back(static_cast<GLint>(batch.first) * kVerticesPerCard);
            counts.push_back(static_cast<GLsizei>(batch.count) * kVerticesPerCard);
            m_stats.cards += batch.count;
            drawn = true;
        }
        m_setDrawn[s] = drawn ? 1 : 0;
    }
    if (runs.empty()) {
        return;
    }

    const world::TrackSettings* settings = m_world.settings ? &*m_world.settings : nullptr;
    const TrackFrameConstants constants = TrackFrameConstants::make(
        viewProjection, glm::inverse(frame.view), camera, frame.lighting, frame.time, glm::mat4(1.0F), settings);
    glNamedBufferSubData(m_frameBlock, 0, sizeof(TrackFrameConstants), constants.c.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, billboardglsl::kFrameBinding, m_frameBlock);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, billboardglsl::kCardBinding, m_cards);
    glBindTextureUnit(billboardglsl::kShadowMaskUnit, m_shadowMask != 0 ? m_shadowMask : m_white.id());
    glBindVertexArray(m_vao);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    const Shader* bound = nullptr;
    for (auto& [key, run] : runs) {
        const auto [type, texture] = key;
        Shader& shader = program(*variantOf(type));
        if (&shader != bound) {
            bound = &shader;
            shader.use();
        }
        shader.set("u_zoom", kZoomCompensation);
        shader.set("u_fadeOutWidth", kFadeOutWidth);
        shader.set("u_minNDotL", minNDotLOf(type));
        shader.set("u_nDotLPower", kNDotLPower);
        glBindTextureUnit(billboardglsl::kDiffuseUnit, m_textures.at(texture).texture.id());
        glMultiDrawArrays(GL_TRIANGLES, run.first.data(), run.second.data(), static_cast<GLsizei>(run.first.size()));
        ++m_stats.draws;
    }

    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace openhorizon::render
