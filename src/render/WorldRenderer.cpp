#include "WorldRenderer.h"

#include "base/Log.h"
#include "world/TrackTextures.h"
#include "xenos/GlslTranslator.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

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
/// Job priority of translating the track effects: before any tile (tiles
/// use their distance in metres, moving objects -2000).
constexpr float kTranslatePriority = -3000.0F;
constexpr std::size_t kFrameBlockBytes = sizeof(TrackFrameConstants);
/// Without bindless textures each batch's material is a uniform block of
/// its own, on the 256-byte offset alignment.
constexpr std::size_t kMaterialBlockBytes = 256;
/// Texture handle table entries allocated at first; it doubles as needed.
constexpr std::size_t kInitialHandles = 4096;
constexpr std::size_t kProgramBlockBytes = 256 * sizeof(glm::vec4);
/// Offsets in the fixed block buffer: the translator's control block, then
/// its system block without and with the alpha-to-mask emulation, each on a
/// 256-byte boundary.
constexpr std::size_t kControlOffset = 0;
constexpr std::size_t kSystemOffset = 256;
constexpr std::size_t kSystemAlphaToMaskOffset = 512;
constexpr std::size_t kFixedBytes = 768;
/// Xbox 360 D3DCULL_NONE.
constexpr std::uint32_t kCullNone = 0;
/// Xbox 360 D3DCULL_CCW.
constexpr std::uint32_t kCullCounterClockwise = 6;
/// Direct3D's alpha comparison "greater or equal", in the translator's
/// alphaTest numbering.
constexpr float kAlphaGreaterEqual = 7.0F;

GLenum blendFactor(std::uint32_t xenosBlend)
{
    switch (xenosBlend) {
    case 0:
        return GL_ZERO;
    case 6:
        return GL_SRC_ALPHA;
    case 7:
        return GL_ONE_MINUS_SRC_ALPHA;
    default:
        return GL_ONE;
    }
}

bool hasExtension(std::string_view name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const auto* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (extension != nullptr && name == extension) {
            return true;
        }
    }
    return false;
}

bool bindlessSupported()
{
    return glGetTextureHandleARB != nullptr && glMakeTextureHandleResidentARB != nullptr
        && glMakeTextureHandleNonResidentARB != nullptr && hasExtension("GL_ARB_bindless_texture")
        && hasExtension("GL_ARB_shader_draw_parameters");
}

GLuint blackVolume()
{
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_3D, 1, &id);
    glTextureStorage3D(id, 1, GL_RGBA8, 1, 1, 1);
    const std::array<std::uint8_t, 4> black{0, 0, 0, 255};
    glTextureSubImage3D(id, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black.data());
    return id;
}

GLuint blackCubeMap()
{
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &id);
    glTextureStorage2D(id, 1, GL_RGBA8, 1, 1);
    const std::array<std::uint8_t, 4> black{0, 0, 0, 255};
    for (int face = 0; face < 6; ++face) {
        glTextureSubImage3D(id, 0, 0, 0, face, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black.data());
    }
    return id;
}

} // namespace

const char* const kSceneLightingGlsl = R"(
uniform vec3 uSunDirection;
uniform vec3 uSunColour;
uniform vec3 uAmbientTop;
uniform vec3 uAmbientBottom;
uniform vec3 uCarAmbientTop;
uniform vec3 uCarAmbientBottom;
uniform vec3 uSkyTop;
uniform vec3 uSkyBottom;
uniform float uSkyColourPower;
uniform float uSkyClampTop;
uniform float uSkyClampBottom;
uniform float uSkyGain;
uniform vec3 uHazeColour;
uniform vec3 uHazeDirection;
uniform float uHazeIntensity;
uniform float uHazePower;
uniform float uHazeEllipticalScale;
uniform vec3 uFogColour;
uniform vec3 uFogScatterColour;
uniform float uFogScatterPower;
uniform float uFogDensity;
uniform float uFogStart;
uniform float uFogHeightFadeAmount;
uniform float uFogHeightFadeStart;
uniform float uFogHeightFadeEnd;
uniform float uFogScatterStart;
uniform float uFogScatterRamp;

uniform float uFogSkyHeightFadeStart;
uniform float uFogSkyHeightFadeEnd;

