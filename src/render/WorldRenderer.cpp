#include "WorldRenderer.h"

#include "base/Log.h"
#include "world/TrackTextures.h"

#include <algorithm>
#include <cstring>

namespace fh::render {

namespace {

constexpr float kTileSize = 500.0F;
/// Tiles closer than this must be complete before nearbyReady() says so.
constexpr float kNearbyRadius = 600.0F;
/// Texture memory kept beyond what visible tiles use, so tiles that come
/// back into view find their textures loaded.
constexpr std::size_t kTextureBudget = std::size_t{1} << 30;
constexpr int kMaxUploadsPerFrame = 6;
constexpr int kMaxTextureUploadsPerFrame = 48;

const char* const kVertexSource = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexcoord0;
layout(location = 3) in vec2 aTexcoord1;
layout(location = 4) in vec4 aColour;
uniform mat4 uViewProjection;
// Identity for the static world; places moving objects (knocked-over props).
uniform mat4 uModel;
out vec3 vPosition;
out vec3 vNormal;
out vec2 vTexcoord0;
out vec2 vTexcoord1;
out vec4 vColour;
void main()
{
    vec4 world = uModel * vec4(aPosition, 1.0);
    vPosition = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vTexcoord0 = aTexcoord0;
    vTexcoord1 = aTexcoord1;
    vColour = aColour;
    gl_Position = uViewProjection * world;
}
)";

const char* const kFragmentSource = R"(#version 450 core
in vec3 vPosition;
in vec3 vNormal;
in vec2 vTexcoord0;
in vec2 vTexcoord1;
in vec4 vColour;
layout(location = 0) out vec4 fragColour;
layout(binding = 0) uniform sampler2D uDiffuse;
layout(binding = 1) uniform sampler2D uLayerB;
layout(binding = 2) uniform sampler2D uLayerC;
layout(binding = 3) uniform sampler2D uSplat;
layout(binding = 4) uniform sampler2D uOcclusion;
uniform int uShading;
uniform vec2 uScaleA;
uniform vec2 uScaleB;
uniform vec2 uScaleC;
uniform vec3 uCamera;
uniform vec3 uSunDirection;
uniform vec3 uSunColour;
uniform vec3 uSkyColour;
uniform vec3 uHorizonColour;
uniform vec3 uGroundColour;
uniform vec3 uFogColour;
uniform float uFogDensity;
uniform float uTime;

const int kPlain = 0;
const int kSplat = 1;
const int kVertexBlend = 2;
const int kWater = 3;

// Ground mixes up to three layers: by the splat map (red weighs B, green C,
// the rest A) or by the vertex colour's alpha, then darkens them by the
// baked occlusion map on the second texture coordinates.
vec3 groundColour()
{
    vec3 a = texture(uDiffuse, vTexcoord0 * uScaleA).rgb;
    vec3 b = texture(uLayerB, vTexcoord0 * uScaleB).rgb;
    vec3 blended;
    if (uShading == kSplat) {
        vec3 c = texture(uLayerC, vTexcoord0 * uScaleC).rgb;
        vec2 w = texture(uSplat, vTexcoord1).rg;
        float wa = max(1.0 - w.x - w.y, 0.0);
        blended = (a * wa + b * w.x + c * w.y) / max(wa + w.x + w.y, 1e-4);
    } else {
        blended = mix(a, b, vColour.a);
    }
    return blended * texture(uOcclusion, vTexcoord1).r;
}

vec3 applyFog(vec3 colour, float distance)
{
    float fog = 1.0 - exp(-distance * uFogDensity);
    return mix(colour, uFogColour, clamp(fog, 0.0, 1.0));
}

void main()
{
    vec3 toEye = uCamera - vPosition;
    float distance = length(toEye);
    toEye /= max(distance, 1e-4);
    vec3 toSun = -uSunDirection;

    if (uShading == kWater) {
        // Two scrolling normal-map samples ripple the surface; the colour is
        // a deep tint mixed towards the sky by the Fresnel term.
        vec2 flow = vec2(uTime * 0.013, uTime * 0.008);
        vec3 n1 = texture(uDiffuse, vTexcoord0 * 4.0 + flow).xyz * 2.0 - 1.0;
        vec3 n2 = texture(uDiffuse, vTexcoord0 * 7.0 - flow.yx).xyz * 2.0 - 1.0;
        vec3 surfaceNormal = normalize(vec3((n1.x + n2.x) * 0.1, 1.0, (n1.y + n2.y) * 0.1));
        float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(toEye, surfaceNormal), 0.0, 1.0), 5.0);
        vec3 colour = mix(vec3(0.02, 0.07, 0.08), uHorizonColour, fresnel);
        float glint = pow(max(dot(reflect(-toEye, surfaceNormal), toSun), 0.0), 200.0);
        colour += uSunColour * glint * 4.0;
        fragColour = vec4(applyFog(colour, distance), mix(0.7, 1.0, fresnel));
        return;
    }

    vec3 normal = normalize(gl_FrontFacing ? vNormal : -vNormal);
    vec3 albedo;
    if (uShading == kPlain) {
        vec4 texel = texture(uDiffuse, vTexcoord0);
        // Foliage, fences and decals are cut-outs.
        if (texel.a < 0.5) {
            discard;
        }
        albedo = texel.rgb;
    } else {
        albedo = groundColour();
    }
    float sun = max(dot(normal, toSun), 0.0);
    // Light from the whole sky dome: the zenith and the brighter horizon
    // above, bounce from the ground below.
    vec3 skyLight = (uSkyColour + uHorizonColour) * 0.5;
    vec3 ambient = mix(uGroundColour, skyLight, 0.5 + 0.5 * normal.y);
    vec3 colour = albedo * (ambient * 0.9 + uSunColour * sun);
    fragColour = vec4(applyFog(colour, distance), 1.0);
}
)";

