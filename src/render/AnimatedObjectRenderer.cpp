#include "AnimatedObjectRenderer.h"

#include "TrackPrograms.h"
#include "WorldRenderer.h"

#include "animated/AnimatedObjects.h"
#include "base/Log.h"
#include "world/TrackTextures.h"
#include "world/TrackWorld.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <format>
#include <tuple>

namespace openhorizon::render {

namespace {

/// A vertex as the programs read it: POSITION0 as four halves, NORMAL0 as
/// four signed bytes, TANGENT0 as a 10:10:10:2 word, BLENDINDICES0 and
/// BLENDWEIGHT0 as four bytes each, all in the components' order.
struct GpuVertex {
    std::array<std::uint16_t, 4> position{};
    std::array<std::int8_t, 4> normal{};
    std::uint32_t tangent = 0;
    std::array<std::uint8_t, 4> bones{};
    std::array<std::uint8_t, 4> weights{};
};
static_assert(sizeof(GpuVertex) == 24);

/// Where each element starts in a vertex of a type (-1: absent), after the
/// declarations the executable builds (0x82EC5380).
struct Layout {
    int weights = -1;
    int bones = -1;
    int normal = -1;
    int tangent = -1;
};

constexpr std::array<Layout, 6> kLayouts{{
    {8, 12, 16, 20},
    {8, 12, 16, -1},
    {-1, 8, 12, 16},
    {-1, 8, 12, -1},
    {-1, -1, 8, 12},
    {-1, -1, 8, -1},
}};

/// The four bytes of a byte4 element in component order: the Xenos fetch
/// takes x from the element's last byte.
std::array<std::uint8_t, 4> components(const std::uint8_t* element)
{
    return {element[3], element[2], element[1], element[0]};
}

/// 0 rigid, 1 four bones, 2 one bone, by vertex type.
int geometryOf(std::uint32_t vertexType)
{
    return vertexType <= 1 ? 1 : vertexType <= 3 ? 2 : 0;
}

/// The per-draw block (ProcAnimDraw).
struct DrawBlock {
    std::array<glm::vec4, 4> instanceMatrix{};
    glm::vec4 uOffset{0.0F};
    glm::vec4 c0{1.0F};
    glm::vec4 c1{1.0F};
    glm::vec4 modelData{0.0F};
    glm::vec4 emissiveSwitchOn{0.0F};
};

/// The material flag at the draw's +32 for which the normal map blends in
/// at 0.7 (0x824458E4).
constexpr std::uint32_t kSoftBumpFlag = 2;
constexpr float kSoftBump = 0.7F;

} // namespace

AnimatedObjectRenderer::AnimatedObjectRenderer(
    const animated::AnimatedObjects& objects, const world::TrackWorld& world, JobPool& jobs)
    : m_objects(objects)
    , m_world(world)
    , m_jobs(jobs)
    , m_white(Texture::solid(255, 255, 255, 255))
{
    std::vector<GpuVertex> vertices;
    std::vector<std::uint16_t> indices;
    m_ranges.resize(objects.objects().size());
    for (std::size_t o = 0; o < objects.objects().size(); ++o) {
        const animated::AnimatedObjectSet& set = objects.objects()[o].set;
        const std::uint8_t* data = set.data.data();
        m_ranges[o].resize(set.entries.size());
        for (std::size_t e = 0; e < set.entries.size(); ++e) {
            for (std::size_t level = 0; level < 3; ++level) {
                for (const animated::AnimatedObjectBatch& batch : set.entries[e].lods[level]) {
                    BatchRange range;
                    range.baseVertex = static_cast<GLint>(vertices.size());
                    range.firstIndex = static_cast<std::uint32_t>(indices.size());
                    range.geometry = geometryOf(batch.vertexType);
                    const Layout& layout = kLayouts[batch.vertexType];
                    range.tangents = layout.tangent >= 0;
                    const std::uint32_t stride = animated::AnimatedObjectSet::vertexStride(batch.vertexType);
                    if (batch.vertexOffset != 0) {
                        for (std::uint32_t v = 0; v < batch.vertexCount; ++v) {
                            const std::uint8_t* in = data + batch.vertexOffset + static_cast<std::size_t>(v) * stride;
                            GpuVertex out;
                            for (std::size_t k = 0; k < 4; ++k) {
                                out.position[k] = loadBe16(in + k * 2);
                            }
                            const auto normal = components(in + layout.normal);
                            for (std::size_t k = 0; k < 4; ++k) {
                                out.normal[k] = static_cast<std::int8_t>(normal[k]);
                            }
                            if (layout.tangent >= 0) {
                                out.tangent = loadBe32(in + layout.tangent);
                            }
                            if (layout.bones >= 0) {
                                out.bones = components(in + layout.bones);
                            }
                            if (layout.weights >= 0) {
                                out.weights = components(in + layout.weights);
                            }
                            vertices.push_back(out);
                        }
                    }
                    if (batch.indexOffset != 0) {
                        for (std::uint32_t i = 0; i < batch.indexCount; ++i) {
                            indices.push_back(loadBe16(data + batch.indexOffset + static_cast<std::size_t>(i) * 2));
                        }
                    }
                    m_ranges[o][e][level].push_back(range);
                }
            }
        }
    }

    glCreateBuffers(1, &m_vertices);
    glNamedBufferStorage(m_vertices,
        static_cast<GLsizeiptr>(std::max<std::size_t>(vertices.size(), 1) * sizeof(GpuVertex)),
        vertices.empty() ? nullptr : vertices.data(), 0);
    glCreateBuffers(1, &m_indices);
    glNamedBufferStorage(m_indices,
        static_cast<GLsizeiptr>(std::max<std::size_t>(indices.size(), 1) * sizeof(std::uint16_t)),
        indices.empty() ? nullptr : indices.data(), 0);
    glCreateVertexArrays(1, &m_vao);
    glVertexArrayVertexBuffer(m_vao, 0, m_vertices, 0, sizeof(GpuVertex));
    glVertexArrayElementBuffer(m_vao, m_indices);
    glEnableVertexArrayAttrib(m_vao, 0);
    glVertexArrayAttribFormat(m_vao, 0, 4, GL_HALF_FLOAT, GL_FALSE, offsetof(GpuVertex, position));
    glEnableVertexArrayAttrib(m_vao, 1);
    glVertexArrayAttribFormat(m_vao, 1, 4, GL_BYTE, GL_TRUE, offsetof(GpuVertex, normal));
    glEnableVertexArrayAttrib(m_vao, 2);
    glVertexArrayAttribFormat(m_vao, 2, 4, GL_INT_2_10_10_10_REV, GL_TRUE, offsetof(GpuVertex, tangent));
    glEnableVertexArrayAttrib(m_vao, 3);
    glVertexArrayAttribIFormat(m_vao, 3, 4, GL_UNSIGNED_BYTE, offsetof(GpuVertex, bones));
    glEnableVertexArrayAttrib(m_vao, 4);
    glVertexArrayAttribFormat(m_vao, 4, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(GpuVertex, weights));
    for (GLuint attribute = 0; attribute < 5; ++attribute) {
        glVertexArrayAttribBinding(m_vao, attribute, 0);
    }
    glCreateBuffers(1, &m_frameBlock);
    glNamedBufferStorage(m_frameBlock, sizeof(TrackFrameConstants), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateBuffers(1, &m_drawBlocks);
    glCreateBuffers(1, &m_bones);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_blockAlignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &m_boneAlignment);
    m_blockAlignment = std::max<GLint>(m_blockAlignment, static_cast<GLint>(sizeof(DrawBlock)));
    m_boneAlignment = std::max<GLint>(m_boneAlignment, 16);

    // Every texture any object names, loaded in the background.
    if (!m_world.textures || !m_world.archive) {
        return;
    }
    for (const animated::AnimatedObject& object : objects.objects()) {
        for (const animated::AnimatedObjectEntry& entry : object.set.entries) {
            for (const auto& level : entry.lods) {
                for (const animated::AnimatedObjectBatch& batch : level) {
                    for (const auto* draws : {&batch.opaque, &batch.blended}) {
                        for (const animated::AnimatedObjectDraw& draw : *draws) {
                            const std::int32_t index = draw.textures[1];
                            if (index >= 0 && static_cast<std::size_t>(index) < object.set.textureIds.size()) {
                                m_normalMaps.insert(object.set.textureIds[static_cast<std::size_t>(index)]);
                            }
                        }
                    }
                }
            }
        }
        for (const std::uint32_t id : object.set.textureIds) {
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
                    logWarning("animated objects: texture {}: {}", id, e.what());
                    const std::lock_guard lock(*mutex);
                    failed->push_back(id);
                }
            });
        }
    }
}