float smoothRamp(float value, float start, float end)
{
    float x = clamp((value - start) / max(end - start, 1e-4), 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// The game raises to powers through log and exp, which gives 0 for a zero
// base whatever the power.
float safePow(float base, float power)
{
    return base > 0.0 ? pow(base, power) : 0.0;
}

float fogExtinction(float distance)
{
    return 1.0 - exp2(-uFogDensity * max(distance - uFogStart, 0.0));
}

vec3 fogColourTowards(vec3 view)
{
    float scatter = safePow(max(dot(view, -uSunDirection), 0.0), uFogScatterPower);
    return mix(uFogColour, uFogScatterColour, scatter);
}

vec3 skyColour(vec3 direction)
{
    vec3 view = normalize(direction);
    // The dome's per-vertex w, linear in elevation; the dome stops 6
    // degrees below the horizon, where w reaches 0.
    float w = max(degrees(asin(clamp(view.y, -1.0, 1.0))) / 90.0 + 1.0 / 15.0, 0.0);
    float t = clamp((w - uSkyClampBottom) / max(uSkyClampTop - uSkyClampBottom, 1e-4), 0.0, 1.0);
    vec3 base = mix(uSkyBottom, uSkyTop, safePow(t, uSkyColourPower));
    float ellipse = clamp(1.0 + (uHazeEllipticalScale - 1.0) * view.y, 0.0, 1.0);
    float haze = uHazeIntensity * safePow(ellipse * max(dot(view, uHazeDirection), 0.0), uHazePower);
    vec3 sky = uSkyGain * clamp(mix(base, uHazeColour, haze), 0.0, 1.0);
    const float kSkyFogRing = 15000.0;
    float band = min(uFogDensity * kSkyFogRing, 1.0)
        * (1.0 - smoothRamp(kSkyFogRing * view.y, uFogSkyHeightFadeStart, uFogSkyHeightFadeEnd));
    return mix(sky, fogColourTowards(view) * uSkyGain, band);
}

vec3 hemisphereAmbient(vec3 normal, vec3 top, vec3 bottom)
{
    return mix(bottom, top, 0.5 + 0.5 * normal.y);
}

vec3 applyFog(vec3 colour, vec3 position, vec3 camera)
{
    vec3 ray = position - camera;
    float distance = length(ray);
    vec3 view = ray / max(distance, 1e-4);
    float amount = fogExtinction(distance)
        * (1.0 - uFogHeightFadeAmount * smoothRamp(position.y, uFogHeightFadeStart, uFogHeightFadeEnd));
    float scatter = safePow(max(dot(view, -uSunDirection), 0.0), uFogScatterPower)
        * clamp((distance - uFogScatterStart) / max(uFogScatterRamp, 1e-4), 0.0, 1.0);
    return mix(colour, mix(uFogColour, uFogScatterColour, scatter), amount);
}
)";

void setSceneLighting(Shader& shader, const SceneLighting& lighting)
{
    shader.set("uSunDirection", lighting.sunDirection);
    shader.set("uSunColour", lighting.sunColour);
    shader.set("uAmbientTop", lighting.ambientTop);
    shader.set("uAmbientBottom", lighting.ambientBottom);
    shader.set("uCarAmbientTop", lighting.carAmbientTop);
    shader.set("uCarAmbientBottom", lighting.carAmbientBottom);
    shader.set("uSkyTop", lighting.skyTop);
    shader.set("uSkyBottom", lighting.skyBottom);
    shader.set("uSkyColourPower", lighting.skyColourPower);
    shader.set("uSkyClampTop", lighting.skyClampTop);
    shader.set("uSkyClampBottom", lighting.skyClampBottom);
    shader.set("uSkyGain", lighting.skyGain);
    shader.set("uHazeColour", lighting.hazeColour);
    shader.set("uHazeDirection", lighting.hazeDirection);
    shader.set("uHazeIntensity", lighting.hazeIntensity);
    shader.set("uHazePower", lighting.hazePower);
    shader.set("uHazeEllipticalScale", lighting.hazeEllipticalScale);
    shader.set("uFogColour", lighting.fogColour);
    shader.set("uFogScatterColour", lighting.fogScatterColour);
    shader.set("uFogScatterPower", lighting.fogScatterPower);
    shader.set("uFogDensity", lighting.fogDensity);
    shader.set("uFogStart", lighting.fogStart);
    shader.set("uFogHeightFadeAmount", lighting.fogHeightFadeAmount);
    shader.set("uFogHeightFadeStart", lighting.fogHeightFadeStart);
    shader.set("uFogHeightFadeEnd", lighting.fogHeightFadeEnd);
    shader.set("uFogScatterStart", lighting.fogScatterStart);
    shader.set("uFogScatterRamp", lighting.fogScatterRamp);
    shader.set("uFogSkyHeightFadeStart", lighting.fogSkyHeightFadeStart);
    shader.set("uFogSkyHeightFadeEnd", lighting.fogSkyHeightFadeEnd);
}

WorldRenderer::WorldRenderer(const world::TrackWorld& world, JobPool& jobs)
    : m_world(world)
    , m_jobs(jobs)
    , m_grid(world.index, kTileSize, world::EventPropFilter::route(m_eventRoute))
    , m_alive(std::make_shared<std::atomic<bool>>(true))
    , m_programs(world.shaders, bindlessSupported())
    , m_bindless(m_programs.bindless())
    , m_depthPrograms(world.shaders, m_bindless, TrackPrograms::kDepthOnly)
    , m_shadowPrograms(world.shaders, m_bindless, TrackPrograms::kShadowDepthOnly)
    , m_white(Texture::solid(255, 255, 255, 255))
    , m_black(Texture::solid(0, 0, 0, 255))
    , m_blackCube(blackCubeMap())
    , m_black3D(blackVolume())
{
    m_tiles.resize(m_grid.tiles().size());
    applyFilters();
    GLint uniformAlignment = 0;
    GLint storageAlignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniformAlignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlignment);
    if (static_cast<std::size_t>(uniformAlignment) > kMaterialBlockBytes
        || static_cast<std::size_t>(storageAlignment) > world::ShadedMesh::kVertexAlignment) {
        throw std::runtime_error(std::format("buffer offset alignment {} / {} exceeds the 256 bytes the world "
                                             "renderer packs its blocks to",
            uniformAlignment, storageAlignment));
    }
    // Translate every track effect up front, ahead of the tiles; the GL
    // thread links them in slices (precompile()) or on first use.
    m_programs.prepareAll(m_jobs, kTranslatePriority);
    m_depthPrograms.prepareAll(m_jobs, kTranslatePriority);
    m_shadowPrograms.prepareAll(m_jobs, kTranslatePriority);
    glCreateBuffers(1, &m_depthFixed);
    glNamedBufferStorage(m_depthFixed, kFixedBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    {
        // The depth passes read no pixel position and test no alpha.
        const auto controlBytes = xenos::ControlConstants{}.pack();
        const auto systemBytes = xenos::SystemConstants{}.pack();
        glNamedBufferSubData(
            m_depthFixed, kControlOffset, static_cast<GLsizeiptr>(controlBytes.size()), controlBytes.data());
        glNamedBufferSubData(
            m_depthFixed, kSystemOffset, static_cast<GLsizeiptr>(systemBytes.size()), systemBytes.data());
    }
    glCreateBuffers(1, &m_fixedBlocks);
    glNamedBufferStorage(m_fixedBlocks, kFixedBytes, nullptr, GL_DYNAMIC_STORAGE_BIT);
    if (m_bindless) {
        m_handles.resize(texturehandle::kFirstTrackTexture);
        m_handles[texturehandle::kBlack] = glGetTextureHandleARB(m_black.id());
        m_handles[texturehandle::kWhite] = glGetTextureHandleARB(m_white.id());
        m_handles[texturehandle::kBlackCube] = glGetTextureHandleARB(m_blackCube);
        m_handles[texturehandle::kBlack3D] = glGetTextureHandleARB(m_black3D);
        for (std::uint32_t entry = 0; entry < texturehandle::kShadowMask; ++entry) {
            glMakeTextureHandleResidentARB(m_handles[entry]);
        }
        m_handles[texturehandle::kShadowMask] = m_handles[texturehandle::kWhite];
        m_dirtyEnd = m_handles.size();
    }
    logInfo("world: {} tiles of {:.0f} m, {} track effects, {}", m_tiles.size(), kTileSize, world.shaders.size(),
        m_bindless ? "bindless textures" : "bound textures");
}

WorldRenderer::~WorldRenderer()
{
    // Queued jobs capture this renderer and write their results into it;
    // the flag makes any that still start return at once, and the wait
    // covers those already running.
    m_alive->store(false);
    m_jobs.clearAndWait();
    for (TileState& tile : m_tiles) {
        tile.mesh.reset();
    }
    m_dynamics.clear();
    if (m_bindless) {
        for (const auto& [id, slot] : m_textures) {
            if (slot.handle != 0) {
                glMakeTextureHandleNonResidentARB(slot.handle);
            }
        }
        for (const auto& [id, slot] : m_cubes) {
            if (slot.handle != 0) {
                glMakeTextureHandleNonResidentARB(slot.handle);
            }
        }
        for (std::uint32_t entry = 0; entry < texturehandle::kShadowMask; ++entry) {
            glMakeTextureHandleNonResidentARB(m_handles[entry]);
        }
        if (m_shadowMaskHandle != 0) {
            glMakeTextureHandleNonResidentARB(m_shadowMaskHandle);
        }
    }
    glDeleteBuffers(1, &m_handleBuffer);
    glDeleteTextures(1, &m_black3D);
    glDeleteTextures(1, &m_blackCube);
    glDeleteBuffers(static_cast<GLsizei>(m_frameBuffers.size()), m_frameBuffers.data());
    glDeleteBuffers(1, &m_fixedBlocks);
    glDeleteBuffers(1, &m_depthFixed);
    glDeleteBuffers(1, &m_depthFrames);
}

void WorldRenderer::destroy(GpuMesh& mesh)
{
    glDeleteVertexArrays(1, &mesh.vao);
    glDeleteBuffers(1, &mesh.vertices);
    glDeleteBuffers(1, &mesh.indices);
    glDeleteBuffers(1, &mesh.materials);
    mesh.vao = mesh.vertices = mesh.indices = mesh.materials = 0;
}

std::vector<std::uint32_t> WorldRenderer::TileBuild::textureIds() const
{
    std::vector<std::uint32_t> ids = shaded.textureIds();
    for (const std::uint32_t id : scattered.textures()) {
        if (id != world::TileMesh::kNoTexture && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::unique_ptr<WorldRenderer::TileBuild> WorldRenderer::build(const std::vector<std::uint32_t>& chunks)
{
    // Procedural copies go to ScatterPass (see there for why).
    std::vector<std::uint32_t> placed;
    std::vector<std::uint32_t> scattered;
    for (const std::uint32_t c : chunks) {
        (c < m_world.index.chunks().size() && m_world.index.chunks()[c].scattered ? scattered : placed).push_back(c);
    }
    const world::TrackTextures* textures = m_world.textures ? &*m_world.textures : nullptr;
    auto result = std::make_unique<TileBuild>();
    result->shaded = world::buildShadedMesh(*m_world.archive, m_world.index, placed, m_world.shaders, textures);
    if (!scattered.empty()) {
        result->scattered = world::buildTileMesh(*m_world.archive, m_world.index, scattered, textures);
    }
    // Translation is CPU work; doing it here keeps it off the GL thread,
    // which then only compiles.
    for (const world::ShadedMesh::Batch& batch : result->shaded.batches) {
        m_programs.prepare(batch.shader);
    }
    return result;
}

std::shared_ptr<WorldRenderer::GpuMesh> WorldRenderer::upload(const TileBuild& build)
{
    const world::ShadedMesh& mesh = build.shaded;
    auto gpu = std::shared_ptr<GpuMesh>(new GpuMesh, [](GpuMesh* m) {
        destroy(*m);
        delete m;
    });
    glCreateBuffers(1, &gpu->vertices);
    glNamedBufferStorage(gpu->vertices, static_cast<GLsizeiptr>(mesh.vertices.size()), mesh.vertices.data(), 0);
    glCreateBuffers(1, &gpu->indices);
    glNamedBufferStorage(
        gpu->indices, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)), mesh.indices.data(), 0);
    glCreateVertexArrays(1, &gpu->vao);
    glVertexArrayElementBuffer(gpu->vao, gpu->indices);

    gpu->batches = mesh.batches;
    gpu->programs.reserve(mesh.batches.size());
    for (const world::ShadedMesh::Batch& batch : mesh.batches) {
        gpu->programs.push_back(m_programs.program(batch.shader));
    }

    static_assert(TrackMaterial::kConstants == world::ShadedMesh::kMaterialConstants);
    const std::size_t stride = m_bindless ? sizeof(TrackMaterial) : kMaterialBlockBytes;
    std::vector<std::uint8_t> materials(std::max<std::size_t>(mesh.batches.size(), 1) * stride, 0);
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        const world::ShadedMesh::Batch& batch = mesh.batches[b];
        // The registers the program samples as cubes.
        std::array<bool, world::ShadedMesh::kSamplers> cube{};
        if (const TrackPrograms::Program* program = gpu->programs[b]) {
            for (const TrackPrograms::Sampler& sampler : program->samplers) {
                if (sampler.cube && sampler.reg >= 0 && static_cast<std::size_t>(sampler.reg) < cube.size()) {
                    cube[static_cast<std::size_t>(sampler.reg)] = true;
                }
            }
        }
        TrackMaterial material;
        std::copy_n(batch.constants.begin(), std::min(batch.constants.size(), TrackMaterial::kConstants),
            material.constants.begin());
        material.base[0] = static_cast<std::uint32_t>(batch.vertexOffset / sizeof(std::uint32_t));
        for (std::size_t r = 0; r < batch.textures.size() && r < material.textures.size(); ++r) {
            const std::uint32_t id = batch.textures[r];
            const bool engine = id == world::ShadedMesh::kNoTexture || id == world::ShadedMesh::kEngineTexture;
            if (!engine && r < cube.size() && cube[r]) {
                requestCube(id);
                material.textures[r] = m_bindless ? cubeEntry(id) : TrackMaterial::kEngineTextureEntry;
                continue;
            }
            material.textures[r] = engine || !m_bindless ? TrackMaterial::kEngineTextureEntry : textureEntry(id);
            if (!engine && std::find(gpu->textureIds.begin(), gpu->textureIds.end(), id) == gpu->textureIds.end()) {
                gpu->textureIds.push_back(id);
            }
        }
        // Without bindless textures the block ends after the base word.
        std::memcpy(materials.data() + b * stride, &material, std::min(stride, sizeof material));
    }
    glCreateBuffers(1, &gpu->materials);
    glNamedBufferStorage(gpu->materials, static_cast<GLsizeiptr>(materials.size()), materials.data(), 0);

    gpu->triangles = mesh.triangleCount() + build.scattered.triangleCount();
    if (!build.scattered.indices.empty()) {
        gpu->scattered = ScatterPass::upload(build.scattered);
    }
    return gpu;
}

void WorldRenderer::requestCube(std::uint32_t id)
{
    if (m_cubes.contains(id)) {
        return;
    }
    m_cubes.emplace(id, CubeSlot{});
    m_jobs.submit(0.0F, [this, id, alive = m_alive] {
        if (!alive->load()) {
            return;
        }
        try {
            auto cube = std::make_unique<world::CubeTexture>(world::loadTrackCube(*m_world.archive, id));
            const std::lock_guard lock(m_resultMutex);
            m_loadedCubes.push_back({id, std::move(cube)});
        } catch (const std::exception& e) {
            // Stays black, like a 2D texture missing from the disc.
            logDebug("world: cube texture {:08X}: {}", id, e.what());
        }
    });
}

std::uint32_t WorldRenderer::cubeEntry(std::uint32_t id)
{
    const auto [it, added] = m_cubeEntries.try_emplace(id, static_cast<std::uint32_t>(m_handles.size()));
    if (added) {
        m_handles.push_back(m_handles[texturehandle::kBlackCube]);
        m_dirtyFirst = std::min(m_dirtyFirst, m_handles.size() - 1);
        m_dirtyEnd = m_handles.size();
    }
    return it->second;
}

std::size_t WorldRenderer::cubeMapsLoaded() const
{
    return static_cast<std::size_t>(
        std::count_if(m_cubes.begin(), m_cubes.end(), [](const auto& entry) { return entry.second.ready; }));
}

GLuint WorldRenderer::cubeTexture(std::uint32_t id) const
{
    const auto it = m_cubes.find(id);
    return it != m_cubes.end() && it->second.ready ? it->second.map.id() : m_blackCube;
}

void WorldRenderer::requestTexture(std::uint32_t id, float priority)
{
    if (!m_world.textures) {
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
            auto chain = std::make_unique<world::TextureMipChain>(m_world.textures->loadTexture(*m_world.archive, id));
            const std::lock_guard lock(m_resultMutex);
            m_loadedTextures.push_back({id, std::move(chain)});
        } catch (const std::exception& e) {
            logDebug("world: texture {:08X}: {}", id, e.what());
            const std::lock_guard lock(m_resultMutex);
            m_failedTextures.push_back(id);
        }
    });
}

