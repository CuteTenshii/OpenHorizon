#include "CrowdRenderer.h"

#include "TrackPrograms.h"
#include "WorldRenderer.h"

#include "base/Error.h"
#include "world/TrackSettings.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace fh::render {

namespace {

namespace draw = spectators::draw;

/// Floats of a model vertex on the GPU: position, normal, texture
/// coordinates, the two bone indices and their weights (the colour of
/// spectators::SpectatorMesh is left out: the files have none).
constexpr int kVertexFloats = 12;
/// Floats of a model instance: the placement's three rows and the
/// animation, skin layer and fade (see kModelVertexSource).
constexpr int kInstanceFloats = 16;
/// Floats of a sprite instance (see kSpriteVertexSource).
constexpr int kSpriteFloats = 12;
constexpr GLuint kBonesBinding = 0;
constexpr GLuint kSkinUnit = 0;
constexpr GLuint kAtlasUnit = 0;
constexpr GLuint kMaskUnit = 1;
/// Frames a second the game renders at in play, one clip frame each (see
/// CrowdRenderer::draw()).
constexpr double kTicksPerSecond = 30.0;
constexpr GLenum kCompressedSrgbAlphaDxt1 = 0x8C4D;
constexpr GLenum kCompressedSrgbAlphaDxt5 = 0x8C4F;
/// The sprite quad as two triangles of its corners (see the vertex shader).
constexpr std::array<std::uint32_t, 6> kQuadIndices{0, 1, 2, 0, 2, 3};

struct DrawCommand {
    std::uint32_t count = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t baseVertex = 0;
    std::uint32_t baseInstance = 0;
};

// PROC_CHARACTER_SKIN_VS (0x82184970): two bones blended by the second's
// weight, the draw record's placement, the fade from FadeValues against the
// distance from the camera.
const char* const kModelVertexSource = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexcoord;
layout(location = 3) in vec2 aBones;
layout(location = 4) in vec2 aWeights;
layout(location = 5) in vec4 aRow0;
layout(location = 6) in vec4 aRow1;
layout(location = 7) in vec4 aRow2;
// x: the frame's first bone row (AnimInfo.x), y: the skin's layer, z and
// w: FadeValues.x and .y.
layout(location = 8) in vec4 aAnimation;
layout(std430, binding = 0) readonly buffer CrowdBones
{
    vec4 uBoneRows[];
};
uniform mat4 uViewProjection;
uniform vec3 uCamera;
out vec3 vPosition;
out vec3 vNormal;
out vec2 vTexcoord;
out vec4 vClip;
flat out float vLayer;
out float vFade;

vec3 bone(float index, vec4 v)
{
    int row = (int(aAnimation.x) + int(index)) * 3;
    return vec3(dot(uBoneRows[row], v), dot(uBoneRows[row + 1], v), dot(uBoneRows[row + 2], v));
}

vec3 place(vec4 v)
{
    return vec3(dot(aRow0, v), dot(aRow1, v), dot(aRow2, v));
}

void main()
{
    vec4 p = vec4(aPosition, 1.0);
    vec4 n = vec4(aNormal, 0.0);
    vec3 skinned = mix(bone(aBones.x, p), bone(aBones.y, p), aWeights.y);
    vec3 normal = mix(bone(aBones.x, n), bone(aBones.y, n), aWeights.y);
    vPosition = place(vec4(skinned, 1.0));
    vNormal = place(vec4(normal, 0.0));
    vTexcoord = aTexcoord;
    vLayer = aAnimation.y;
    vFade = clamp((distance(vPosition, uCamera) - aAnimation.z) / (aAnimation.w - aAnimation.z), 0.0, 1.0);
    gl_Position = uViewProjection * vec4(vPosition, 1.0);
    vClip = gl_Position;
}
)";

// PROC_CHARACTER_SKIN_PS (0x82165788) without its headlight term.
const char* const kModelFragmentBody = R"(
in vec3 vPosition;
in vec3 vNormal;
in vec2 vTexcoord;
in vec4 vClip;
flat in float vLayer;
in float vFade;
layout(location = 0) out vec4 fragColour;
layout(binding = 0) uniform sampler2DArray uSkins;
layout(binding = 1) uniform sampler2D uShadowMask;
uniform vec3 uCamera;
uniform vec3 uToSun;
uniform vec3 uCrowdSunColour;
uniform vec3 uCrowdAmbient;