AnimatedObjectRenderer::~AnimatedObjectRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vertices);
    glDeleteBuffers(1, &m_indices);
    glDeleteBuffers(1, &m_frameBlock);
    glDeleteBuffers(1, &m_drawBlocks);
    glDeleteBuffers(1, &m_bones);
}

void AnimatedObjectRenderer::update()
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
        slot.texture = Texture::fromMipChain(*texture.chain, !m_normalMaps.contains(texture.id));
        slot.ready = true;
        ++m_stats.texturesLoaded;
    }
    for (const std::uint32_t id : failed) {
        m_textures[id].failed = true;
    }
}

bool AnimatedObjectRenderer::texturesSettled() const
{
    return std::ranges::all_of(m_textures, [](const auto& entry) { return entry.second.ready || entry.second.failed; });
}

GLuint AnimatedObjectRenderer::texture(std::uint32_t object, std::int32_t index, bool& missing) const
{
    missing = false;
    const std::vector<std::uint32_t>& ids = m_objects.objects()[object].set.textureIds;
    if (index < 0 || static_cast<std::size_t>(index) >= ids.size()) {
        missing = true;
        return 0;
    }
    const auto found = m_textures.find(ids[static_cast<std::size_t>(index)]);
    if (found == m_textures.end() || found->second.failed) {
        missing = true;
        return 0;
    }
    return found->second.ready ? found->second.texture.id() : 0;
}