bool WorldRenderer::texturesReady(const TileBuild& build) const
{
    for (const std::uint32_t id : build.textureIds()) {
        const auto it = m_textures.find(id);
        if (it == m_textures.end() || (!it->second.ready && !it->second.failed)) {
            return false;
        }
    }
    return true;
}

const Texture* WorldRenderer::texture(std::uint32_t id)
{
    const auto it = m_textures.find(id);
    if (it == m_textures.end() || !it->second.ready) {
        return nullptr;
    }
    it->second.lastUsed = m_frame;
    return &it->second.texture;
}

void WorldRenderer::touchTextures(const GpuMesh& mesh)
{
    for (const std::uint32_t id : mesh.textureIds) {
        const auto it = m_textures.find(id);
        if (it != m_textures.end()) {
            it->second.lastUsed = m_frame;
        }
    }
}

std::uint32_t WorldRenderer::textureEntry(std::uint32_t id)
{
    const auto [it, added] = m_textureEntries.try_emplace(id, static_cast<std::uint32_t>(m_handles.size()));
    if (added) {
        // Black until the texture is resident, and for good if the disc
        // lacks it (see the class comment).
        m_handles.push_back(m_handles[texturehandle::kBlack]);
        m_dirtyFirst = std::min(m_dirtyFirst, m_handles.size() - 1);
        m_dirtyEnd = m_handles.size();
    }
    return it->second;
}