void main()
{
    vec4 albedo = texture(uSkins, vec3(vTexcoord, vLayer));
    float alpha = albedo.a * vFade;
    if (alpha < 0.5) {
        discard;
    }
    vec2 screen = vec2(0.5 * vClip.x / vClip.w + 0.5, 0.5 - 0.5 * vClip.y / vClip.w);
    float mask = texture(uShadowMask, screen).r;
    vec3 n = normalize(vNormal);
    vec3 light = uCrowdAmbient + uCrowdSunColour * clamp(dot(n, uToSun), 0.0, 1.0) * mask;
    vec3 colour = applyFog(albedo.rgb * light, vPosition, uCamera);
    fragColour = vec4(sqrt(max(colour, vec3(0.0))), alpha);
}
)";

// PROC_CHARACTER_BILLBOARD_VS (0x821839D0), one instance per person and
// four vertices a quad.
const char* const kSpriteVertexSource = R"(#version 450 core
// xyz: the feet; w: the size factor, negative for billboard-only people.
layout(location = 0) in vec4 aPlace;
// xy: the facing's x and z; z: the model's first cell; w: 1 for six views.
layout(location = 1) in vec4 aCell;
layout(location = 2) in vec4 aTint;
uniform mat4 uViewProjection;
uniform vec3 uCamera;
uniform vec3 uCamLook;
uniform vec4 uFade;
uniform float uAtlasRows;
uniform vec3 uToSun;
uniform vec3 uCrowdSunColour;
uniform vec3 uCrowdAmbient;
uniform float uEmissive;
out vec3 vPosition;
out vec2 vTexcoord;
out vec4 vClip;
out vec3 vLight;
out float vFade;

const float kCellsAlong = 32.0;
const float kHalfWidth = 0.5;
const float kHeight = 2.0;
const vec2 kUvScale = vec2(0.9, 0.95);
const float kSizeMin = 0.95;
const float kSizeMax = 1.05;
const float kFront = 0.707;
const float kPi = 3.1415927;

float ramp(float value, float from, float to)
{
    return clamp((value - from) / (to - from), 0.0, 1.0);
}

float arcCos(float c)
{
    float a = min(abs(c), 1.0);
    float p = ((-0.0187293 * a + 0.074261) * a - 0.2121144) * a + 1.5707288;
    float angle = sqrt(1.0 - a) * p;
    return c < 0.0 ? kPi - angle : angle;
}

void main()
{
    int corner = gl_VertexID & 3;
    float across = (corner == 0 || corner == 3) ? 1.0 : -1.0;
    float up = corner >= 2 ? 1.0 : 0.0;

    vec3 base = aPlace.xyz - uCamera;
    float d = length(base);
    vec2 toCamera = -normalize(base.xz);
    vec2 facing = aCell.xy;
    float c = dot(facing, toCamera);
    float s = toCamera.y * facing.x - toCamera.x * facing.y;
    float column;
    if (aCell.w > 0.5) {
        column = floor(min(arcCos(c), kPi * 0.5) * 8.0 / kPi + 1.5000114);
    } else {
        column = c > kFront ? 1.0 : 2.0;
    }
    if (c < -kFront) {
        column = 0.0;
    }
    float mirror = (s > 0.0 && column > 1.0) ? -1.0 : 1.0;
    float cell = aCell.z + column;
    float row = floor(cell / kCellsAlong);
    float cellX = cell - row * kCellsAlong;

    float size = mix(kSizeMin, kSizeMax, abs(aPlace.w));
    float fadeIn = ramp(d, uFade.x, uFade.y);
    float fadeOut = ramp(d, uFade.z, uFade.w);
    vFade = aPlace.w < 0.0 ? fadeOut : min(fadeIn, fadeOut);

    vec2 look = normalize(uCamLook.xz);
    vec3 right = vec3(-look.y, 0.0, look.x);
    vec3 offset = right * across * kUvScale.x * kHalfWidth * size + vec3(0.0, up * kUvScale.y * kHeight * size, 0.0);
    vPosition = uCamera + base + (vFade > 0.0 ? offset : vec3(0.0));
    gl_Position = uViewProjection * vec4(vPosition, 1.0);
    vClip = gl_Position;
    vTexcoord = vec2((cellX + 0.5 + 0.5 * mirror * across * kUvScale.x) / kCellsAlong,
        (row + 1.0 - up * kUvScale.y) / uAtlasRows);

    vec3 normal = -normalize(vec3(uCamLook.x, 0.0, uCamLook.z));
    vLight = aTint.rgb * aTint.rgb * uEmissive + uCrowdAmbient
        + clamp(dot(normal, uToSun), 0.0, 1.0) * uCrowdSunColour;
}
)";

// PROC_CHARACTER_BILLBOARD_PS (0x821652B0).
const char* const kSpriteFragmentBody = R"(
in vec3 vPosition;
in vec2 vTexcoord;
in vec4 vClip;
in vec3 vLight;
in float vFade;
layout(location = 0) out vec4 fragColour;
layout(binding = 0) uniform sampler2D uAtlas;
layout(binding = 1) uniform sampler2D uShadowMask;
uniform vec3 uCamera;
uniform vec3 uCrowdAmbient;

