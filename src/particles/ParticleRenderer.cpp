#include "ParticleRenderer.h"

#include "ParticleManager.h"
#include "ParticleShaders.h"
#include "ParticleSystem.h"

#include "base/Error.h"
#include "base/Log.h"
#include "render/GlProgram.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace openhorizon::particles {
namespace {

/// One particle as the shaders read it (ParticleShaders.cpp).
struct Record {
    glm::vec4 position;
    glm::vec4 shape;
    glm::vec4 motion;
    glm::vec4 frame;
};
static_assert(sizeof(Record) == 64);

/// The quaternion of a rotation given as row-vector matrix rows, as
/// XMQuaternionRotationMatrix builds it (the game's 0x82487140).
glm::vec4 quaternionOf(const glm::mat3& m)
{
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        return {(m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s, 0.25F * s};
    }
    if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0F + m[0][0] - m[1][1] - m[2][2]) * 2.0F;
        return {0.25F * s, (m[0][1] + m[1][0]) / s, (m[2][0] + m[0][2]) / s, (m[1][2] - m[2][1]) / s};
    }
    if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0F + m[1][1] - m[0][0] - m[2][2]) * 2.0F;
        return {(m[0][1] + m[1][0]) / s, 0.25F * s, (m[1][2] + m[2][1]) / s, (m[2][0] - m[0][2]) / s};
    }
    const float s = std::sqrt(1.0F + m[2][2] - m[0][0] - m[1][1]) * 2.0F;
    return {(m[2][0] + m[0][2]) / s, (m[1][2] + m[2][1]) / s, 0.25F * s, (m[0][1] - m[1][0]) / s};
}

std::string lower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The effect texture loader replaces a .tga extension with .xds
/// (0x82CF9198), which is how names such as particle_smoke.tga resolve.
std::string texturePath(const std::string& name)
{
    std::string file = lower(name);
    if (file.ends_with(".tga")) {
        file.replace(file.size() - 4, 4, ".xds");
    }
    return std::string(kEffectsFolder) + "/" + file;
}

/// Whether an .xds file's fetch constant (bytes 28-51) marks its colour
/// channels as gamma-encoded: the X, Y and Z sign fields (bits 2-7 of
/// the first dword) set to 3.
bool isGammaTexture(const Bytes& data)
{
    if (data.size() < 32) {
        return false;
    }
    const std::uint32_t word = (std::uint32_t(data[28]) << 24) | (std::uint32_t(data[29]) << 16)
        | (std::uint32_t(data[30]) << 8) | std::uint32_t(data[31]);
    return ((word >> 2) & 3U) == 3U && ((word >> 4) & 3U) == 3U && ((word >> 6) & 3U) == 3U;
}

int quadTypeOf(const EffectDefinition& d)
{
    if (d.render.type == ParticleType::BillboardedQuad) {
        return 0;
    }
    return d.spinAxis == SpinAxis::PerpToDirOfTravel ? 1 : 2;
}

} // namespace