void WorldRenderer::setHandle(std::uint32_t entry, GLuint64 handle)
{
    m_handles[entry] = handle;
    m_dirtyFirst = std::min<std::size_t>(m_dirtyFirst, entry);
    m_dirtyEnd = std::max<std::size_t>(m_dirtyEnd, entry + 1);
}

void WorldRenderer::flushHandles()
{
    if (m_handles.size() > m_handleCapacity) {
        m_handleCapacity = std::max(kInitialHandles, m_handleCapacity);
        while (m_handleCapacity < m_handles.size()) {
            m_handleCapacity *= 2;
        }
        glDeleteBuffers(1, &m_handleBuffer);
        glCreateBuffers(1, &m_handleBuffer);
        glNamedBufferStorage(m_handleBuffer, static_cast<GLsizeiptr>(m_handleCapacity * sizeof(GLuint64)), nullptr,
            GL_DYNAMIC_STORAGE_BIT);
        m_dirtyFirst = 0;
        m_dirtyEnd = m_handles.size();
    }
    if (m_dirtyFirst < m_dirtyEnd) {
        glNamedBufferSubData(m_handleBuffer, static_cast<GLintptr>(m_dirtyFirst * sizeof(GLuint64)),
            static_cast<GLsizeiptr>((m_dirtyEnd - m_dirtyFirst) * sizeof(GLuint64)), m_handles.data() + m_dirtyFirst);
    }
    m_dirtyFirst = m_handles.size();
    m_dirtyEnd = 0;
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
        TextureSlot& slot = m_textures[id];
        total -= slot.texture.byteSize();
        if (slot.handle != 0) {
            glMakeTextureHandleNonResidentARB(slot.handle);
            setHandle(textureEntry(id), m_handles[texturehandle::kBlack]);
        }
        m_textures.erase(id);
    }
    m_stats.textureBytes = total;
}