void main()
{
    vec4 albedo = texture(uAtlas, vTexcoord);
    float alpha = albedo.a * vFade;
    if (alpha < 0.5) {
        discard;
    }
    vec2 screen = vec2(0.5 * vClip.x / vClip.w + 0.5, 0.5 - 0.5 * vClip.y / vClip.w);
    float mask = texture(uShadowMask, screen).r;
    vec3 light = (vLight - uCrowdAmbient) * mask + uCrowdAmbient;
    vec3 colour = applyFog(albedo.rgb * light, vPosition, uCamera);
    fragColour = vec4(sqrt(max(colour, vec3(0.0))), alpha);
}
)";

std::string fragmentSource(const char* body)
{
    return std::string("#version 450 core\n") + kSceneLightingGlsl + body;
}

/// The planes a point in view satisfies (dot(plane, (p, 1)) >= 0): the
/// four sides and the near plane of the reversed-depth projection, whose far
/// plane is at infinity.
std::array<glm::vec4, 5> viewPlanes(const glm::mat4& viewProjection)
{
    const glm::mat4 t = glm::transpose(viewProjection);
    return {t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[3] - t[2]};
}

bool boxInView(const std::array<glm::vec4, 5>& planes, const glm::vec3& lo, const glm::vec3& hi)
{
    for (const glm::vec4& plane : planes) {
        // The corner furthest along the plane's normal.
        const glm::vec3 corner(
            plane.x >= 0.0F ? hi.x : lo.x, plane.y >= 0.0F ? hi.y : lo.y, plane.z >= 0.0F ? hi.z : lo.z);
        if (glm::dot(glm::vec3(plane), corner) + plane.w < 0.0F) {
            return false;
        }
    }
    return true;
}

float distanceToBox(const glm::vec3& point, const glm::vec3& lo, const glm::vec3& hi)
{
    return glm::length(glm::max(glm::max(lo - point, point - hi), glm::vec3(0.0F)));
}

GLenum skinFormat(world::TextureSurface::Format format, bool& compressed)
{
    compressed = true;
    switch (format) {
    case world::TextureSurface::Format::Dxt1:
        return kCompressedSrgbAlphaDxt1;
    case world::TextureSurface::Format::Dxt3:
    case world::TextureSurface::Format::Dxt5:
        return kCompressedSrgbAlphaDxt5;
    case world::TextureSurface::Format::Bc5:
    case world::TextureSurface::Format::Rgba8:
        break;
    }
    compressed = false;
    return GL_SRGB8_ALPHA8;
}

} // namespace

CrowdRenderer::CrowdRenderer(const spectators::CrowdSet& crowd)
    : m_crowd(crowd)
    , m_people(crowd.spectators().size())
    , m_crt()
    , m_shown(crowd.groups().size(), false)
    , m_white(Texture::solid(255, 255, 255, 255))
    , m_modelShader("crowd model", kModelVertexSource, fragmentSource(kModelFragmentBody))
    , m_spriteShader("crowd sprite", kSpriteVertexSource, fragmentSource(kSpriteFragmentBody))
{
    for (std::size_t i = 0; i < m_people.size(); ++i) {
        const spectators::Spectator& person = crowd.spectators()[i];
        m_people[i].animation = person.animation;
        m_people[i].frame = person.startFrame;
    }
    for (const spectators::CrowdGroup& group : crowd.groups()) {
        m_randomState.push_back(group.randomState);
    }
    uploadMeshes(crowd);
    uploadSkins(crowd);
    uploadClips(crowd);
    uploadSprites(crowd);
    setSession(m_session);
}

CrowdRenderer::~CrowdRenderer()
{
    const std::array buffers{m_meshVertices, m_meshIndices, m_instances, m_modelCommands, m_bones, m_spriteInstances,
        m_spriteIndices, m_spriteCommands};
    glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    const std::array arrays{m_modelVao, m_spriteVao};
    glDeleteVertexArrays(static_cast<GLsizei>(arrays.size()), arrays.data());
    glDeleteTextures(1, &m_skins);
}

