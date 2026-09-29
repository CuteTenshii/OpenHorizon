#include "CarRenderer.h"

#include "base/Log.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <utility>

namespace fh::render {

namespace {

using carmodel::CarMesh;
using carmodel::ShaderSettings;

const glm::mat4 kMirrorZ = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 1.0F, -1.0F));

glm::vec3 mirrorZ(const glm::vec3& v)
{
    return {v.x, v.y, -v.z};
}

/// The rotation (or reflection) of a placement, its columns normalised so a
/// scaled wheel keeps unit directions.
glm::mat3 rotationPart(const glm::mat4& m)
{
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) {
        const float length = glm::length(r[i]);
        if (length > 0.0F) {
            r[i] /= length;
        }
    }
    return r;
}

/// SH coefficients in D3DX order (Y00, Y1-1, Y10, Y11), whose band 1 is
/// proportional to (-y, z, -x) of a direction.
constexpr float kPi = 3.14159265358979F;

/// D3DXSHEvalHemisphereLight for order 2 as the executable's copy computes
/// it (0x83158FC8): the zonal coefficients 2 sqrt(pi) avg and 2 sqrt(pi / 3)
/// (top - avg) with avg = (top + bottom) / 2, turned towards `dir` by the
/// rotation at 0x831580F0 (band 1 = (-dir.y, dir.z, -dir.x)) and multiplied
/// by 1.5 (the constant at 0x82000C80).
glm::vec4 shHemisphere(const glm::vec3& dir, float top, float bottom)
{
    const float average = (top + bottom) * 0.5F;
    const float band0 = 2.0F * std::sqrt(kPi) * average * 1.5F;
    const float band1 = 2.0F * std::sqrt(kPi / 3.0F) * (top - average) * 1.5F;
    return {band0, -dir.y * band1, dir.z * band1, -dir.x * band1};
}

/// D3DXSHEvalDirectionalLight for order 2 and unit intensity (0x83158E78):
/// the SH basis at `dir` (0x83158910: 0.5 / sqrt(pi) and 0.5 sqrt(3 / pi)
/// times (-y, z, -x)) scaled by pi / 0.75.
glm::vec4 shDirectional(const glm::vec3& dir)
{
    const float scale = kPi / 0.75F;
    const float y0 = 0.5F / std::sqrt(kPi);
    const float y1 = 0.5F * std::sqrt(3.0F / kPi);
    return glm::vec4(y0, -y1 * dir.y, y1 * dir.z, -y1 * dir.x) * scale;
}

/// Band 1 of `sh` expressed in the frame `toObject` maps world directions
/// into, as 0x8244A320 rotates the coefficients by the object's matrix
/// before upload.
glm::vec4 rotateSh(const glm::vec4& sh, const glm::mat3& toObject)
{
    const glm::vec3 world(-sh.w, -sh.y, sh.z);
    const glm::vec3 object = toObject * world;
    return {sh.x, -object.y, object.z, -object.x};
}

/// shDirectionalColour: half the sun's colour (the car light block's
/// 0x82E1155C store of SunColor · SunColorMult · 0.5), w its luminance with
/// the weights at 0x82001110 (0x82466380).
glm::vec4 directionalColour(const glm::vec3& sun)
{
    const glm::vec3 colour = sun * 0.5F;
    return {colour, glm::dot(colour, glm::vec3(0.299F, 0.587F, 0.114F))};
}

/// The model's rotation as a quaternion (x, y, z, w); the layout is
/// inferred from the name, the fill site was not traced.
glm::vec4 modelToWorldQuaternion(const glm::mat4& m)
{
    const glm::quat q = glm::quat_cast(rotationPart(m));
    return {q.x, q.y, q.z, q.w};
}

/// Vertex streams of the car vertex shaders, as the translator binds them
/// (stream = 95 - fetch constant): the part's vertices, the per-vertex morph
/// offsets of damage (vf90) and the damage grid (vf89).
constexpr GLuint kVertexStream = 0;
constexpr GLuint kMorphStream = 5;
constexpr GLuint kDamageStream = 6;
/// The damage grid the vertex shaders index: 10 × 10 × 10 cells of two
/// float4 each.
constexpr std::size_t kDamageGridBytes = 1000 * 2 * 16;

/// OpenGL factor of an Xbox 360 D3DBLEND value.
GLenum blendFactor(std::uint32_t d3dBlend)
{
    switch (d3dBlend) {
    case 0:
        return GL_ZERO;
    case 1:
        return GL_ONE;
    case 4:
        return GL_SRC_COLOR;
    case 5:
        return GL_ONE_MINUS_SRC_COLOR;
    case 6:
        return GL_SRC_ALPHA;
    case 7:
        return GL_ONE_MINUS_SRC_ALPHA;
    case 8:
        return GL_DST_COLOR;
    case 9:
        return GL_ONE_MINUS_DST_COLOR;
    case 10:
        return GL_DST_ALPHA;
    case 11:
        return GL_ONE_MINUS_DST_ALPHA;
    default:
        return GL_ONE;
    }
}