void WorldRenderer::setHiddenChunks(const std::vector<bool>& hidden)
{
    m_hiddenChunks = hidden;
    applyFilters();
}

void WorldRenderer::setEventRoute(int routeId)
{
    if (routeId == m_eventRoute) {
        return;
    }
    m_eventRoute = routeId;
    applyFilters();
}

void WorldRenderer::applyFilters()
{
    for (std::size_t tile : m_grid.update(world::EventPropFilter::route(m_eventRoute), m_hiddenChunks)) {
        // Rebuilt at the current distance on the next update; the old mesh
        // stays on screen meanwhile.
        m_tiles[tile].requestedState = -1;
        m_tiles[tile].pending.reset();
        m_tiles[tile].pendingState = -1;
    }
    const auto& chunks = m_world.index.chunks();
    m_eventChunksIncluded = 0;
    for (const world::WorldTileGrid::Tile& tile : m_grid.tiles()) {
        for (const std::uint32_t c : tile.chunks) {
            m_eventChunksIncluded += chunks[c].placed && chunks[c].placement.eventProp ? 1 : 0;
        }
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
        std::unique_ptr<TileBuild> mesh = build(chunks);
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
    std::deque<BuiltDynamic> dynamics;
    std::vector<std::uint32_t> failed;
    std::deque<LoadedCube> cubes;
    {
        const std::lock_guard lock(m_resultMutex);
        built.swap(m_builtTiles);
        cubes.swap(m_loadedCubes);
        const std::size_t take = std::min<std::size_t>(m_loadedTextures.size(), kMaxTextureUploadsPerFrame);
        for (std::size_t i = 0; i < take; ++i) {
            loaded.push_back(std::move(m_loadedTextures.front()));
            m_loadedTextures.pop_front();
        }
        failed.swap(m_failedTextures);
        dynamics.swap(m_builtDynamics);
    }
    for (std::uint32_t id : failed) {
        m_textures[id].failed = true;
    }
    for (LoadedCube& loadedCube : cubes) {
        CubeSlot& slot = m_cubes[loadedCube.id];
        // Colour, as the track's other colour textures.
        slot.map = CubeMap::fromCube(*loadedCube.cube, true);
        slot.ready = slot.map.id() != 0;
        if (m_bindless && slot.ready) {
            slot.handle = glGetTextureHandleARB(slot.map.id());
            glMakeTextureHandleResidentARB(slot.handle);
            setHandle(cubeEntry(loadedCube.id), slot.handle);
        }
    }
    for (LoadedTexture& texture : loaded) {
        TextureSlot& slot = m_textures[texture.id];
        // Normal maps (BC5) and masks are linear; colour maps are sRGB.
        const bool colour = texture.chain->format != world::TextureSurface::Format::Bc5;
        slot.texture = Texture::fromMipChain(*texture.chain, colour);
        slot.ready = true;
        slot.requested = true;
        if (m_bindless) {
            slot.handle = glGetTextureHandleARB(slot.texture.id());
            glMakeTextureHandleResidentARB(slot.handle);
            setHandle(textureEntry(texture.id), slot.handle);
        }
    }
    for (BuiltDynamic& dynamic : dynamics) {
        const auto it = m_dynamics.find(dynamic.handle);
        if (it == m_dynamics.end() || dynamic.mesh->empty()) {
            continue;
        }
        for (const std::uint32_t id : dynamic.mesh->textureIds()) {
            requestTexture(id, 0.0F);
        }
        it->second.mesh = upload(*dynamic.mesh);
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
        for (const std::uint32_t id : tile.pending->textureIds()) {
            requestTexture(id, distance);
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
        tile.mesh = tile.pending->empty() ? nullptr : upload(*tile.pending);
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
            std::unique_ptr<TileBuild> mesh = build(chunks);
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

bool WorldRenderer::texturesLoaded(const world::ShadedMesh::Batch& batch) const
{
    for (const std::uint32_t id : batch.textures) {
        if (id == world::ShadedMesh::kNoTexture || id == world::ShadedMesh::kEngineTexture) {
            continue;
        }
        const auto it = m_textures.find(id);
        if (it == m_textures.end() || (!it->second.ready && !it->second.failed)) {
            return false;
        }
    }
    return true;
}

void WorldRenderer::collect(const GpuMesh& mesh, std::uint32_t frameSlot)
{
    for (std::uint32_t b = 0; b < mesh.batches.size(); ++b) {
        const world::ShadedMesh::Batch& batch = mesh.batches[b];
        const TrackPrograms::Program* program = mesh.programs[b];
        if (program == nullptr || batch.indexCount == 0 || !texturesLoaded(batch)) {
            continue;
        }
        if (batch.backdrop && batch.chunk != world::ShadedMesh::kMergedChunks
            && !m_world.index.chunks()[batch.chunk].visibleFrom(m_cameraZone)) {
            continue;
        }
        (program->state.blend ? m_blended : m_opaque).push_back({program, &mesh, b, frameSlot});
    }
}

void WorldRenderer::bindSamplers(const TrackPrograms::Program& program, const world::ShadedMesh::Batch& batch)
{
    for (const TrackPrograms::Sampler& sampler : program.samplers) {
        const std::uint32_t id = sampler.reg >= 0 && static_cast<std::size_t>(sampler.reg) < batch.textures.size()
            ? batch.textures[static_cast<std::size_t>(sampler.reg)]
            : world::ShadedMesh::kNoTexture;
        if (sampler.cube) {
            bindTexture(sampler.unit, cubeTexture(id));
            continue;
        }
        const Texture* own
            = id != world::ShadedMesh::kNoTexture && id != world::ShadedMesh::kEngineTexture ? texture(id) : nullptr;
        if (own != nullptr) {
            bindTexture(sampler.unit, own->id());
            continue;
        }
        if (sampler.shadowMask) {
            bindTexture(sampler.unit, m_shadowMask != 0 ? m_shadowMask : m_white.id());
        } else {
            bindTexture(sampler.unit, m_black.id());
        }
    }
}

void WorldRenderer::bindTexture(GLuint unit, GLuint texture)
{
    if (unit < m_boundTextures.size()) {
        if (m_boundTextures[unit] == texture) {
            return;
        }
        m_boundTextures[unit] = texture;
    }
    glBindTextureUnit(unit, texture);
}

void WorldRenderer::drawItems(const std::vector<DrawItem>& items)
{
    const TrackPrograms::Program* program = nullptr;
    const GpuMesh* mesh = nullptr;
    std::uint32_t frameSlot = 0xFFFFFFFF;
    for (const DrawItem& item : items) {
        const world::ShadedMesh::Batch& batch = item.mesh->batches[item.batch];
        if (item.program != program) {
            program = item.program;
            program->shader.use();
            applyState(program->state);
            glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kVertexConstants, program->defaults, 0,
                static_cast<GLsizeiptr>(kProgramBlockBytes));
            glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kPixelConstants, program->defaults,
                static_cast<GLintptr>(kProgramBlockBytes), static_cast<GLsizeiptr>(kProgramBlockBytes));
        }
        if (item.mesh != mesh) {
            mesh = item.mesh;
            glBindVertexArray(mesh->vao);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, mesh->vertices);
            if (m_bindless) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, trackbinding::kMaterial, mesh->materials);
            }
        }
        if (item.frameSlot != frameSlot) {
            frameSlot = item.frameSlot;
            glBindBufferRange(GL_UNIFORM_BUFFER, trackbinding::kFrame, m_frameBuffer,
                static_cast<GLintptr>(frameSlot * kFrameBlockBytes), static_cast<GLsizeiptr>(kFrameBlockBytes));
        }
        const auto* indices
            = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t));
        if (m_bindless) {
            // The base instance tells the programs which material to read.
            glDrawElementsInstancedBaseInstance(
                GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, indices, 1, item.batch);
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, trackbinding::kMaterial, mesh->materials,
                static_cast<GLintptr>(item.batch * kMaterialBlockBytes), static_cast<GLsizeiptr>(kMaterialBlockBytes));
            bindSamplers(*program, batch);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, indices);
        }
        ++m_stats.batches;
        m_stats.triangles += batch.indexCount / 3;
    }
}