void CrowdRenderer::uploadMeshes(const spectators::CrowdSet& crowd)
{
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    for (const spectators::SpectatorModel& source : crowd.models()) {
        const spectators::SpectatorMesh& mesh = source.mesh;
        Model model;
        model.baseVertex = static_cast<std::uint32_t>(vertices.size() / kVertexFloats);
        for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
            const float* f = &mesh.vertices[v * spectators::SpectatorMesh::kFloatsPerVertex];
            vertices.insert(vertices.end(), f, f + spectators::SpectatorMesh::kColour);
            vertices.insert(
                vertices.end(), f + spectators::SpectatorMesh::kBones, f + spectators::SpectatorMesh::kFloatsPerVertex);
        }
        model.lods = static_cast<int>(std::min<std::size_t>(mesh.lods.size(), model.firstIndex.size()));
        for (int lod = 0; lod < model.lods; ++lod) {
            const spectators::SpectatorMesh::Batch& batch = mesh.lods[static_cast<std::size_t>(lod)];
            model.firstIndex[static_cast<std::size_t>(lod)] = static_cast<std::uint32_t>(indices.size());
            model.indexCount[static_cast<std::size_t>(lod)] = batch.indexCount;
            indices.insert(indices.end(), mesh.indices.begin() + batch.firstIndex,
                mesh.indices.begin() + batch.firstIndex + batch.indexCount);
        }
        m_models.push_back(model);
    }
    glCreateBuffers(1, &m_meshVertices);
    glNamedBufferStorage(m_meshVertices, static_cast<GLsizeiptr>(std::max<std::size_t>(vertices.size(), 1) * 4),
        vertices.empty() ? nullptr : vertices.data(), 0);
    glCreateBuffers(1, &m_meshIndices);
    glNamedBufferStorage(m_meshIndices, static_cast<GLsizeiptr>(std::max<std::size_t>(indices.size(), 1) * 4),
        indices.empty() ? nullptr : indices.data(), 0);
    glCreateBuffers(1, &m_instances);
    glNamedBufferStorage(
        m_instances, static_cast<GLsizeiptr>(draw::kModelQueue * kInstanceFloats * 4), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateBuffers(1, &m_modelCommands);
    glNamedBufferStorage(m_modelCommands,
        static_cast<GLsizeiptr>(crowd.models().size() * m_models.front().firstIndex.size() * sizeof(DrawCommand) + 4),
        nullptr, GL_DYNAMIC_STORAGE_BIT);

    glCreateVertexArrays(1, &m_modelVao);
    glVertexArrayVertexBuffer(m_modelVao, 0, m_meshVertices, 0, kVertexFloats * 4);
    glVertexArrayVertexBuffer(m_modelVao, 1, m_instances, 0, kInstanceFloats * 4);
    glVertexArrayBindingDivisor(m_modelVao, 1, 1);
    glVertexArrayElementBuffer(m_modelVao, m_meshIndices);
    const std::array<std::pair<GLint, GLuint>, 5> attributes{{{3, 0}, {3, 3}, {2, 6}, {2, 8}, {2, 10}}};
    for (GLuint i = 0; i < attributes.size(); ++i) {
        glEnableVertexArrayAttrib(m_modelVao, i);
        glVertexArrayAttribFormat(m_modelVao, i, attributes[i].first, GL_FLOAT, GL_FALSE, attributes[i].second * 4);
        glVertexArrayAttribBinding(m_modelVao, i, 0);
    }
    for (GLuint i = 0; i < 4; ++i) {
        const GLuint location = 5 + i;
        glEnableVertexArrayAttrib(m_modelVao, location);
        glVertexArrayAttribFormat(m_modelVao, location, 4, GL_FLOAT, GL_FALSE, i * 16);
        glVertexArrayAttribBinding(m_modelVao, location, 1);
    }
}