GLint shadingCode(world::TileMesh::Shading shading)
{
    switch (shading) {
    case world::TileMesh::Shading::Plain:
        return 0;
    case world::TileMesh::Shading::Splat:
        return 1;
    case world::TileMesh::Shading::VertexBlend:
        return 2;
    case world::TileMesh::Shading::Water:
        return 3;
    }
    return 0;
}

} // namespace

WorldRenderer::WorldRenderer(const world::TrackWorld& world, JobPool& jobs)
    : m_world(world)
    , m_jobs(jobs)
    , m_grid(world.index, kTileSize)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
    , m_shader("world", kVertexSource, kFragmentSource)
    , m_white(Texture::solid(200, 200, 200, 255))
    , m_flatNormal(Texture::solid(128, 128, 255, 255))
{
    m_tiles.resize(m_grid.tiles().size());
    logInfo("world: {} tiles of {:.0f} m", m_tiles.size(), kTileSize);
}

WorldRenderer::~WorldRenderer()
{
    // Queued jobs capture this renderer and write their results into it;
    // the flag makes any that still start return at once, and the wait
    // covers those already running.
    m_alive->store(false);
    m_jobs.clearAndWait();
    for (TileState& tile : m_tiles) {
        if (tile.mesh) {
            destroy(*tile.mesh);
        }
    }
}

void WorldRenderer::destroy(GpuMesh& mesh)
{
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vbo);
    glDeleteBuffers(1, &mesh.ibo);
    mesh.vao = mesh.vbo = mesh.ibo = 0;
}

std::shared_ptr<WorldRenderer::GpuMesh> WorldRenderer::upload(const world::TileMesh& mesh)
{
    auto gpu = std::shared_ptr<GpuMesh>(new GpuMesh, [](GpuMesh* m) {
        destroy(*m);
        delete m;
    });
    using world::TileMesh;
    glCreateBuffers(1, &gpu->vbo);
    glNamedBufferStorage(gpu->vbo, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)),
        mesh.vertices.data(), 0);
    glCreateBuffers(1, &gpu->ibo);
    glNamedBufferStorage(gpu->ibo, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
        mesh.indices.data(), 0);
    glCreateVertexArrays(1, &gpu->vao);
    constexpr GLsizei stride = TileMesh::kFloatsPerVertex * sizeof(float);
    glVertexArrayVertexBuffer(gpu->vao, 0, gpu->vbo, 0, stride);
    glVertexArrayElementBuffer(gpu->vao, gpu->ibo);
    const auto attribute = [&](GLuint index, GLint size, GLenum type, GLboolean normalised, int floatOffset) {
        glEnableVertexArrayAttrib(gpu->vao, index);
        glVertexArrayAttribFormat(gpu->vao, index, size, type, normalised,
            static_cast<GLuint>(floatOffset * static_cast<int>(sizeof(float))));
        glVertexArrayAttribBinding(gpu->vao, index, 0);
    };
    attribute(0, 3, GL_FLOAT, GL_FALSE, 0);
    attribute(1, 3, GL_FLOAT, GL_FALSE, TileMesh::kNormal);
    attribute(2, 2, GL_FLOAT, GL_FALSE, TileMesh::kTexcoord);
    attribute(3, 2, GL_FLOAT, GL_FALSE, TileMesh::kSecondTexcoord);
    // The colour's four bytes share one float's slot.
    attribute(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, TileMesh::kColour);
    gpu->batches = mesh.batches;
    gpu->triangles = mesh.triangleCount();
    return gpu;
}