/// The pass's own blending, else the engine's for its draw list: none for
/// the opaque list, source alpha over the scene for the translucent one
/// (the state the detail glass techniques' Cleanup passes restore).
void applyBlend(const std::optional<carmodel::CarPass::Blend>& blend, bool translucent)
{
    if (blend) {
        if (blend->enable) {
            glEnable(GL_BLEND);
            glBlendFunc(blendFactor(blend->source), blendFactor(blend->destination));
        } else {
            glDisable(GL_BLEND);
        }
        return;
    }
    if (translucent) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

} // namespace

/// Compiled programs shared by every car on the GL thread, by translated
/// source.
class CarProgramCache {
public:
    std::shared_ptr<XenosProgram> get(const std::shared_ptr<const carmodel::CarPass>& source, const std::string& name)
    {
        if (const auto known = m_programs.find(source.get()); known != m_programs.end()) {
            return known->second;
        }
        std::shared_ptr<XenosProgram> program;
        try {
            program = std::make_shared<XenosProgram>(source->program, name, source->squareRootOutput);
        } catch (const std::exception& e) {
            logWarning("car shader {}: {}", name, e.what());
        }
        m_sources.push_back(source);
        m_programs.emplace(source.get(), program);
        return program;
    }

private:
    /// Keeps the sources alive so their addresses stay unique keys.
    std::vector<std::shared_ptr<const carmodel::CarPass>> m_sources;
    std::map<const carmodel::CarPass*, std::shared_ptr<XenosProgram>> m_programs;
};

namespace {

/// The cache lives while any car renderer does, so its programs go before
/// the GL context.
std::shared_ptr<CarProgramCache> programCache()
{
    static std::weak_ptr<CarProgramCache> shared;
    std::shared_ptr<CarProgramCache> cache = shared.lock();
    if (!cache) {
        cache = std::make_shared<CarProgramCache>();
        shared = cache;
    }
    return cache;
}

} // namespace

CarRenderer::CarRenderer(const carmodel::CarModel& model)
    : m_blurRim(model.blurRim)
    , m_nodes(model.mesh.nodes)
    , m_batches(model.mesh.batches)
    , m_ranges(model.mesh.ranges)
    , m_settings(model.settings)
    , m_tyreSettings(model.tyreSettings)
    , m_black(Texture::solid(0, 0, 0, 0))
    , m_white(Texture::solid(255, 255, 255, 255))
{
    const CarMesh& mesh = model.mesh;
    glCreateBuffers(1, &m_vertices);
    glNamedBufferStorage(m_vertices, static_cast<GLsizeiptr>(std::max<std::size_t>(mesh.vertices.size(), 4)),
        mesh.vertices.empty() ? nullptr : mesh.vertices.data(), 0);
    glCreateBuffers(1, &m_indices);
    glNamedBufferStorage(m_indices, static_cast<GLsizeiptr>(std::max<std::size_t>(mesh.indices.size() * 4, 4)),
        mesh.indices.empty() ? nullptr : mesh.indices.data(), 0);
    const std::vector<std::uint8_t> zeros(kDamageGridBytes, 0);
    glCreateBuffers(1, &m_zeros);
    glNamedBufferStorage(m_zeros, static_cast<GLsizeiptr>(zeros.size()), zeros.data(), 0);
    // The vertex shaders fetch from storage buffers; the array only carries
    // the index buffer.
    glCreateVertexArrays(1, &m_vao);
    glVertexArrayElementBuffer(m_vao, m_indices);

    m_textures.reserve(model.textures.size());
    for (const carmodel::CarModel::Texture& texture : model.textures) {
        const bool colour = texture.chain.format != world::TextureSurface::Format::Bc5;
        m_textures.push_back(Texture::fromMipChain(texture.chain, colour));
    }

    m_cache = programCache();
    std::map<std::string, std::vector<Pass>> byTechnique;
    m_passes.reserve(m_batches.size());
    for (const CarMesh::Batch& batch : m_batches) {
        auto known = byTechnique.find(batch.technique);
        if (known == byTechnique.end()) {
            std::vector<Pass> passes;
            if (model.effect) {
                for (const auto& source : model.effect->passes(batch.technique)) {
                    Pass pass;
                    pass.blend = source->blend;
                    pass.program = m_cache->get(source, batch.technique);
                    if (!pass.program) {
                        passes.clear();
                        break;
                    }
                    auto defaults = std::make_shared<XenosConstants>();
                    pass.program->loadDefaults(*defaults);
                    pass.defaults = std::move(defaults);
                    passes.push_back(std::move(pass));
                }
            }
            known = byTechnique.emplace(batch.technique, std::move(passes)).first;
        }
        m_passes.push_back(known->second);
    }
}

