#include "CarRenderer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fh::render {

namespace {

using carmodel::CarMesh;
using carmodel::Shading;

const char* const kVertexSource = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexcoord0;
layout(location = 4) in vec4 aColour;
uniform mat4 uViewProjection;
uniform mat4 uModel;
out vec3 vPosition;
out vec3 vNormal;
out vec2 vTexcoord0;
out vec4 vColour;
void main()
{
    vec4 world = uModel * vec4(aPosition, 1.0);
    vPosition = world.xyz;
    vNormal = mat3(uModel) * aNormal;
    vTexcoord0 = aTexcoord0;
    vColour = aColour;
    gl_Position = uViewProjection * world;
}
)";

const char* const kFragmentSource = R"(#version 450 core
in vec3 vPosition;
in vec3 vNormal;
in vec2 vTexcoord0;
in vec4 vColour;
layout(location = 0) out vec4 fragColour;
layout(binding = 0) uniform sampler2D uDiffuse;
uniform int uShading;
uniform vec4 uColour;
uniform float uGlow;
uniform float uMetallic;
uniform vec3 uCamera;
uniform vec3 uSunDirection;
uniform vec3 uSunColour;
uniform vec3 uSkyColour;
uniform vec3 uHorizonColour;
uniform vec3 uGroundColour;
uniform vec3 uFogColour;
uniform float uFogDensity;

const int kPlain = 0;
const int kPaint = 1;
const int kGlass = 2;
const int kLights = 3;
const int kMetal = 4;
const int kRubber = 5;
const int kInterior = 6;
const int kDecal = 7;

// The sky the surface would mirror: horizon colour below, zenith above.
vec3 skyReflection(vec3 direction)
{
    float up = clamp(direction.y, -0.2, 1.0);
    return up < 0.0 ? uGroundColour * 2.0 : mix(uHorizonColour, uSkyColour, sqrt(up));
}

void main()
{
    vec3 normal = normalize(gl_FrontFacing ? vNormal : -vNormal);
    vec3 toEye = normalize(uCamera - vPosition);
    vec3 toSun = -uSunDirection;
    vec4 texel = texture(uDiffuse, vTexcoord0);
    if ((uShading == kDecal || uShading == kPlain || uShading == kInterior) && texel.a < 0.5) {
        discard;
    }
    vec3 albedo = texel.rgb * uColour.rgb;
    if (uShading == kPaint) {
        // The body atlas's alpha marks decals (badges, script); elsewhere
        // the paint colour shows and the texel's colour is not used.
        albedo = mix(uColour.rgb, texel.rgb, texel.a);
    }
    float diffuse = max(dot(normal, toSun), 0.0);
    vec3 ambient = mix(uGroundColour, (uSkyColour + uHorizonColour) * 0.5, 0.5 + 0.5 * normal.y);
    vec3 halfway = normalize(toSun + toEye);
    float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(dot(normal, toEye), 0.0, 1.0), 5.0);
    vec3 reflection = skyReflection(reflect(-toEye, normal));

    vec3 colour;
    float alpha = 1.0;
    if (uShading == kPaint) {
        // A clear coat over the base colour: sky reflection by Fresnel and a
        // tight sun highlight.
        vec3 base = albedo * (ambient * 0.8 + uSunColour * diffuse);
        float highlight = pow(max(dot(normal, halfway), 0.0), 200.0);
        // Metallic flakes spread a coloured sheen around the highlight.
        float sheen = pow(max(dot(normal, halfway), 0.0), 12.0) * uMetallic;
        colour = mix(base, reflection, fresnel * 0.8) + uSunColour * (highlight * 2.0 + sheen * albedo * 0.8);
    } else if (uShading == kGlass) {
        float highlight = pow(max(dot(normal, halfway), 0.0), 300.0);
        colour = reflection * 0.9 + uSunColour * highlight * 3.0 + albedo * 0.05;
        alpha = clamp(0.35 + fresnel * 0.6, 0.0, 0.95);
    } else if (uShading == kMetal) {
        float highlight = pow(max(dot(normal, halfway), 0.0), 60.0);
        colour = albedo * (ambient * 0.3 + uSunColour * diffuse * 0.3) + reflection * albedo * 0.8
            + uSunColour * highlight;
    } else if (uShading == kLights) {
        colour = albedo * (ambient * 0.6 + uSunColour * diffuse * 0.6) + albedo * uGlow * 4.0;
        colour += reflection * fresnel * 0.5;
    } else if (uShading == kRubber) {
        colour = albedo * (ambient * 0.9 + uSunColour * diffuse * 0.8);
    } else {
        colour = albedo * (ambient * 0.9 + uSunColour * diffuse);
    }
    float fog = 1.0 - exp(-length(uCamera - vPosition) * uFogDensity);
    fragColour = vec4(mix(colour, uFogColour, clamp(fog, 0.0, 1.0)), alpha);
}
)";

GLint shadingCode(Shading shading)
{
    return static_cast<GLint>(shading);
}

} // namespace