void WorldRenderer::requestTexture(std::uint32_t id, float priority)
{
    if (id == world::TileMesh::kNoTexture || !m_world.textures) {
        return;
    }
    TextureSlot& slot = m_textures[id];
    slot.lastUsed = m_frame;
    if (slot.requested) {
        return;
    }
    slot.requested = true;
    m_jobs.submit(priority, [this, id, alive = m_alive] {
        if (!alive->load()) {
            return;
        }
        try {
            auto chain = std::make_unique<world::TextureMipChain>(
                m_world.textures->loadTexture(*m_world.archive, id));
            const std::lock_guard lock(m_resultMutex);
            m_loadedTextures.push_back({id, std::move(chain)});
        } catch (const std::exception& e) {
            logDebug("world: texture {:08X}: {}", id, e.what());
            const std::lock_guard lock(m_resultMutex);
            m_failedTextures.push_back(id);
        }
    });
}

bool WorldRenderer::texturesReady(const world::TileMesh& mesh) const
{
    for (const world::TileMesh::Batch& batch : mesh.batches) {
        for (std::uint32_t id : batch.textures()) {
            if (id == world::TileMesh::kNoTexture) {
                continue;
            }
            const auto it = m_textures.find(id);
            if (it == m_textures.end() || (!it->second.ready && !it->second.failed)) {
                return false;
            }
        }
    }
    return true;
}

const Texture& WorldRenderer::textureOr(std::uint32_t id, const Texture& fallback)
{
    const auto it = m_textures.find(id);
    if (it == m_textures.end() || !it->second.ready) {
        return fallback;
    }
    it->second.lastUsed = m_frame;
    return it->second.texture;
}

void WorldRenderer::evictTextures()
{
    std::size_t total = 0;
    for (const auto& [id, slot] : m_textures) {
        total += slot.texture.byteSize();
    }
    m_stats.textureBytes = total;
    if (total <= kTextureBudget) {
        return;
    }
    // Oldest first, but never one drawn in the last few frames.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
    for (const auto& [id, slot] : m_textures) {
        if (slot.ready && slot.lastUsed + 3 < m_frame) {
            order.emplace_back(slot.lastUsed, id);
        }
    }
    std::sort(order.begin(), order.end());
    for (const auto& [used, id] : order) {
        if (total <= kTextureBudget * 3 / 4) {
            break;
        }
        total -= m_textures[id].texture.byteSize();
        m_textures.erase(id);
    }
    m_stats.textureBytes = total;
}

void WorldRenderer::setHiddenChunks(const std::vector<bool>& hidden)
{
    for (std::size_t tile : m_grid.update(world::EventPropFilter{}, hidden)) {
        // Rebuilt at the current distance on the next update; the old mesh
        // stays on screen meanwhile.
        m_tiles[tile].requestedState = -1;
        m_tiles[tile].pending.reset();
        m_tiles[tile].pendingState = -1;
    }
}

int WorldRenderer::addDynamic(std::vector<std::uint32_t> chunks)
{
    const int handle = m_nextDynamic++;
    m_dynamics.emplace(handle, Dynamic{});
    m_jobs.submit(-2000.0F, [this, handle, chunks = std::move(chunks), alive = m_alive] {
        if (!alive->load()) {
            return;
        }
        auto mesh = std::make_unique<world::TileMesh>(world::buildTileMesh(
            *m_world.archive, m_world.index, chunks, m_world.textures ? &*m_world.textures : nullptr));
        const std::lock_guard lock(m_resultMutex);
        m_builtDynamics.push_back({handle, std::move(mesh)});
    });
    return handle;
}

void WorldRenderer::setDynamicTransform(int handle, const glm::mat4& transform)
{
    const auto it = m_dynamics.find(handle);
    if (it != m_dynamics.end()) {
        it->second.transform = transform;
    }
}

void WorldRenderer::removeDynamic(int handle)
{
    m_dynamics.erase(handle);
}