CarRenderer::~CarRenderer()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteBuffers(1, &m_vertices);
    glDeleteBuffers(1, &m_indices);
    glDeleteBuffers(1, &m_zeros);
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

void CarRenderer::fillConstants(XenosConstants& c, const XenosProgram& program, const FrameParams& frame,
    const CarPose& pose, const CarMesh::Batch& batch, const glm::mat4& world) const
{
    const carmodel::CarMesh::Node& node = m_nodes[batch.node];
    // The game's world is ours mirrored in Z (the carbins store the game's
    // coordinates; see CarBin), so its matrices are ours conjugated by that
    // mirror and its vectors ours with z negated.
    const glm::mat4 gameWorld = kMirrorZ * world;
    const glm::mat4 gameView = frame.view * kMirrorZ;
    program.setMatrix(c, "matWVP", frame.projection * gameView * gameWorld);
    program.setMatrix(c, "matWV", gameView * gameWorld);
    program.setMatrix(c, "matW", gameWorld);
    program.setMatrix(c, "matWInvT", glm::inverseTranspose(gameWorld));
    const glm::vec4 eye(glm::vec3(kMirrorZ * glm::vec4(frame.cameraPosition, 1.0F)), 1.0F);
    program.set(c, "c_eyePosWS", eye);
    program.set(c, "c_vsEyePosWS", eye);

    const CarMesh::VertexRange& range = m_ranges[batch.range];
    program.set(c, "pack_partPosition", range.packedPosition);
    program.set(c, "uv1CompressionFactors", batch.uv0);
    program.set(c, "uv2CompressionFactors", batch.uv1);
    program.set(c, "shCompressionFactors", glm::vec4(node.shCompression, 0.0F, 0.0F));
    // x = 0 skips the damage morph fetch: the undamaged shape (0x82E69640
    // sets 1 only for parts with morph data on a damaged car's LOD).
    program.set(c, "vsUseMorph", glm::vec4(0.0F));
    program.set(c, "q_ModelToWorld", modelToWorldQuaternion(gameWorld));
    // The wheel shaders spin and scale the wheel themselves; the node's
    // matrix already does both, so they get the identity: a rotation of
    // (cos 0, sin 0) and a scale of 1 (see the tread and rim_V2 vertex
    // shaders, which multiply the radial and axial coordinates by
    // wheelScale.x and .y and rotate by tireRotation.xy).
    program.set(c, "tireRotation", glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
    program.set(c, "wheelScale", glm::vec4(1.0F, 1.0F, 0.0F, 0.0F));
    // The tyre shaders clamp the height of every vertex, after that
    // rotation, to between tireDeformation.z - .y + 0.001 and .y, flattening
    // the tyre where it meets the ground (tire and tread vertex shaders,
    // slots 26 to 51). How the game fills it from the contact is not traced;
    // bounds of plus and minus the part's box diagonal leave every vertex
    // where the model has it. wheelMorph.z scales a bulge that then stays 0.
    const float reach = range.packedPosition.w;
    program.set(c, "tireDeformation", glm::vec4(0.0F, reach, -reach, 0.0F));
    program.set(c, "wheelMorph", glm::vec4(0.0F));

    const SceneLighting& light = frame.lighting;
    const glm::mat3 toObject = glm::transpose(rotationPart(gameWorld));
    const glm::vec3 up(0.0F, 1.0F, 0.0F);
    const glm::vec3 toSun = mirrorZ(-light.sunDirection);
    const glm::vec4 directional = rotateSh(shDirectional(toSun), toObject);
    program.set(c, "shDirectionalLightI", directional);
    program.set(c, "shHemisphericalLightR",
        rotateSh(shHemisphere(up, light.carAmbientTop.r, light.carAmbientBottom.r), toObject));
    program.set(c, "shHemisphericalLightG",
        rotateSh(shHemisphere(up, light.carAmbientTop.g, light.carAmbientBottom.g), toObject));
    program.set(c, "shHemisphericalLightB",
        rotateSh(shHemisphere(up, light.carAmbientTop.b, light.carAmbientBottom.b), toObject));
    program.set(c, "shDirectionalColour", directionalColour(light.sunColour));

    // Material parameters of the settings files, then the car's paint.
    const ShaderSettings::Parameters* parameters = settingsFor(batch, pose);
    if (parameters != nullptr) {
        for (const auto& [name, value] : *parameters) {
            program.set(c, name, value);
        }
    }
    program.set(c, "PaintColor", glm::vec4(pose.paint, 1.0F));

    c.system.streamBase[kVertexStream] = range.byteOffset / 4;
    c.system.streamBase[kMorphStream] = 0;
    c.system.streamBase[kDamageStream] = 0;
}

bool CarRenderer::wheelBatchVisible(const CarMesh::Batch& batch, const CarPose& pose) const
{
    const CarMesh::Node& node = m_nodes[batch.node];
    if (!batch.motionBlur || !node.corner) {
        return true;
    }
    // The blurred rims stand in for the spinning wheel from GeomBlendStart
    // of MaxRPM on; how the executable blends between the two sets of
    // meshes was not traced, so below that speed the blurred ones are left
    // out and above it drawn with the sharp ones.
    if (!m_blurRim || m_blurRim->maxRpm <= 0.0F) {
        return false;
    }
    constexpr float kRadiansPerSecondToRpm = 60.0F / (2.0F * kPi);
    const float rpm = std::abs(pose.spinRate[static_cast<std::size_t>(*node.corner)]) * kRadiansPerSecondToRpm;
    return rpm / m_blurRim->maxRpm >= m_blurRim->blendStart;
}

const ShaderSettings::Parameters* CarRenderer::settingsFor(const CarMesh::Batch& batch, const CarPose& pose) const
{
    const bool wheel = m_nodes[batch.node].kind == CarMesh::NodeKind::Wheel;
    if (wheel) {
        return m_tyreSettings.technique(batch.technique);
    }
    const auto finish = m_settings.find(pose.metallic ? "Metallic" : "Normal");
    return finish == m_settings.end() ? nullptr : finish->second.technique(batch.technique);
}

void CarRenderer::bindTextures(const XenosProgram& program, const CarMesh::Batch& batch) const
{
    for (const xenos::SamplerBinding& sampler : program.source().samplers) {
        // Samplers without a file are render targets the game draws each
        // frame (reflections, mirrors) and read as black here. The shadow
        // mask is the exception: it holds 1 where nothing shadows the car,
        // and with no shadow pass that is everywhere.
        const Texture* texture = sampler.name == "ShadowMaskSamp" ? &m_white : &m_black;
        for (const auto& [name, index] : batch.textures) {
            if (name == sampler.name && index < m_textures.size()) {
                texture = &m_textures[index];
            }
        }
        if (sampler.dimension == xenos::TextureDimension::Cube) {
            // The environment cube maps are rendered at run time and not
            // produced here; an empty unit reads as black.
            glBindTextureUnit(sampler.unit, 0);
            continue;
        }
        texture->bind(sampler.unit);
    }
}

void CarRenderer::draw(const FrameParams& frame, const CarPose& pose, bool translucent)
{
    m_draws.clear();
    m_stream.clear();
    XenosConstants constants;
    for (const CarMesh::Node& node : m_nodes) {
        const glm::mat4 world = nodeTransform(node, pose) * node.placement;
        // Placements flip the file's Z; one that also mirrors (right-hand
        // wheels without a model of their own) has a positive determinant.
        const bool mirrored = glm::determinant(glm::mat3(world)) > 0.0F;
        for (std::uint32_t b = node.firstBatch; b < node.firstBatch + node.batchCount; ++b) {
            const CarMesh::Batch& batch = m_batches[b];
            if (batch.translucent != translucent || batch.indexCount == 0 || !wheelBatchVisible(batch, pose)) {
                continue;
            }
            for (const Pass& pass : m_passes[b]) {
                constants = *pass.defaults;
                fillConstants(constants, *pass.program, frame, pose, batch, world);
                m_draws.push_back({b, &pass, m_stream.append(constants), mirrored});
            }
        }
    }
    if (m_draws.empty()) {
        return;
    }
    m_stream.upload();

    glBindVertexArray(m_vao);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kVertexStream, m_vertices);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kMorphStream, m_zeros);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kDamageStream, m_zeros);
    // The passes set no culling, leaving the engine's back-face culling.
    // Carbin triangles wind clockwise seen from the side their normals face
    // (see CarBin), as the world's do, so the frame's culling state applies;
    // a mirrored placement reverses them.
    glEnable(GL_CULL_FACE);
    if (translucent) {
        glDepthMask(GL_FALSE);
    }
    for (const Draw& draw : m_draws) {
        const CarMesh::Batch& batch = m_batches[draw.batch];
        applyBlend(draw.pass->blend, translucent);
        glFrontFace(draw.mirrored ? GL_CW : GL_CCW);
        glUseProgram(draw.pass->program->id());
        bindTextures(*draw.pass->program, batch);
        m_stream.bind(draw.constants);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
    }
    glDisable(GL_BLEND);
    glFrontFace(GL_CCW);
    glBindVertexArray(0);
    if (translucent) {
        glDepthMask(GL_TRUE);
    }
}

} // namespace fh::render