void WorldRenderer::draw(const FrameParams& frame)
{
    m_stats.tilesDrawn = 0;
    m_stats.triangles = 0;
    m_stats.batches = 0;
    m_opaque.clear();
    m_blended.clear();
    m_scatterDraws.clear();
    // Other passes bind textures between frames.
    m_boundTextures.fill(0);

    const glm::mat4 viewProjection = frame.projection * frame.view;
    const glm::mat4 inverseView = glm::inverse(frame.view);
    const auto& tiles = m_grid.tiles();
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
                    * glm::vec4(
                        (corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1.0F);
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
        ++m_stats.tilesDrawn;
        touchTextures(*state.mesh);
        collect(*state.mesh, 0);
        if (state.mesh->scattered) {
            m_scatterDraws.emplace_back(state.mesh.get(), glm::mat4(1.0F));
        }
    }

    // Frame constants: the static world's, then one block per moving object.
    std::vector<TrackFrameConstants> blocks;
    const world::TrackSettings* settings = m_world.settings ? &*m_world.settings : nullptr;
    blocks.push_back(TrackFrameConstants::make(
        viewProjection, inverseView, frame.cameraPosition, frame.lighting, frame.time, glm::mat4(1.0F), settings));
    m_timeGain = blocks.front().c[249];
    m_emissiveSwitchOn = blocks.front().c[213];
    for (const auto& [handle, dynamic] : m_dynamics) {
        if (!dynamic.mesh) {
            continue;
        }
        const auto slot = static_cast<std::uint32_t>(blocks.size());
        blocks.push_back(TrackFrameConstants::make(viewProjection, inverseView, frame.cameraPosition, frame.lighting,
            frame.time, dynamic.transform, settings));
        touchTextures(*dynamic.mesh);
        collect(*dynamic.mesh, slot);
        if (dynamic.mesh->scattered) {
            m_scatterDraws.emplace_back(dynamic.mesh.get(), dynamic.transform);
        }
    }
    const std::size_t ring = m_frame % kFrameBuffers;
    if (blocks.size() > m_frameSlots[ring]) {
        glDeleteBuffers(1, &m_frameBuffers[ring]);
        m_frameSlots[ring] = std::max<std::size_t>(blocks.size(), m_frameSlots[ring] * 2);
        glCreateBuffers(1, &m_frameBuffers[ring]);
        glNamedBufferStorage(m_frameBuffers[ring], static_cast<GLsizeiptr>(m_frameSlots[ring] * kFrameBlockBytes),
            nullptr, GL_DYNAMIC_STORAGE_BIT);
    }
    m_frameBuffer = m_frameBuffers[ring];
    glNamedBufferSubData(m_frameBuffer, 0, static_cast<GLsizeiptr>(blocks.size() * kFrameBlockBytes), blocks.data());

    // The translator's fixed blocks: no boolean or loop constants set (the
    // headlight pass, pixel b100, stays off), identity clip space (the
    // camera's projection already gives reversed-Z clip coordinates), pixel
    // rows counted from the top as Direct3D does.
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[3] != m_fixedHeight) {
        m_fixedHeight = viewport[3];
        xenos::SystemConstants system;
        system.screen = {1.0F, static_cast<float>(viewport[3]), 0.0F, 0.0F};
        // Alpha to mask on a single-sample target: with the passes' offsets
        // (0xAA, two quarters per sample) a pixel is covered when its alpha
        // reaches one half. Inferred from how the Xbox 360 dithers alpha to
        // coverage; the foliage effects set it instead of an alpha test.
        xenos::SystemConstants alphaToMask = system;
        alphaToMask.alphaTest = {kAlphaGreaterEqual, 0.5F, 0.0F, 0.0F};
        const auto controlBytes = xenos::ControlConstants{}.pack();
        const auto systemBytes = system.pack();
        const auto alphaToMaskBytes = alphaToMask.pack();
        glNamedBufferSubData(
            m_fixedBlocks, kControlOffset, static_cast<GLsizeiptr>(controlBytes.size()), controlBytes.data());
        glNamedBufferSubData(
            m_fixedBlocks, kSystemOffset, static_cast<GLsizeiptr>(systemBytes.size()), systemBytes.data());
        glNamedBufferSubData(m_fixedBlocks, kSystemAlphaToMaskOffset, static_cast<GLsizeiptr>(alphaToMaskBytes.size()),
            alphaToMaskBytes.data());
    }
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kControlConstants, m_fixedBlocks, kControlOffset,
        static_cast<GLsizeiptr>(xenos::ControlConstants::kBytes));

    if (m_bindless) {
        flushHandles();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, trackbinding::kTextureHandles, m_handleBuffer);
    }

    // Grouped by program, then by tile, to keep state changes down; with
    // bound textures, batches sharing textures go together.
    const auto order = [bindless = m_bindless](const DrawItem& a, const DrawItem& b) {
        if (bindless) {
            return std::tie(a.program, a.frameSlot, a.mesh, a.batch)
                < std::tie(b.program, b.frameSlot, b.mesh, b.batch);
        }
        const auto& ta = a.mesh->batches[a.batch].textures;
        const auto& tb = b.mesh->batches[b.batch].textures;
        return std::tie(a.program, a.frameSlot, ta, a.mesh, a.batch)
            < std::tie(b.program, b.frameSlot, tb, b.mesh, b.batch);
    };
    std::sort(m_opaque.begin(), m_opaque.end(), order);
    std::sort(m_blended.begin(), m_blended.end(), order);
    drawItems(m_opaque);
    if (!m_scatterDraws.empty()) {
        applyState(world::TrackShaders::RenderState{});
        m_scatter.begin(frame);
        const auto loaded = [this](std::uint32_t id) { return texture(id); };
        for (const auto& [mesh, model] : m_scatterDraws) {
            m_scatter.setModel(model);
            m_stats.triangles += m_scatter.draw(*mesh->scattered, loaded, m_world.index, m_cameraZone);
            ++m_stats.batches;
        }
        // The pass binds its own textures and vertex array.
        m_boundTextures.fill(0);
    }
    // Blending passes (decals, road blends, lakes) after everything opaque.
    drawItems(m_blended);

    // Back to the state the other scene passes expect.
    applyState(world::TrackShaders::RenderState{});
    glBindVertexArray(0);
    glUseProgram(0);
}