void CrowdRenderer::uploadSkins(const spectators::CrowdSet& crowd)
{
    // One array of every model's skin: the skins are all 256x256 DXT1 with
    // full mip chains on the disc, so one draw can reach them all.
    std::vector<std::uint32_t> textures;
    for (std::size_t m = 0; m < crowd.models().size(); ++m) {
        const std::uint32_t texture = crowd.models()[m].texture;
        auto found = std::find(textures.begin(), textures.end(), texture);
        if (found == textures.end()) {
            textures.push_back(texture);
            found = textures.end() - 1;
        }
        m_models[m].layer = static_cast<std::uint32_t>(found - textures.begin());
    }
    if (textures.empty()) {
        throw DataError("the crowd has no models");
    }
    const world::TextureMipChain& first = crowd.textures()[textures.front()].chain;
    for (const std::uint32_t texture : textures) {
        const world::TextureMipChain& chain = crowd.textures()[texture].chain;
        if (chain.width != first.width || chain.height != first.height || chain.format != first.format
            || chain.levels.size() != first.levels.size()) {
            throw DataError("crowd skin {} differs in size or format from {}", crowd.textures()[texture].path,
                crowd.textures()[textures.front()].path);
        }
    }
    bool compressed = true;
    const GLenum format = skinFormat(first.format, compressed);
    const auto levels = static_cast<GLsizei>(first.levels.size());
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_skins);
    glTextureStorage3D(m_skins, levels, format, first.width, first.height, static_cast<GLsizei>(textures.size()));
    for (std::size_t layer = 0; layer < textures.size(); ++layer) {
        const world::TextureMipChain& chain = crowd.textures()[textures[layer]].chain;
        for (GLsizei level = 0; level < levels; ++level) {
            const Bytes& data = chain.levels[static_cast<std::size_t>(level)];
            const int w = chain.levelWidth(level);
            const int h = chain.levelHeight(level);
            if (compressed) {
                glCompressedTextureSubImage3D(m_skins, level, 0, 0, static_cast<GLint>(layer), w, h, 1, format,
                    static_cast<GLsizei>(data.size()), data.data());
            } else {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTextureSubImage3D(
                    m_skins, level, 0, 0, static_cast<GLint>(layer), w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
            }
        }
    }
    // The skinned draw clamps its sampler and filters linearly (0x8246C158).
    glTextureParameteri(m_skins, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(m_skins, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_skins, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_skins, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_skins, GL_TEXTURE_MAX_LEVEL, levels - 1);

    if (crowd.spriteTexture() < 0) {
        throw DataError("the crowd has no sprite atlas");
    }
    const world::TextureMipChain& atlas = crowd.textures()[static_cast<std::size_t>(crowd.spriteTexture())].chain;
    m_atlas = Texture::fromMipChain(atlas, true);
    // NumAlongNumDownWidthHeight.y: the atlas's rows of cells (0x82E9F290).
    m_atlasRows = std::max(crowd.spriteLayout().rows(), 1);
}

void CrowdRenderer::uploadClips(const spectators::CrowdSet& crowd)
{
    // Every frame of every clip as bone rows: row r of bone b of frame f of
    // clip c at (clipBase[c] + f * bones + b) * 3 + r, so a person's
    // AnimInfo.x is the first bone of its frame.
    std::vector<glm::vec4> rows;
    std::vector<glm::mat4> matrices;
    for (std::uint32_t a = 0; a < crowd.animations().size(); ++a) {
        const spectators::AnimationClip& clip = crowd.animations()[a].clip;
        m_clipBase.push_back(static_cast<std::uint32_t>(rows.size() / 3));
        matrices.resize(clip.boneCount());
        for (int f = 0; f < clip.frameCount; ++f) {
            crowd.frameSkinMatrices(a, f, matrices);
            for (const glm::mat4& m : matrices) {
                for (int r = 0; r < 3; ++r) {
                    rows.emplace_back(m[0][r], m[1][r], m[2][r], m[3][r]);
                }
            }
        }
    }
    glCreateBuffers(1, &m_bones);
    glNamedBufferStorage(m_bones, static_cast<GLsizeiptr>(std::max<std::size_t>(rows.size(), 1) * sizeof(glm::vec4)),
        rows.empty() ? nullptr : glm::value_ptr(rows.front()), 0);
}

void CrowdRenderer::uploadSprites(const spectators::CrowdSet& crowd)
{
    // Each set's sprites together, in record order, so a set is one draw
    // of its instances as the game's billboard draw is.
    std::vector<float> instances;
    for (const spectators::CrowdGroup& group : crowd.groups()) {
        m_groupSpriteFirst.push_back(static_cast<std::uint32_t>(instances.size() / kSpriteFloats));
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const spectators::Spectator& person = crowd.spectators()[i];
            if (person.spriteCell < 0 || person.walkPath >= 0) {
                continue;
            }
            const glm::vec3 facing = person.facing();
            const float size = person.billboardOnly ? -person.sizeFactor : person.sizeFactor;
            const std::array<float, kSpriteFloats> values{person.position.x, person.position.y, person.position.z, size,
                facing.x, facing.z, static_cast<float>(person.spriteCell), person.sixSpriteViews ? 1.0F : 0.0F,
                person.tint.r, person.tint.g, person.tint.b, 0.0F};
            instances.insert(instances.end(), values.begin(), values.end());
        }
        m_groupSpriteCount.push_back(
            static_cast<std::uint32_t>(instances.size() / kSpriteFloats) - m_groupSpriteFirst.back());
    }
    glCreateBuffers(1, &m_spriteInstances);
    glNamedBufferStorage(m_spriteInstances, static_cast<GLsizeiptr>(std::max<std::size_t>(instances.size(), 1) * 4),
        instances.empty() ? nullptr : instances.data(), 0);
    glCreateBuffers(1, &m_spriteIndices);
    glNamedBufferStorage(m_spriteIndices, sizeof(kQuadIndices), kQuadIndices.data(), 0);
    glCreateBuffers(1, &m_spriteCommands);
    glNamedBufferStorage(m_spriteCommands,
        static_cast<GLsizeiptr>(std::max<std::size_t>(crowd.groups().size(), 1) * sizeof(DrawCommand)), nullptr,
        GL_DYNAMIC_STORAGE_BIT);

    glCreateVertexArrays(1, &m_spriteVao);
    glVertexArrayVertexBuffer(m_spriteVao, 0, m_spriteInstances, 0, kSpriteFloats * 4);
    glVertexArrayBindingDivisor(m_spriteVao, 0, 1);
    glVertexArrayElementBuffer(m_spriteVao, m_spriteIndices);
    for (GLuint i = 0; i < 3; ++i) {
        glEnableVertexArrayAttrib(m_spriteVao, i);
        glVertexArrayAttribFormat(m_spriteVao, i, 4, GL_FLOAT, GL_FALSE, i * 16);
        glVertexArrayAttribBinding(m_spriteVao, i, 0);
    }
}