const Shader& AnimatedObjectRenderer::program(const animatedglsl::Variant& variant)
{
    const int key
        = variant.geometry | (variant.normalMap ? 4 : 0) | (variant.emissive ? 8 : 0) | (variant.alphaToMask ? 16 : 0);
    const auto found = m_programs.find(key);
    if (found != m_programs.end()) {
        return found->second;
    }
    return m_programs
        .emplace(key,
            Shader(std::format("animated object {}", key), animatedglsl::vertexSource(variant),
                animatedglsl::fragmentSource(variant)))
        .first->second;
}

void AnimatedObjectRenderer::draw(const FrameParams& frame)
{
    m_stats.instances = 0;
    m_stats.draws = 0;
    m_stats.triangles = 0;
    m_opaque.clear();
    m_blended.clear();
    m_drawnInstances.clear();
    m_blockData.clear();
    m_boneData.clear();

    const glm::mat4 viewProjection = frame.projection * frame.view;
    const world::TrackSettings* settings = m_world.settings ? &*m_world.settings : nullptr;
    const TrackFrameConstants constants = TrackFrameConstants::make(viewProjection, glm::inverse(frame.view),
        frame.cameraPosition, frame.lighting, frame.time, glm::mat4(1.0F), settings);

    const auto blockStride = static_cast<std::size_t>(m_blockAlignment);
    const auto boneStride = static_cast<std::size_t>(m_boneAlignment);
    const std::vector<glm::mat4>& pose = m_objects.pose();
    for (const animated::AnimatedObjects::Visible& visible : m_objects.visible()) {
        const animated::AnimatedObjects::Instance& instance = m_objects.instances()[visible.instance];
        const animated::AnimatedObject& object = m_objects.objects()[instance.object];
        const auto level = static_cast<std::size_t>(visible.lod);
        bool drawn = false;
        for (std::size_t e = 0; e < object.set.entries.size(); ++e) {
            const std::vector<animated::AnimatedObjectBatch>& batches = object.set.entries[e].lods[level];
            const glm::mat4* composites
                = pose.data() + visible.firstMatrix + m_objects.entryMatrixOffset(instance.object, e);
            const std::size_t boneCount = object.granny.models[e].bones.size();
            for (std::size_t b = 0; b < batches.size(); ++b) {
                const animated::AnimatedObjectBatch& batch = batches[b];
                const BatchRange& range = m_ranges[instance.object][e][level][b];
                if (batch.indexCount == 0 || batch.vertexCount == 0) {
                    continue;
                }
                DrawBlock block;
                const glm::mat4 matrix = range.geometry == 0
                    ? instance.world * composites[static_cast<std::size_t>(std::max(batch.bone, 0))]
                    : instance.world;
                for (int row = 0; row < 4; ++row) {
                    block.instanceMatrix[static_cast<std::size_t>(row)] = matrix[row];
                }
                block.modelData = glm::vec4(batch.modelData, 0.0F, 0.0F, 0.0F);
                block.emissiveSwitchOn = constants.c[213];
                std::uint32_t firstBone = 0;
                if (range.geometry != 0) {
                    // Pad the palette to the storage offset alignment.
                    while ((m_boneData.size() * sizeof(glm::vec4)) % boneStride != 0) {
                        m_boneData.emplace_back(0.0F);
                    }
                    firstBone = static_cast<std::uint32_t>(m_boneData.size() / 4);
                    for (std::size_t bone = 0; bone < boneCount; ++bone) {
                        const glm::mat4& m = composites[bone];
                        m_boneData.push_back(m[3]);
                        m_boneData.push_back(m[0]);
                        m_boneData.push_back(m[1]);
                        m_boneData.push_back(m[2]);
                    }
                }
                for (int list = 0; list < 2; ++list) {
                    const std::vector<animated::AnimatedObjectDraw>& draws = list == 0 ? batch.opaque : batch.blended;
                    for (const animated::AnimatedObjectDraw& draw : draws) {
                        bool missing = false;
                        const GLuint diffuse = texture(instance.object, draw.textures[0], missing);
                        if (diffuse == 0) {
                            continue;
                        }
                        bool normalMissing = false;
                        bool emissiveMissing = false;
                        const GLuint normal = draw.textures[1] >= 0 && range.tangents
                            ? texture(instance.object, draw.textures[1], normalMissing)
                            : 0;
                        const GLuint emissive
                            = draw.textures[2] >= 0 ? texture(instance.object, draw.textures[2], emissiveMissing) : 0;
                        // Wait for maps still loading; leave out those that
                        // will not.
                        if ((draw.textures[1] >= 0 && range.tangents && normal == 0 && !normalMissing)
                            || (draw.textures[2] >= 0 && emissive == 0 && !emissiveMissing)) {
                            continue;
                        }
                        animatedglsl::Variant variant;
                        variant.geometry = range.geometry;
                        variant.normalMap = normal != 0;
                        variant.emissive = emissive != 0;
                        variant.alphaToMask = list == 0;
                        DrawBlock drawBlock = block;
                        drawBlock.c0 = glm::vec4(1.0F, 1.0F, 1.0F, draw.specularIntensity);
                        drawBlock.c1 = glm::vec4(0.0F, 1.0F, 1.0F, draw.flags[2] == kSoftBumpFlag ? kSoftBump : 1.0F);
                        const std::size_t at = m_blockData.size();
                        m_blockData.resize(at + blockStride);
                        std::memcpy(m_blockData.data() + at, &drawBlock, sizeof(DrawBlock));
                        DrawItem item;
                        item.program = &program(variant);
                        item.block = static_cast<std::uint32_t>(at / blockStride);
                        item.firstBone = firstBone;
                        item.bones = range.geometry != 0 ? static_cast<std::uint32_t>(boneCount) : 0;
                        item.baseVertex = range.baseVertex;
                        item.firstIndex = range.firstIndex + draw.firstTriangle * 3;
                        item.indexCount = draw.triangleCount * 3;
                        item.textures = {diffuse, normal, emissive};
                        item.cullNone = draw.flags[0] != 0;
                        item.additive = draw.flags[1] != 0;
                        (list == 0 ? m_opaque : m_blended).push_back(item);
                        ++m_stats.draws;
                        m_stats.triangles += draw.triangleCount;
                        drawn = true;
                    }
                }
            }
        }
        if (drawn) {
            ++m_stats.instances;
            m_drawnInstances.push_back(visible.instance);
        }
    }
    if (m_opaque.empty() && m_blended.empty()) {
        return;
    }

    glNamedBufferSubData(m_frameBlock, 0, sizeof(TrackFrameConstants), constants.c.data());
    // New storage every frame, so the driver never waits for the GPU to
    // finish with the last frame's.
    glNamedBufferData(m_drawBlocks, static_cast<GLsizeiptr>(m_blockData.size()), m_blockData.data(), GL_STREAM_DRAW);
    if (!m_boneData.empty()) {
        glNamedBufferData(
            m_bones, static_cast<GLsizeiptr>(m_boneData.size() * sizeof(glm::vec4)), m_boneData.data(), GL_STREAM_DRAW);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER, animatedglsl::kFrameBinding, m_frameBlock);
    glBindVertexArray(m_vao);
    glBindTextureUnit(animatedglsl::kShadowMaskUnit, m_shadowMask != 0 ? m_shadowMask : m_white.id());
    // The game culls CULLMODE 6, the winding opposite to the track
    // effects', whose triangles this renderer keeps with GL's defaults.
    glCullFace(GL_FRONT);
    std::ranges::stable_sort(m_opaque, [](const DrawItem& a, const DrawItem& b) {
        return std::tie(a.program, a.textures) < std::tie(b.program, b.textures);
    });
    issue(m_opaque);
    issue(m_blended);

    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
}