void WorldRenderer::applyState(const world::TrackShaders::RenderState& state)
{
    if (state.cull == kCullNone) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
    }
    glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
    if (state.blend) {
        glEnable(GL_BLEND);
        glBlendFunc(blendFactor(state.sourceBlend), blendFactor(state.destinationBlend));
    } else {
        glDisable(GL_BLEND);
    }
    // The slope-scaled part of the pass's depth bias. Reversed depth grows
    // towards the camera, so the offset keeps its sign. The constant part
    // is a distance in the game's own depth range, which this projection
    // does not share, and is left out.
    if (state.slopeScaleDepthBias != 0.0F) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(state.slopeScaleDepthBias, 0.0F);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    const std::size_t system = state.alphaToMask ? kSystemAlphaToMaskOffset : kSystemOffset;
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kSystemConstants, m_fixedBlocks, static_cast<GLintptr>(system),
        static_cast<GLsizeiptr>(xenos::SystemConstants::kBytes));
}

bool WorldRenderer::precompile(std::chrono::microseconds budget)
{
    // A third of the slice each, so the loading screen keeps its pace.
    const std::chrono::microseconds share = budget / 3;
    const bool scene = m_programs.compileSome(share);
    const bool depth = m_depthPrograms.compileSome(share);
    const bool shadow = m_shadowPrograms.compileSome(share);
    return scene || depth || shadow;
}

void WorldRenderer::setShadowMask(GLuint texture)
{
    if (texture == m_shadowMask) {
        return;
    }
    if (m_bindless && m_shadowMaskHandle != 0) {
        glMakeTextureHandleNonResidentARB(m_shadowMaskHandle);
        m_shadowMaskHandle = 0;
    }
    m_shadowMask = texture;
    if (m_bindless) {
        if (texture != 0) {
            m_shadowMaskHandle = glGetTextureHandleARB(texture);
            glMakeTextureHandleResidentARB(m_shadowMaskHandle);
        }
        setHandle(texturehandle::kShadowMask,
            m_shadowMaskHandle != 0 ? m_shadowMaskHandle : m_handles[texturehandle::kWhite]);
    }
}

void WorldRenderer::drawDepthOnly(const glm::mat4& viewProjection, const glm::vec3& camera, const glm::mat4& reach)
{
    drawDepthPass(m_depthPrograms, viewProjection, camera, false, &reach);
}

void WorldRenderer::drawShadowCasters(const glm::mat4& lightViewProjection, const glm::vec3& camera)
{
    drawDepthPass(m_shadowPrograms, lightViewProjection, camera, true, nullptr);
}