void CrowdRenderer::setSession(const spectators::CrowdSession& session)
{
    m_session = session;
    for (std::size_t g = 0; g < m_crowd.groups().size(); ++g) {
        m_shown[g] = session.shows(m_crowd.groups()[g]);
    }
}

void CrowdRenderer::advance(std::uint32_t spectator, int ticks)
{
    Person& person = m_people[spectator];
    const spectators::Spectator& placed = m_crowd.spectators()[spectator];
    if (person.animation < 0 || ticks <= 0 || placed.crowdClass < 0) {
        return;
    }
    const int frames = m_crowd.animations()[static_cast<std::size_t>(person.animation)].clip.frameCount;
    const int next = person.frame + ticks;
    if (next < frames) {
        person.frame = next;
        return;
    }
    // A clip that ends is followed by a cheer when the runtime's rand() is
    // even and the class has cheers, by an idle otherwise, each drawn from
    // the class's table with the set's generator; the frame starts over.
    const spectators::CrowdClass& crowdClass = m_crowd.classes()[static_cast<std::size_t>(placed.crowdClass)];
    draw::CrowdRandom random(m_randomState[placed.group]);
    const bool cheer = (m_crt.next() & 1U) == 0 && !crowdClass.cheerTable.empty();
    const std::vector<std::uint32_t>& table = cheer ? crowdClass.cheerTable : crowdClass.idleTable;
    if (!table.empty()) {
        person.animation = static_cast<int>(table[random.next() % draw::kClipSlots]);
    }
    m_randomState[placed.group] = random.state();
    person.frame = 0;
}

