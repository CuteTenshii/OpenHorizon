#include "ScatterPass.h"

#include "WorldRenderer.h"

#include <cstdint>
#include <string>

namespace fh::render {

namespace {

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

const char* const kFragmentBody = R"(
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
        vec3 mirrored = reflect(-toEye, surfaceNormal);
        vec3 colour = mix(vec3(0.02, 0.07, 0.08), skyColour(mirrored), fresnel);
        float glint = pow(max(dot(mirrored, toSun), 0.0), 200.0);
        colour += uSunColour * glint * 4.0;
        // The scene target holds square-root colour (see FrameRenderer).
        fragColour = vec4(sqrt(max(applyFog(colour, vPosition, uCamera), vec3(0.0))), mix(0.7, 1.0, fresnel));
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
    vec3 ambient = hemisphereAmbient(normal, uAmbientTop, uAmbientBottom);
    vec3 colour = albedo * (ambient + uSunColour * sun);
    fragColour = vec4(sqrt(max(applyFog(colour, vPosition, uCamera), vec3(0.0))), 1.0);
}
)";

std::string worldFragmentSource()
{
    return std::string("#version 450 core\n") + kSceneLightingGlsl + kFragmentBody;
}

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

ScatterPass::Mesh::~Mesh()
{
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ibo);
}

ScatterPass::ScatterPass()
    : m_shader("world scatter", kVertexSource, worldFragmentSource())
    , m_white(Texture::solid(200, 200, 200, 255))
{
}

std::shared_ptr<ScatterPass::Mesh> ScatterPass::upload(const world::TileMesh& mesh)
{
    using world::TileMesh;
    auto gpu = std::make_shared<Mesh>();
    glCreateBuffers(1, &gpu->vbo);
    glNamedBufferStorage(
        gpu->vbo, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)), mesh.vertices.data(), 0);
    glCreateBuffers(1, &gpu->ibo);
    glNamedBufferStorage(
        gpu->ibo, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)), mesh.indices.data(), 0);
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
    return gpu;
}

void ScatterPass::begin(const FrameParams& frame)
{
    m_shader.use();
    m_shader.set("uViewProjection", frame.projection * frame.view);
    m_shader.set("uModel", glm::mat4(1.0F));
    m_shader.set("uCamera", frame.cameraPosition);
    setSceneLighting(m_shader, frame.lighting);
    m_shader.set("uTime", frame.time);
}

void ScatterPass::setModel(const glm::mat4& model)
{
    m_shader.set("uModel", model);
}

std::size_t ScatterPass::draw(const Mesh& mesh, const std::function<const Texture*(std::uint32_t)>& texture,
    const world::WorldIndex& index, int cameraZone)
{
    using Layer = world::TileMesh::Layer;
    const auto textureOr = [&texture](std::uint32_t id, const Texture* fallback) {
        const Texture* loaded = id == world::TileMesh::kNoTexture ? nullptr : texture(id);
        return loaded != nullptr ? loaded : fallback;
    };
    std::size_t triangles = 0;
    glBindVertexArray(mesh.vao);
    for (const world::TileMesh::Batch& batch : mesh.batches) {
        if (batch.shading == world::TileMesh::Shading::Water || batch.indexCount == 0) {
            continue;
        }
        if (batch.backdrop && batch.chunk != world::TileMesh::kMergedChunks
            && !index.chunks()[batch.chunk].visibleFrom(cameraZone)) {
            continue;
        }
        const Texture* base = textureOr(batch.texture, &m_white);
        base->bind(0);
        textureOr(batch.layers[Layer::LayerB], base)->bind(1);
        textureOr(batch.layers[Layer::LayerC], base)->bind(2);
        textureOr(batch.layers[Layer::SplatMap], &m_white)->bind(3);
        textureOr(batch.layers[Layer::OcclusionMap], &m_white)->bind(4);
        m_shader.set("uShading", shadingCode(batch.shading));
        m_shader.set("uScaleA", glm::vec2(batch.layerScales[0], batch.layerScales[1]));
        m_shader.set("uScaleB", glm::vec2(batch.layerScales[2], batch.layerScales[3]));
        m_shader.set("uScaleC", glm::vec2(batch.layerScales[4], batch.layerScales[5]));
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
        triangles += batch.indexCount / 3;
    }
    return triangles;
}

} // namespace fh::render