ParticleRenderer::ParticleRenderer(const Vfs& vfs)
    : m_vfs(&vfs)
    , m_program(render::linkProgram("particle", glsl::kVertex, glsl::kFragment))
{
    glCreateVertexArrays(1, &m_vao);
    glCreateBuffers(1, &m_records);
    glCreateBuffers(1, &m_indices);
    glCreateSamplers(1, &m_sampler);
    // The animation frames step past the texture's edge and rely on
    // wrapping (the vertex shader never takes the frame index modulo the
    // frame count).
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateSamplers(1, &m_depthSampler);
    glSamplerParameteri(m_depthSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_depthSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_depthSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_depthSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCreateFramebuffers(1, &m_depthFramebuffer);
}

ParticleRenderer::~ParticleRenderer()
{
    for (auto& [name, t] : m_textures) {
        if (t.id != 0) {
            glDeleteTextures(1, &t.id);
        }
    }
    glDeleteTextures(1, &m_depthCopy);
    glDeleteFramebuffers(1, &m_depthFramebuffer);
    glDeleteSamplers(1, &m_sampler);
    glDeleteSamplers(1, &m_depthSampler);
    glDeleteBuffers(1, &m_records);
    glDeleteBuffers(1, &m_indices);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteProgram(m_program);
}

GLint ParticleRenderer::uniform(const char* name)
{
    const auto it = m_uniforms.find(name);
    if (it != m_uniforms.end()) {
        return it->second;
    }
    const GLint location = glGetUniformLocation(m_program, name);
    m_uniforms.emplace(name, location);
    return location;
}

GLuint ParticleRenderer::texture(const std::string& name)
{
    if (name.empty()) {
        return 0;
    }
    Texture& t = m_textures[lower(name)];
    if (t.loaded) {
        return t.id;
    }
    t.loaded = true;
    const std::string path = texturePath(name);
    const std::optional<Bytes> data = m_vfs->read(path);
    if (!data) {
        m_missing.push_back(path);
        return 0;
    }
    try {
        const world::RgbaImage image = world::decodeXboxTexture(*data);
        const bool gamma = isGammaTexture(*data);
        int levels = 1;
        for (int size = std::max(image.width, image.height); size > 1; size /= 2) {
            ++levels;
        }
        glCreateTextures(GL_TEXTURE_2D, 1, &t.id);
        glTextureStorage2D(t.id, levels, gamma ? GL_SRGB8_ALPHA8 : GL_RGBA8, image.width, image.height);
        glTextureSubImage2D(t.id, 0, 0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        glGenerateTextureMipmap(t.id);
    } catch (const DataError& e) {
        logWarning("particles: {}: {}", path, e.what());
        m_missing.push_back(path);
    }
    return t.id;
}

void ParticleRenderer::copyDepth(int width, int height)
{
    if (width != m_depthWidth || height != m_depthHeight || m_depthCopy == 0) {
        glDeleteTextures(1, &m_depthCopy);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_depthCopy);
        glTextureStorage2D(m_depthCopy, 1, GL_DEPTH_COMPONENT32F, width, height);
        glNamedFramebufferTexture(m_depthFramebuffer, GL_DEPTH_ATTACHMENT, m_depthCopy, 0);
        m_depthWidth = width;
        m_depthHeight = height;
    }
    GLint bound = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    glBlitNamedFramebuffer(static_cast<GLuint>(bound), m_depthFramebuffer, 0, 0, width, height, 0, 0, width, height,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
}

void ParticleRenderer::draw(const ParticleManager& manager, const ParticleFrame& frame)
{
    m_lastDrawn = 0;
    bool anySoft = false;
    bool any = false;
    for (const ParticleManager::System& s : manager.systems()) {
        if (s.system->size() > 0 && s.system->definition().render.type != ParticleType::Modeled) {
            any = true;
            anySoft = anySoft || s.system->definition().soft();
        }
    }
    if (!any) {
        return;
    }
    std::array<GLint, 4> viewport{};
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    if (anySoft) {
        copyDepth(viewport[2], viewport[3]);
    }

    glUseProgram(m_program);
    glBindVertexArray(m_vao);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindSampler(0, m_sampler);
    glBindSampler(1, m_depthSampler);
    glBindSampler(2, m_sampler);
    glBindTextureUnit(1, m_depthCopy);

    const auto setVec3
        = [this](const char* name, const glm::vec3& v) { glProgramUniform3f(m_program, uniform(name), v.x, v.y, v.z); };
    glProgramUniformMatrix4fv(
        m_program, uniform("u_viewProjection"), 1, GL_FALSE, glm::value_ptr(frame.viewProjection));
    setVec3("u_cameraPosition", frame.cameraPosition);
    setVec3("u_viewForward", frame.viewForward);
    glProgramUniform4f(
        m_program, uniform("u_sunColour"), frame.sunColour.r, frame.sunColour.g, frame.sunColour.b, 0.0F);
    setVec3("u_ambientColour", frame.ambientColour);
    setVec3("u_carPosition", frame.headLightCarPosition);
    setVec3("u_carUp", frame.headLightCarUp);
    setVec3("u_carRight", frame.headLightCarRight);
    glProgramUniform1f(m_program, uniform("u_nearPlane"), frame.nearPlane);

    for (const ParticleManager::System& s : manager.systems()) {
        if (s.system->size() > 0 && s.system->definition().render.type != ParticleType::Modeled) {
            drawSystem(*s.system, frame);
        }
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);
}

void ParticleRenderer::drawSystem(const ParticleSystem& system, const ParticleFrame& frame)
{
    const EffectDefinition& d = system.definition();
    const GLuint diffuse = texture(d.render.texture);
    if (diffuse == 0) {
        // The game samples no texture, reads alpha 0 and discards every
        // pixel (the shaders' alpha test), so nothing shows.
        return;
    }
    GLuint gradient = 0;
    if (d.gradient()) {
        gradient = texture(d.render.texture2);
        if (gradient == 0) {
            return;
        }
    }

    // The game keeps particles in front of the camera and, for zsort
    // effects, draws them far to near (0x82DC67E8, comparator 0x82DBF7B0).
    struct Entry {
        std::size_t index;
        float key;
    };
    std::vector<Entry> order;
    order.reserve(system.size());
    for (std::size_t i = 0; i < system.size(); ++i) {
        const Particle& p = system.at(i);
        if (!p.alive) {
            continue;
        }
        const float key = glm::dot(p.position - frame.cameraPosition, frame.viewForward);
        if (key >= 0.0F) {
            order.push_back({i, key});
        }
    }
    if (order.empty()) {
        return;
    }
    if (d.render.zSort) {
        std::ranges::stable_sort(order, [](const Entry& a, const Entry& b) { return a.key > b.key; });
    }

    const int quadType = quadTypeOf(d);
    std::vector<Record> records(order.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        const Particle& p = system.at(order[k].index);
        Record& r = records[k];
        r.position = glm::vec4(p.position, 1.0F);
        r.shape = glm::vec4(p.size, p.age, p.random);
        if (quadType == 2) {
            const glm::mat3 m = ParticleSystem::orientation(p);
            r.motion = quaternionOf(m);
            r.frame = glm::vec4(m[1], 0.0F);
        } else {
            r.motion = glm::vec4(p.velocity, p.spinAngle);
            r.frame = glm::vec4(0.0F);
        }
    }
    const std::size_t count = records.size();
    if (count > m_recordCapacity) {
        m_recordCapacity = std::max(count, m_recordCapacity * 2);
        glNamedBufferData(
            m_records, static_cast<GLsizeiptr>(m_recordCapacity * sizeof(Record)), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(m_records, 0, static_cast<GLsizeiptr>(count * sizeof(Record)), records.data());
    if (count > m_indexCapacity) {
        m_indexCapacity = std::max(count, m_indexCapacity * 2);
        std::vector<std::uint32_t> indices(m_indexCapacity * 6);
        for (std::size_t q = 0; q < m_indexCapacity; ++q) {
            const auto base = static_cast<std::uint32_t>(q * 4);
            const std::array<std::uint32_t, 6> quad = {base, base + 1, base + 2, base, base + 2, base + 3};
            std::ranges::copy(quad, indices.begin() + static_cast<std::ptrdiff_t>(q * 6));
        }
        glNamedBufferData(
            m_indices, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);
        glVertexArrayElementBuffer(m_vao, m_indices);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_records);

    const RenderSettings& r = d.render;
    const Material& m = r.material;
    glProgramUniform1i(m_program, uniform("u_quadType"), quadType);
    glProgramUniform4f(
        m_program, uniform("u_params"), m.fadeInTo, m.fadeOutFrom, r.maxScreenSize, r.forceEdgeOnFadeOff ? 1.0F : 0.0F);
    glProgramUniform4fv(m_program, uniform("u_beginColour"), 1, glm::value_ptr(m.rgbStart));
    glProgramUniform4fv(m_program, uniform("u_midColour"), 1, glm::value_ptr(m.rgbMid));
    glProgramUniform4fv(m_program, uniform("u_endColour"), 1, glm::value_ptr(m.rgbEnd));
    glProgramUniform4f(m_program, uniform("u_lightDirection"), frame.towardsSun.x, frame.towardsSun.y,
        frame.towardsSun.z, r.headLightPower);
    glProgramUniform4fv(m_program, uniform("u_lightInfluence"), 1, glm::value_ptr(r.lightInfluence));
    glProgramUniform4fv(m_program, uniform("u_specularInfluence"), 1, glm::value_ptr(r.specularLightInfluence));
    glProgramUniform1f(m_program, uniform("u_density"), d.density);
    glProgramUniform1f(m_program, uniform("u_facingMatchesVelocity"), r.facingMatchesVelocity ? 1.0F : 0.0F);
    int variant = 0;
    variant |= d.animated() ? 1 : 0;
    variant |= d.gradient() ? 2 : 0;
    variant |= d.rowOnly() ? 4 : 0;
    glProgramUniform1i(m_program, uniform("u_variant"), variant);
    glProgramUniform3f(m_program, uniform("u_animation"), d.animationRate, static_cast<float>(d.framesX),
        static_cast<float>(d.framesY));
    glProgramUniform1f(m_program, uniform("u_gradientRows"), static_cast<float>(d.gradientRows));
    glProgramUniform1i(m_program, uniform("u_soft"), d.soft() ? 1 : 0);
    glProgramUniform1i(m_program, uniform("u_gammaOutput"), m.blend == BlendType::Alpha ? 1 : 0);

    if (m.blend == BlendType::Additive) {
        glBlendFunc(GL_ONE, GL_ONE);
    } else {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glBindTextureUnit(0, diffuse);
    glBindTextureUnit(2, gradient);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(count * 6), GL_UNSIGNED_INT, nullptr);
    m_lastDrawn += count;
}

} // namespace openhorizon::particles