void CrowdRenderer::draw(const FrameParams& frame, const world::TrackSettings* settings)
{
    m_stats = {};
    const glm::mat4 viewProjection = frame.projection * frame.view;
    const std::array<glm::vec4, 5> planes = viewPlanes(viewProjection);
    const glm::vec3 camera = frame.cameraPosition;

    // Whole clip frames at the game's rate on the presentation clock.
    const auto tick = static_cast<long long>(std::floor(static_cast<double>(frame.time) * kTicksPerSecond));
    const int ticks = m_lastTick < 0 ? 0 : static_cast<int>(std::clamp<long long>(tick - m_lastTick, 0, 1000));
    m_lastTick = tick;
    m_stats.ticks = ticks;
    m_ticksApplied += static_cast<std::uint64_t>(ticks);
    const float walked = std::max(frame.time - m_lastTime, 0.0F) * spectators::CrowdSet::kWalkingSpeed;
    m_lastTime = frame.time;

    std::vector<std::uint32_t> visible;
    std::vector<Queued> queue;
    const float modelReach2 = draw::kModelDistance * draw::kModelDistance;
    const float walkerReach2 = draw::kWalkerDistance * draw::kWalkerDistance;
    const auto personInView = [&](const glm::vec3& feet) {
        const glm::vec3 half(draw::kPersonBoxHalfWidth.x, 0.0F, draw::kPersonBoxHalfWidth.y);
        return boxInView(planes, feet - half, feet + half + glm::vec3(0.0F, draw::kPersonBoxHeight, 0.0F));
    };
    for (std::uint32_t g = 0; g < m_crowd.groups().size(); ++g) {
        const spectators::CrowdGroup& group = m_crowd.groups()[g];
        if (!m_shown[g] || group.count == 0) {
            continue;
        }
        // Walking sets' bounds hold their paths only loosely; test the
        // people themselves.
        const bool walking = m_crowd.spectators()[group.first].walkPath >= 0;
        if (!walking
            && !boxInView(planes, group.boundsMin, group.boundsMax + glm::vec3(0.0F, draw::kPersonBoxHeight, 0.0F))) {
            continue;
        }
        ++m_stats.visibleSets;
        m_stats.visibleEventSets += group.hasEventBlock ? 1 : 0;
        visible.push_back(g);
        const float setDistance = distanceToBox(camera, group.boundsMin, group.boundsMax);
        if (!walking && setDistance > draw::kModelDistance) {
            continue;
        }
        for (std::uint32_t i = group.first; i < group.first + group.count && queue.size() < draw::kModelQueue; ++i) {
            const spectators::Spectator& person = m_crowd.spectators()[i];
            if (person.walkPath >= 0 || person.billboardOnly || person.model < 0) {
                continue;
            }
            const glm::vec3 offset = person.position - camera;
            const float d2 = glm::dot(offset, offset);
            if (d2 > modelReach2 || !personInView(person.position)) {
                continue;
            }
            advance(i, ticks);
            queue.push_back({i, d2, person.position, person.heading, false});
        }
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            const spectators::Spectator& person = m_crowd.spectators()[i];
            if (person.walkPath < 0) {
                continue;
            }
            // Walkers move on while their path is within reach (0x82EC0630).
            const spectators::WalkPath& path = m_crowd.walkPaths()[static_cast<std::size_t>(person.walkPath)].path;
            if (distanceToBox(camera, path.boundsMin, path.boundsMax) > draw::kWalkerDistance) {
                continue;
            }
            Person& state = m_people[i];
            state.walked += walked;
            if (queue.size() >= draw::kModelQueue || person.model < 0) {
                continue;
            }
            float heading = person.heading;
            const glm::vec3 position = m_crowd.walkerAt(i, state.walked / spectators::CrowdSet::kWalkingSpeed, heading);
            const glm::vec3 offset = position - camera;
            const float d2 = glm::dot(offset, offset);
            if (d2 > walkerReach2 || !personInView(position)) {
                continue;
            }
            // Walkers loop their clip (0x82ECB3F8).
            if (state.animation >= 0 && ticks > 0) {
                const int frames = m_crowd.animations()[static_cast<std::size_t>(state.animation)].clip.frameCount;
                state.frame = (state.frame + ticks) % std::max(frames, 1);
            }
            queue.push_back({i, d2, position, heading, true});
        }
    }

    std::stable_sort(queue.begin(), queue.end(),
        [](const Queued& a, const Queued& b) { return a.distanceSquared < b.distanceSquared; });
    std::vector<float> distances;
    distances.reserve(queue.size());
    for (const Queued& entry : queue) {
        distances.push_back(std::sqrt(entry.distanceSquared));
    }
    const std::vector<int> lods = draw::modelLods(distances, static_cast<int>(m_models.front().firstIndex.size()));

    // Draw records grouped by model and level of detail.
    std::vector<std::uint32_t> order(queue.size());
    std::iota(order.begin(), order.end(), 0U);
    std::vector<int> lodOf(queue.size());
    for (std::size_t q = 0; q < queue.size(); ++q) {
        const auto model = static_cast<std::size_t>(m_crowd.spectators()[queue[q].spectator].model);
        lodOf[q] = std::min(lods[q], m_models[model].lods - 1);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const int ma = m_crowd.spectators()[queue[a].spectator].model;
        const int mb = m_crowd.spectators()[queue[b].spectator].model;
        return ma != mb ? ma < mb : lodOf[a] < lodOf[b];
    });
    std::vector<float> instances;
    instances.reserve(queue.size() * kInstanceFloats);
    std::vector<DrawCommand> commands;
    for (const std::uint32_t q : order) {
        const Queued& entry = queue[q];
        const spectators::Spectator& person = m_crowd.spectators()[entry.spectator];
        const auto modelIndex = static_cast<std::size_t>(person.model);
        const Model& model = m_models[modelIndex];
        const int lod = lodOf[q];
        // Riders play their carrier's clip and frame (0x82EE2E80).
        const Person& state = m_people[person.leader >= 0 ? static_cast<std::size_t>(person.leader) : entry.spectator];
        if (state.animation < 0) {
            continue;
        }
        const auto animation = static_cast<std::size_t>(state.animation);
        const auto bones = static_cast<std::uint32_t>(m_crowd.animations()[animation].clip.boneCount());
        const float bone = static_cast<float>(m_clipBase[animation] + static_cast<std::uint32_t>(state.frame) * bones);
        const float scale = entry.walker ? 1.0F : person.modelScale;
        const glm::vec3 right = glm::vec3(std::cos(entry.heading), 0.0F, -std::sin(entry.heading)) * scale;
        const glm::vec3 up = glm::vec3(0.0F, 1.0F, 0.0F) * scale;
        const glm::vec3 forward = glm::vec3(std::sin(entry.heading), 0.0F, std::cos(entry.heading)) * scale;
        const glm::vec4 fade = entry.walker ? draw::kWalkerFade : draw::kModelFade;
        const std::array<float, kInstanceFloats> values{right.x, up.x, forward.x, entry.position.x, right.y, up.y,
            forward.y, entry.position.y, right.z, up.z, forward.z, entry.position.z, bone,
            static_cast<float>(model.layer), fade.x, fade.y};
        const auto instance = static_cast<std::uint32_t>(instances.size() / kInstanceFloats);
        instances.insert(instances.end(), values.begin(), values.end());
        const auto lodIndex = static_cast<std::size_t>(lod);
        if (commands.empty() || commands.back().firstIndex != model.firstIndex[lodIndex]
            || commands.back().baseVertex != static_cast<std::int32_t>(model.baseVertex)) {
            commands.push_back({model.indexCount[lodIndex], 0, model.firstIndex[lodIndex],
                static_cast<std::int32_t>(model.baseVertex), instance});
        }
        ++commands.back().instanceCount;
        ++m_stats.models[lodIndex];
        m_stats.walkers += entry.walker ? 1 : 0;
    }

    std::vector<DrawCommand> sprites;
    for (const std::uint32_t g : visible) {
        const spectators::CrowdGroup& group = m_crowd.groups()[g];
        // Past the far fade's end every sprite is collapsed to a point.
        if (m_groupSpriteCount[g] == 0
            || distanceToBox(camera, group.boundsMin, group.boundsMax) >= draw::kSpriteFade.z) {
            continue;
        }
        sprites.push_back(
            {static_cast<std::uint32_t>(kQuadIndices.size()), m_groupSpriteCount[g], 0, 0, m_groupSpriteFirst[g]});
        ++m_stats.spriteSets;
        m_stats.spriteInstances += m_groupSpriteCount[g];
    }

    const glm::vec3 toSun = -frame.lighting.sunDirection;
    const float emissive = TrackFrameConstants::emissiveSwitchOn(
        frame.lighting.switchOnLights, settings != nullptr ? settings->trackLightMap.z : 0.0F, frame.time)
                               .y;
    const glm::vec3 look = -glm::vec3(glm::inverse(frame.view)[2]);
    glBindTextureUnit(kMaskUnit, m_shadowMask != 0 ? m_shadowMask : m_white.id());

    if (!commands.empty()) {
        glNamedBufferSubData(m_instances, 0, static_cast<GLsizeiptr>(instances.size() * 4), instances.data());
        glNamedBufferSubData(
            m_modelCommands, 0, static_cast<GLsizeiptr>(commands.size() * sizeof(DrawCommand)), commands.data());
        m_modelShader.use();
        setSceneLighting(m_modelShader, frame.lighting);
        m_modelShader.set("uViewProjection", viewProjection);
        m_modelShader.set("uCamera", camera);
        m_modelShader.set("uToSun", toSun);
        m_modelShader.set("uCrowdSunColour", frame.lighting.sunColour);
        m_modelShader.set("uCrowdAmbient", frame.lighting.ambientColour);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBonesBinding, m_bones);
        glBindTextureUnit(kSkinUnit, m_skins);
        glBindVertexArray(m_modelVao);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_modelCommands);
        glMultiDrawElementsIndirect(
            GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(commands.size()), sizeof(DrawCommand));
    }
    if (!sprites.empty()) {
        glNamedBufferSubData(
            m_spriteCommands, 0, static_cast<GLsizeiptr>(sprites.size() * sizeof(DrawCommand)), sprites.data());
        m_spriteShader.use();
        setSceneLighting(m_spriteShader, frame.lighting);
        m_spriteShader.set("uViewProjection", viewProjection);
        m_spriteShader.set("uCamera", camera);
        m_spriteShader.set("uCamLook", look);
        m_spriteShader.set("uFade", draw::kSpriteFade);
        m_spriteShader.set("uAtlasRows", static_cast<float>(m_atlasRows));
        m_spriteShader.set("uToSun", toSun);
        m_spriteShader.set("uCrowdSunColour", frame.lighting.sunColour);
        m_spriteShader.set("uCrowdAmbient", frame.lighting.ambientColour);
        m_spriteShader.set("uEmissive", emissive);
        m_atlas.bind(kAtlasUnit);
        // The quads always face along the view; which way they wind does
        // not matter.
        glDisable(GL_CULL_FACE);
        glBindVertexArray(m_spriteVao);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_spriteCommands);
        glMultiDrawElementsIndirect(
            GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(sprites.size()), sizeof(DrawCommand));
        glEnable(GL_CULL_FACE);
    }
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
}

} // namespace fh::render