void WorldRenderer::drawDepthPass(TrackPrograms& programs, const glm::mat4& viewProjection, const glm::vec3& camera,
    bool farPlane, const glm::mat4* reach)
{
    const auto insideOf = [](const glm::mat4& box, bool orthographic, const glm::vec3& lo, const glm::vec3& hi) {
        const int planes = orthographic ? 6 : 5;
        for (int plane = 0; plane < planes; ++plane) {
            int beyond = 0;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec4 p = box
                    * glm::vec4(
                        (corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1.0F);
                const float value = plane == 0 ? p.x + p.w
                    : plane == 1               ? p.w - p.x
                    : plane == 2               ? p.y + p.w
                    : plane == 3               ? p.w - p.y
                    : plane == 4               ? (orthographic ? p.z : p.w)
                                               : p.w - p.z;
                beyond += value < 0.0F ? 1 : 0;
            }
            if (beyond == 8) {
                return false;
            }
        }
        return true;
    };
    const auto inside = [&](const glm::vec3& lo, const glm::vec3& hi) {
        return insideOf(viewProjection, farPlane, lo, hi) && (reach == nullptr || insideOf(*reach, true, lo, hi));
    };
    const auto frameFor = [&](const glm::mat4& model) {
        TrackFrameConstants f = TrackFrameConstants::make(
            viewProjection, glm::translate(glm::mat4(1.0F), camera), camera, SceneLighting{}, 0.0F, model);
        f.setHackMatrix(viewProjection, camera);
        return f;
    };
    std::vector<TrackFrameConstants> blocks;
    blocks.push_back(frameFor(glm::mat4(1.0F)));
    std::vector<DrawItem> items;
    const auto gather = [&](const GpuMesh& mesh, std::uint32_t slot) {
        for (std::uint32_t b = 0; b < mesh.batches.size(); ++b) {
            const world::ShadedMesh::Batch& batch = mesh.batches[b];
            if (batch.indexCount == 0 || !texturesLoaded(batch)) {
                continue;
            }
            if (batch.backdrop && batch.chunk != world::ShadedMesh::kMergedChunks
                && !m_world.index.chunks()[batch.chunk].visibleFrom(m_cameraZone)) {
                continue;
            }
            const TrackPrograms::Program* program = programs.program(batch.shader);
            // Blended surfaces (decals, glass) leave the depth alone, as the
            // scene's own pass does.
            const TrackPrograms::Program* scene = mesh.programs[b];
            if (program != nullptr && (scene == nullptr || !scene->state.blend)) {
                items.push_back({program, &mesh, b, slot});
            }
        }
    };
    const auto& tiles = m_grid.tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const TileState& state = m_tiles[i];
        if (state.mesh && inside(tiles[i].boundsMin, tiles[i].boundsMax)) {
            touchTextures(*state.mesh);
            gather(*state.mesh, 0);
        }
    }
    for (const auto& [handle, dynamic] : m_dynamics) {
        if (!dynamic.mesh) {
            continue;
        }
        const auto slot = static_cast<std::uint32_t>(blocks.size());
        blocks.push_back(frameFor(dynamic.transform));
        gather(*dynamic.mesh, slot);
    }
    if (items.empty()) {
        return;
    }

    // One buffer for every depth pass of a frame, each pass's blocks after
    // the last's.
    if (m_depthFrameOf != m_frame) {
        m_depthFrameOf = m_frame;
        m_depthFrameUsed = 0;
    }
    if (m_depthFrameUsed + blocks.size() > m_depthFrameSlots) {
        glDeleteBuffers(1, &m_depthFrames);
        m_depthFrameSlots = std::max<std::size_t>(blocks.size() * 8, m_depthFrameSlots * 2);
        glCreateBuffers(1, &m_depthFrames);
        glNamedBufferStorage(m_depthFrames, static_cast<GLsizeiptr>(m_depthFrameSlots * kFrameBlockBytes), nullptr,
            GL_DYNAMIC_STORAGE_BIT);
        m_depthFrameUsed = 0;
    }
    const std::size_t base = m_depthFrameUsed;
    glNamedBufferSubData(m_depthFrames, static_cast<GLintptr>(base * kFrameBlockBytes),
        static_cast<GLsizeiptr>(blocks.size() * kFrameBlockBytes), blocks.data());
    m_depthFrameUsed += blocks.size();

    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kControlConstants, m_depthFixed, kControlOffset,
        static_cast<GLsizeiptr>(xenos::ControlConstants::kBytes));
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kSystemConstants, m_depthFixed, kSystemOffset,
        static_cast<GLsizeiptr>(xenos::SystemConstants::kBytes));
    if (m_bindless) {
        flushHandles();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, trackbinding::kTextureHandles, m_handleBuffer);
    }
    m_boundTextures.fill(0);
    std::sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) {
        return std::tie(a.program, a.frameSlot, a.mesh, a.batch) < std::tie(b.program, b.frameSlot, b.mesh, b.batch);
    });
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    const TrackPrograms::Program* program = nullptr;
    const GpuMesh* mesh = nullptr;
    std::uint32_t frameSlot = 0xFFFFFFFF;
    for (const DrawItem& item : items) {
        const world::ShadedMesh::Batch& batch = item.mesh->batches[item.batch];
        if (item.program != program) {
            program = item.program;
            program->shader.use();
            // Xbox 360 D3DCULL: 2 culls the faces turned away (counter-
            // clockwise in the pass's clip space), 6 those turned towards it.
            const world::TrackShaders::RenderState& state = program->state;
            if (!state.cullGiven || state.cull == kCullNone) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(state.cull == kCullCounterClockwise ? GL_FRONT : GL_BACK);
            }
            glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kVertexConstants, program->defaults, 0,
                static_cast<GLsizeiptr>(kProgramBlockBytes));
            glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kPixelConstants, program->defaults,
                static_cast<GLintptr>(kProgramBlockBytes), static_cast<GLsizeiptr>(kProgramBlockBytes));
        }
        if (item.mesh != mesh) {
            mesh = item.mesh;
            glBindVertexArray(mesh->vao);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, mesh->vertices);
            if (m_bindless) {
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, trackbinding::kMaterial, mesh->materials);
            }
        }
        if (item.frameSlot != frameSlot) {
            frameSlot = item.frameSlot;
            glBindBufferRange(GL_UNIFORM_BUFFER, trackbinding::kFrame, m_depthFrames,
                static_cast<GLintptr>((base + frameSlot) * kFrameBlockBytes),
                static_cast<GLsizeiptr>(kFrameBlockBytes));
        }
        const auto* indices
            = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t));
        if (m_bindless) {
            glDrawElementsInstancedBaseInstance(
                GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, indices, 1, item.batch);
        } else {
            glBindBufferRange(GL_UNIFORM_BUFFER, trackbinding::kMaterial, mesh->materials,
                static_cast<GLintptr>(item.batch * kMaterialBlockBytes), static_cast<GLsizeiptr>(kMaterialBlockBytes));
            bindSamplers(*program, batch);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT, indices);
        }
    }
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBindVertexArray(0);
    glUseProgram(0);
    m_boundTextures.fill(0);
}

} // namespace fh::render