void WorldRenderer::update(const glm::vec3& camera)
{
    ++m_frame;
    m_camera = camera;
    m_cameraZone = m_world.zones ? m_world.zones->zoneAt(camera.x, camera.z) : -1;

    // Finished work from the workers.
    std::deque<BuiltTile> built;
    std::deque<LoadedTexture> loaded;
    std::vector<std::uint32_t> failed;
    {
        const std::lock_guard lock(m_resultMutex);
        built.swap(m_builtTiles);
        const std::size_t take = std::min<std::size_t>(m_loadedTextures.size(), kMaxTextureUploadsPerFrame);
        for (std::size_t i = 0; i < take; ++i) {
            loaded.push_back(std::move(m_loadedTextures.front()));
            m_loadedTextures.pop_front();
        }
        failed.swap(m_failedTextures);
        for (BuiltDynamic& dynamic : m_builtDynamics) {
            const auto it = m_dynamics.find(dynamic.handle);
            if (it != m_dynamics.end() && !dynamic.mesh->indices.empty()) {
                it->second.mesh = upload(*dynamic.mesh);
                for (const world::TileMesh::Batch& batch : dynamic.mesh->batches) {
                    for (std::uint32_t id : batch.textures()) {
                        requestTexture(id, 0.0F);
                    }
                }
            }
        }
        m_builtDynamics.clear();
    }
    for (std::uint32_t id : failed) {
        m_textures[id].failed = true;
    }
    for (LoadedTexture& texture : loaded) {
        TextureSlot& slot = m_textures[texture.id];
        // Normal maps (BC5) and masks are linear; colour maps are sRGB.
        const bool colour = texture.chain->format != world::TextureSurface::Format::Bc5;
        slot.texture = Texture::fromMipChain(*texture.chain, colour);
        slot.ready = true;
        slot.requested = true;
    }
    for (BuiltTile& result : built) {
        TileState& tile = m_tiles[result.tile];
        tile.building = false;
        if (result.state != tile.requestedState) {
            continue;
        }
        tile.pending = std::move(result.mesh);
        tile.pendingState = result.state;
        const float distance = m_grid.distanceTo(m_grid.tiles()[result.tile], camera.x, camera.z);
        for (const world::TileMesh::Batch& batch : tile.pending->batches) {
            for (std::uint32_t id : batch.textures()) {
                requestTexture(id, distance);
            }
        }
    }

    // Promote pending meshes whose textures are in.
    int uploads = 0;
    for (TileState& tile : m_tiles) {
        if (!tile.pending || uploads >= kMaxUploadsPerFrame) {
            continue;
        }
        if (!texturesReady(*tile.pending)) {
            continue;
        }
        tile.mesh = tile.pending->indices.empty() ? nullptr : upload(*tile.pending);
        tile.builtState = tile.pendingState;
        tile.pending.reset();
        tile.pendingState = -1;
        ++uploads;
    }

    // Schedule builds for tiles whose level-of-detail state changed.
    bool nearbyReady = true;
    const auto& tiles = m_grid.tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        TileState& state = m_tiles[i];
        const float distance = m_grid.distanceTo(tiles[i], camera.x, camera.z);
        if (distance > m_drawDistance) {
            if (state.mesh || state.pending) {
                state.mesh.reset();
                state.pending.reset();
                state.builtState = -1;
                state.requestedState = -1;
            }
            continue;
        }
        const int wanted = m_grid.stateAt(tiles[i], distance);
        if (distance < kNearbyRadius && (state.builtState != wanted || state.pending)) {
            nearbyReady = false;
        }
        if (wanted == state.requestedState || state.building) {
            continue;
        }
        state.requestedState = wanted;
        state.building = true;
        std::vector<std::uint32_t> chunks = m_grid.chunksAt(tiles[i], distance);
        m_jobs.submit(distance, [this, i, wanted, chunks = std::move(chunks), alive = m_alive] {
            if (!alive->load()) {
                return;
            }
            auto mesh = std::make_unique<world::TileMesh>(world::buildTileMesh(*m_world.archive, m_world.index,
                chunks, m_world.textures ? &*m_world.textures : nullptr));
            const std::lock_guard lock(m_resultMutex);
            m_builtTiles.push_back({i, wanted, std::move(mesh)});
        });
    }
    m_nearbyReady = nearbyReady;
    evictTextures();
    m_stats.pendingJobs = m_jobs.pending();
    m_stats.texturesLoaded = static_cast<std::size_t>(
        std::count_if(m_textures.begin(), m_textures.end(), [](const auto& entry) { return entry.second.ready; }));
}