CarRenderer::CarRenderer(const carmodel::CarModel& model)
    : m_nodes(model.mesh.nodes)
    , m_batches(model.mesh.batches)
    , m_white(Texture::solid(255, 255, 255, 255))
    , m_shader("car", kVertexSource, kFragmentSource)
{
    const CarMesh& mesh = model.mesh;
    glCreateBuffers(1, &m_vbo);
    glNamedBufferStorage(m_vbo, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(float)), mesh.vertices.data(), 0);
    glCreateBuffers(1, &m_ibo);
    glNamedBufferStorage(
        m_ibo, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)), mesh.indices.data(), 0);
    glCreateVertexArrays(1, &m_vao);
    constexpr GLsizei stride = CarMesh::kFloatsPerVertex * sizeof(float);
    glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, stride);
    glVertexArrayElementBuffer(m_vao, m_ibo);
    const auto attribute = [&](GLuint index, GLint size, GLenum type, GLboolean normalised, int floatOffset) {
        glEnableVertexArrayAttrib(m_vao, index);
        glVertexArrayAttribFormat(
            m_vao, index, size, type, normalised, static_cast<GLuint>(floatOffset * static_cast<int>(sizeof(float))));
        glVertexArrayAttribBinding(m_vao, index, 0);
    };
    attribute(0, 3, GL_FLOAT, GL_FALSE, 0);
    attribute(1, 3, GL_FLOAT, GL_FALSE, CarMesh::kNormal);
    attribute(2, 2, GL_FLOAT, GL_FALSE, CarMesh::kTexcoord);
    attribute(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, CarMesh::kColour);

    m_textures.reserve(model.textures.size());
    for (const carmodel::CarModel::Texture& texture : model.textures) {
        // Normal maps (BC5) are linear; everything else is colour.
        const bool colour = texture.chain.format != world::TextureSurface::Format::Bc5;
        m_textures.push_back(Texture::fromMipChain(texture.chain, colour));
    }
}

CarRenderer::~CarRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vbo);
    glDeleteBuffers(1, &m_ibo);
}

glm::mat4 CarRenderer::nodeTransform(const CarMesh::Node& node, const CarPose& pose) const
{
    if (!node.corner || node.kind == CarMesh::NodeKind::Body || node.kind == CarMesh::NodeKind::Cockpit) {
        return pose.body;
    }
    const auto c = static_cast<std::size_t>(*node.corner);
    const glm::vec3 up(0.0F, 1.0F, 0.0F);
    glm::mat4 m = glm::translate(pose.body, pose.hubs[c]);
    m *= glm::mat4_cast(glm::angleAxis(pose.steer[c], up));
    // Rolling forward turns the top of the wheel towards +z; in this
    // left-handed car space that is a positive angle about +x.
    if (node.kind != CarMesh::NodeKind::Caliper) {
        m *= glm::mat4_cast(glm::angleAxis(pose.spin[c], glm::vec3(1.0F, 0.0F, 0.0F)));
    }
    return glm::translate(m, -node.pivot);
}

void CarRenderer::draw(const FrameParams& frame, const CarPose& pose, bool translucent)
{
    m_shader.use();
    m_shader.set("uViewProjection", frame.projection * frame.view);
    m_shader.set("uCamera", frame.cameraPosition);
    m_shader.set("uSunDirection", frame.sunDirection);
    m_shader.set("uSunColour", frame.sunColour);
    m_shader.set("uSkyColour", frame.skyColour);
    m_shader.set("uHorizonColour", frame.horizonColour);
    m_shader.set("uGroundColour", frame.groundColour);
    m_shader.set("uFogColour", frame.fogColour);
    m_shader.set("uFogDensity", frame.fogDensity);
    if (translucent) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
    }
    glBindVertexArray(m_vao);
    for (const CarMesh::Node& node : m_nodes) {
        m_shader.set("uModel", nodeTransform(node, pose));
        for (std::uint32_t b = node.firstBatch; b < node.firstBatch + node.batchCount; ++b) {
            const CarMesh::Batch& batch = m_batches[b];
            if (batch.translucent != translucent || batch.indexCount == 0) {
                continue;
            }
            const bool hasTexture = batch.texture != CarMesh::kNoTexture && batch.texture < m_textures.size();
            (hasTexture ? m_textures[batch.texture] : m_white).bind(0);
            m_shader.set("uShading", shadingCode(batch.shading));
            glm::vec4 tint = batch.colour;
            if (batch.shading == Shading::Paint) {
                tint *= glm::vec4(pose.paint, 1.0F);
            }
            m_shader.set("uColour", tint);
            m_shader.set("uMetallic", pose.metallic ? 1.0F : 0.0F);
            // Tail lamps sit at the back of the car; the rest are headlamps.
            float glow = 0.0F;
            if (batch.shading == Shading::Lights) {
                const bool rear = batch.material.find("tail") != std::string::npos
                    || batch.material.find("brake") != std::string::npos;
                glow = rear ? std::max(pose.brakeLights, pose.headLights * 0.4F) : pose.headLights;
            }
            m_shader.set("uGlow", glow);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
        }
    }
    glBindVertexArray(0);
    if (translucent) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
}

} // namespace fh::render