void AnimatedObjectRenderer::issue(const std::vector<DrawItem>& items)
{
    const Shader* bound = nullptr;
    for (const DrawItem& item : items) {
        if (item.program != bound) {
            bound = item.program;
            bound->use();
        }
        if (item.cullNone) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
        }
        if (&items == &m_opaque) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        } else if (item.additive) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glDepthMask(GL_FALSE);
        } else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_TRUE);
        }
        glBindBufferRange(GL_UNIFORM_BUFFER, animatedglsl::kDrawBinding, m_drawBlocks,
            static_cast<GLintptr>(item.block) * m_blockAlignment, sizeof(DrawBlock));
        if (item.bones > 0) {
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, animatedglsl::kBoneBinding, m_bones,
                static_cast<GLintptr>(item.firstBone) * 4 * static_cast<GLintptr>(sizeof(glm::vec4)),
                static_cast<GLsizeiptr>(item.bones) * 4 * static_cast<GLsizeiptr>(sizeof(glm::vec4)));
        }
        glBindTextureUnit(animatedglsl::kDiffuseUnit, item.textures[0]);
        if (item.textures[1] != 0) {
            glBindTextureUnit(animatedglsl::kNormalUnit, item.textures[1]);
        }
        if (item.textures[2] != 0) {
            glBindTextureUnit(animatedglsl::kEmissiveUnit, item.textures[2]);
        }
        glDrawElementsBaseVertex(GL_TRIANGLES, static_cast<GLsizei>(item.indexCount), GL_UNSIGNED_SHORT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(item.firstIndex) * sizeof(std::uint16_t)),
            item.baseVertex);
    }
}

} // namespace openhorizon::render