void WorldRenderer::drawBatches(const GpuMesh& mesh, bool waterPass)
{
    for (const world::TileMesh::Batch& batch : mesh.batches) {
        const bool water = batch.shading == world::TileMesh::Shading::Water;
        if (water != waterPass || batch.indexCount == 0) {
            continue;
        }
        if (batch.backdrop && batch.chunk != world::TileMesh::kMergedChunks
            && !m_world.index.chunks()[batch.chunk].visibleFrom(m_cameraZone)) {
            continue;
        }
        using Layer = world::TileMesh::Layer;
        const Texture& fallback = water ? m_flatNormal : m_white;
        textureOr(batch.texture, fallback).bind(0);
        textureOr(batch.layers[Layer::LayerB], textureOr(batch.texture, fallback)).bind(1);
        textureOr(batch.layers[Layer::LayerC], textureOr(batch.texture, fallback)).bind(2);
        textureOr(batch.layers[Layer::SplatMap], m_white).bind(3);
        textureOr(batch.layers[Layer::OcclusionMap], m_white).bind(4);
        m_shader.set("uShading", shadingCode(batch.shading));
        m_shader.set("uScaleA", glm::vec2(batch.layerScales[0], batch.layerScales[1]));
        m_shader.set("uScaleB", glm::vec2(batch.layerScales[2], batch.layerScales[3]));
        m_shader.set("uScaleC", glm::vec2(batch.layerScales[4], batch.layerScales[5]));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
        if (!waterPass) {
            ++m_stats.batches;
            m_stats.triangles += batch.indexCount / 3;
        }
    }
}

void WorldRenderer::draw(const FrameParams& frame)
{
    m_stats.tilesDrawn = 0;
    m_stats.triangles = 0;
    m_stats.batches = 0;

    m_shader.use();
    m_shader.set("uViewProjection", frame.projection * frame.view);
    m_shader.set("uModel", glm::mat4(1.0F));
    m_shader.set("uCamera", frame.cameraPosition);
    m_shader.set("uSunDirection", frame.sunDirection);
    m_shader.set("uSunColour", frame.sunColour);
    m_shader.set("uSkyColour", frame.skyColour);
    m_shader.set("uHorizonColour", frame.horizonColour);
    m_shader.set("uGroundColour", frame.groundColour);
    m_shader.set("uFogColour", frame.fogColour);
    m_shader.set("uFogDensity", frame.fogDensity);
    m_shader.set("uTime", frame.time);

    const glm::mat4 viewProjection = frame.projection * frame.view;
    const auto& tiles = m_grid.tiles();
    // Opaque first; water is drawn after, blended over what is below it.
    for (int pass = 0; pass < 2; ++pass) {
        const bool waterPass = pass == 1;
        if (waterPass) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const TileState& state = m_tiles[i];
            if (!state.mesh) {
                continue;
            }
            // Cull the whole tile against the view frustum by its bounds.
            const glm::vec3& lo = tiles[i].boundsMin;
            const glm::vec3& hi = tiles[i].boundsMax;
            bool outside = false;
            for (int plane = 0; plane < 5 && !outside; ++plane) {
                int beyond = 0;
                for (int corner = 0; corner < 8; ++corner) {
                    const glm::vec4 p = viewProjection
                        * glm::vec4((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                            (corner & 4) ? hi.z : lo.z, 1.0F);
                    const float value = plane == 0 ? p.x + p.w
                        : plane == 1               ? p.w - p.x
                        : plane == 2               ? p.y + p.w
                        : plane == 3               ? p.w - p.y
                                                   : p.w;
                    if (value < 0.0F) {
                        ++beyond;
                    }
                }
                outside = beyond == 8;
            }
            if (outside) {
                continue;
            }
            glBindVertexArray(state.mesh->vao);
            if (!waterPass) {
                ++m_stats.tilesDrawn;
            }
            drawBatches(*state.mesh, waterPass);
        }
        m_shader.set("uModel", glm::mat4(1.0F));
        for (const auto& [handle, dynamic] : m_dynamics) {
            if (!dynamic.mesh) {
                continue;
            }
            m_shader.set("uModel", dynamic.transform);
            glBindVertexArray(dynamic.mesh->vao);
            drawBatches(*dynamic.mesh, waterPass);
        }
        m_shader.set("uModel", glm::mat4(1.0F));
        if (waterPass) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }
    glBindVertexArray(0);
}

} // namespace fh::render
