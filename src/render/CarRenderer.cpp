#include "CarRenderer.h"

#include "base/Log.h"
#include "carmodel/CarMaterials.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <tuple>
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

/// The shaders' names of CarRenderer's per-draw constants, in the order of
/// CarRenderer::Constant.
constexpr std::array kConstantNames{"matWVP", "matWV", "matW", "matWInvT", "c_eyePosWS", "c_vsEyePosWS",
    "pack_partPosition", "uv1CompressionFactors", "uv2CompressionFactors", "shCompressionFactors", "vsUseMorph",
    "q_ModelToWorld", "tireRotation", "wheelScale", "tireDeformation", "wheelMorph", "shDirectionalLightI",
    "shHemisphericalLightR", "shHemisphericalLightG", "shHemisphericalLightB", "shDirectionalColour", "PaintColor",
    "dynamicLightsAmount", "dynamicLights2"};

/// Direct3D's alpha comparison "greater or equal", in the translator's
/// alphaTest numbering.
constexpr float kAlphaGreaterEqual = 7.0F;
/// Alpha to mask on the single-sample scene target: a pixel is kept when
/// its alpha reaches one half, as WorldRenderer emulates it for the
/// foliage. Inferred from how the Xbox 360 dithers alpha to coverage; the
/// offsets the car draw leaves in RB_COLORCONTROL were not traced.
constexpr float kAlphaToMaskThreshold = 0.5F;

/// Texture units whose binding draw() tracks, enough for every sampler
/// unit of the translated shaders (pixel s0 to s15, vertex from 16).
constexpr std::size_t kTextureUnits = 32;
/// Stands for a unit whose binding is not known yet: no texture name.
constexpr GLuint kUnknownTexture = ~GLuint{0};
/// Stands in a pass's textures for the shadow mask, bound at draw time.
constexpr GLuint kShadowMaskTexture = ~GLuint{0} - 1;
/// Stand in a pass's textures for the environment cubes, bound at draw
/// time.
constexpr GLuint kDynamicCubeTexture = ~GLuint{0} - 2;
constexpr GLuint kStaticCubeTexture = ~GLuint{0} - 3;

/// The mask CarRenderer::setShadowMask() gave, shared by every car.
GLuint g_shadowMask = 0;
/// The cubes CarRenderer::setEnvironmentCubes() gave, shared by every car.
GLuint g_dynamicCube = 0;
GLuint g_staticCube = 0;

/// reflectionScaler (pixel c52) as the car draw sets it for every pass: the
/// vector at 0x82001120, (1, 1, 1, 1), loaded at 0x82E635C8 and stored at
/// +6848 of the draw's constants (0x82E635E4), which is pixel register 52
/// (+6016 + 16 r, the same layout as dynamicLightsAmount, c58, at +6944
/// and dynamicLights2, c108, at +7744). The effect's default is 0, which
/// takes every environment reflection away.
constexpr glm::vec4 kReflectionScaler{1.0F, 1.0F, 1.0F, 1.0F};

bool sameBlend(const std::optional<carmodel::CarPass::Blend>& a, const std::optional<carmodel::CarPass::Blend>& b)
{
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a || (a->enable == b->enable && a->source == b->source && a->destination == b->destination);
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
            // Written as the pixel shader writes it: the scene target holds
            // square-root colour as the game's does (see FrameRenderer).
            program = std::make_shared<XenosProgram>(source->program, name);
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

    m_boundsCentre = (mesh.boundsMin + mesh.boundsMax) * 0.5F;
    m_boundsRadius = glm::length(mesh.boundsMax - mesh.boundsMin) * 0.5F;
    // Every vertex of a corner part lies within its box's half diagonal of
    // the box's centre, and that centre within the tyre's half diagonal of
    // the hub, the tyre being the corner's largest part; twice the largest
    // half diagonal of a corner part therefore holds the whole corner
    // however the wheel steers, spins or travels on its suspension.
    for (const CarMesh::Batch& batch : m_batches) {
        if (m_nodes[batch.node].corner) {
            m_wheelRadius = std::max(m_wheelRadius, 2.0F * m_ranges[batch.range].packedPosition.w);
        }
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
                    pass.program->set(*defaults, "reflectionScaler", kReflectionScaler);
                    pass.defaults = std::move(defaults);
                    for (const xenos::ConstantBinding& c : pass.program->source().constants) {
                        if (c.name == "useStaticCubeMap" && c.set == xenos::RegisterSet::Bool) {
                            pass.useStaticCubeMap = c.index;
                        }
                    }
                    auto slots = std::make_shared<ConstantSlots>();
                    static_assert(kConstantNames.size() == std::tuple_size_v<ConstantSlots>);
                    for (std::size_t i = 0; i < slots->size(); ++i) {
                        (*slots)[i] = pass.program->floatSlot(kConstantNames[i]);
                    }
                    pass.slots = std::move(slots);
                    passes.push_back(std::move(pass));
                }
            }
            known = byTechnique.emplace(batch.technique, std::move(passes)).first;
        }
        m_passes.push_back(known->second);
        m_casterPasses.push_back(casterPasses(model, casterTechnique(batch, m_nodes[batch.node].kind)));
        const carmodel::MaterialDrawState state = carmodel::materialDrawState(batch.material);
        for (Pass& pass : m_passes.back()) {
            pass.textures = texturesFor(*pass.program, batch);
            pass.cull = state.cull;
            pass.alphaToMask = state.alphaToMask;
            if (!state.blend) {
                pass.blend = carmodel::CarPass::Blend{};
            }
            for (const bool metallic : {false, true}) {
                ParameterSlots& resolved = pass.parameters[metallic ? 1 : 0];
                if (const ShaderSettings::Parameters* parameters = settingsFor(batch, metallic)) {
                    for (const auto& [name, value] : *parameters) {
                        XenosProgram::FloatSlot slot = pass.program->floatSlot(name);
                        if (!slot.empty()) {
                            resolved.emplace_back(std::move(slot), value);
                        }
                    }
                }
            }
        }
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

void CarRenderer::fillConstants(XenosConstants& c, const Pass& pass, const FrameParams& frame, const CarPose& pose,
    const CarMesh::Batch& batch, const glm::mat4& world) const
{
    const ConstantSlots& slots = *pass.slots;
    const auto set = [&](Constant constant, const glm::vec4& value) {
        XenosProgram::set(c, slots[static_cast<std::size_t>(constant)], value);
    };
    const auto setMatrix = [&](Constant constant, const glm::mat4& value) {
        XenosProgram::setMatrix(c, slots[static_cast<std::size_t>(constant)], value);
    };
    const carmodel::CarMesh::Node& node = m_nodes[batch.node];
    // The game's world is ours mirrored in Z (the carbins store the game's
    // coordinates; see CarBin), so its matrices are ours conjugated by that
    // mirror and its vectors ours with z negated.
    const glm::mat4 gameWorld = kMirrorZ * world;
    const glm::mat4 gameView = frame.view * kMirrorZ;
    setMatrix(Constant::WorldViewProjection, frame.projection * gameView * gameWorld);
    setMatrix(Constant::WorldView, gameView * gameWorld);
    setMatrix(Constant::World, gameWorld);
    setMatrix(Constant::WorldInverseTranspose, glm::inverseTranspose(gameWorld));
    const glm::vec4 eye(glm::vec3(kMirrorZ * glm::vec4(frame.cameraPosition, 1.0F)), 1.0F);
    set(Constant::EyePosition, eye);
    set(Constant::VertexEyePosition, eye);

    const CarMesh::VertexRange& range = m_ranges[batch.range];
    set(Constant::PartPosition, range.packedPosition);
    set(Constant::Uv1Compression, batch.uv0);
    set(Constant::Uv2Compression, batch.uv1);
    set(Constant::ShCompression, glm::vec4(node.shCompression, 0.0F, 0.0F));
    // x = 0 skips the damage morph fetch: the undamaged shape (0x82E69640
    // sets 1 only for parts with morph data on a damaged car's LOD).
    set(Constant::UseMorph, glm::vec4(0.0F));
    set(Constant::ModelToWorld, modelToWorldQuaternion(gameWorld));
    // The wheel shaders spin and scale the wheel themselves; the node's
    // matrix already does both, so they get the identity: a rotation of
    // (cos 0, sin 0) and a scale of 1 (see the tread and rim_V2 vertex
    // shaders, which multiply the radial and axial coordinates by
    // wheelScale.x and .y and rotate by tireRotation.xy).
    set(Constant::TireRotation, glm::vec4(1.0F, 0.0F, 0.0F, 0.0F));
    set(Constant::WheelScale, glm::vec4(1.0F, 1.0F, 0.0F, 0.0F));
    // The tyre shaders clamp the height of every vertex, after that
    // rotation, to between tireDeformation.z - .y + 0.001 and .y, flattening
    // the tyre where it meets the ground (tire and tread vertex shaders,
    // slots 26 to 51). How the game fills it from the contact is not traced;
    // bounds of plus and minus the part's box diagonal leave every vertex
    // where the model has it. wheelMorph.z scales a bulge that then stays 0.
    const float reach = range.packedPosition.w;
    set(Constant::TireDeformation, glm::vec4(0.0F, reach, -reach, 0.0F));
    set(Constant::WheelMorph, glm::vec4(0.0F));

    const SceneLighting& light = frame.lighting;
    const glm::mat3 toObject = glm::transpose(rotationPart(gameWorld));
    const glm::vec3 up(0.0F, 1.0F, 0.0F);
    const glm::vec3 toSun = mirrorZ(-light.sunDirection);
    const glm::vec4 directional = rotateSh(shDirectional(toSun), toObject);
    set(Constant::ShDirectionalLight, directional);
    set(Constant::ShHemisphereRed,
        rotateSh(shHemisphere(up, light.carAmbientTop.r, light.carAmbientBottom.r), toObject));
    set(Constant::ShHemisphereGreen,
        rotateSh(shHemisphere(up, light.carAmbientTop.g, light.carAmbientBottom.g), toObject));
    set(Constant::ShHemisphereBlue,
        rotateSh(shHemisphere(up, light.carAmbientTop.b, light.carAmbientBottom.b), toObject));
    set(Constant::ShDirectionalColour, directionalColour(light.sunColour));

    // Material parameters of the settings files, then the car's paint.
    for (const auto& [slot, value] : pass.parameters[pose.metallic ? 1 : 0]) {
        XenosProgram::set(c, slot, value);
    }
    set(Constant::PaintColour, glm::vec4(pose.paint, 1.0F));
    // The lamps, as the car draw sets them for every pass (0x824491B8-
    // 0x824493FC) from the car presentation's lamp setters (vtable
    // 0x82255BFC, slots 60-65, identified by their order and argument
    // counts against the deferred commands at 0x82E7E368-0x82E7E3F0):
    // dynamicLightsAmount holds the brake, reverse and left and right
    // indicator amounts, dynamicLights2 the headlamp amount, a light-state
    // bit, the daytime running lamp amount and another bit. The lamp lenses
    // add BrakeColor times dynamicLightsAmount.x. This game drives only the
    // brake and headlamp amounts (CarPose); the reverse lamps, indicators,
    // running lamps and light-state bits have no source here and stay 0.
    set(Constant::LampAmounts, glm::vec4(pose.brakeLights, 0.0F, 0.0F, 0.0F));
    set(Constant::HeadLampAmounts, glm::vec4(pose.headLights, 0.0F, 0.0F, 0.0F));
    if (pass.useStaticCubeMap) {
        c.control.setBool(*pass.useStaticCubeMap, pose.useStaticCubeMap);
    }

    if (pass.alphaToMask) {
        c.system.alphaTest = {kAlphaGreaterEqual, kAlphaToMaskThreshold, 0.0F, 0.0F};
    }

    c.system.streamBase[kVertexStream] = range.byteOffset / 4;
    c.system.streamBase[kMorphStream] = 0;
    c.system.streamBase[kDamageStream] = 0;
}

bool CarRenderer::inView(const FrameParams& frame, const CarPose& pose) const
{
    // The frustum's side planes and the plane through the eye facing along
    // the view (w = 0), from the rows of the view-projection matrix; unlike
    // the near and far planes they do not depend on the depth convention
    // (reversed, infinite far plane).
    const glm::mat4 viewProjection = frame.projection * frame.view;
    const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
    const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
    const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);
    const std::array<glm::vec4, 5> planes{row3 + row0, row3 - row0, row3 + row1, row3 - row1, row3};
    const auto outside = [&](const glm::vec3& centre, float radius) {
        for (const glm::vec4& plane : planes) {
            const float length = glm::length(glm::vec3(plane));
            if (length > 0.0F && glm::dot(glm::vec3(plane), centre) + plane.w < -radius * length) {
                return true;
            }
        }
        return false;
    };
    // Car space is not scaled, so radii carry over to world space.
    if (!outside(glm::vec3(pose.body * glm::vec4(m_boundsCentre, 1.0F)), m_boundsRadius)) {
        return true;
    }
    for (const glm::vec3& hub : pose.hubs) {
        if (m_wheelRadius > 0.0F && !outside(glm::vec3(pose.body * glm::vec4(hub, 1.0F)), m_wheelRadius)) {
            return true;
        }
    }
    return false;
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

const ShaderSettings::Parameters* CarRenderer::settingsFor(const CarMesh::Batch& batch, bool metallic) const
{
    const bool wheel = m_nodes[batch.node].kind == CarMesh::NodeKind::Wheel;
    if (wheel) {
        return m_tyreSettings.technique(batch.technique);
    }
    const auto finish = m_settings.find(metallic ? "Metallic" : "Normal");
    return finish == m_settings.end() ? nullptr : finish->second.technique(batch.technique);
}

std::vector<std::pair<GLuint, GLuint>> CarRenderer::texturesFor(
    const XenosProgram& program, const CarMesh::Batch& batch) const
{
    std::vector<std::pair<GLuint, GLuint>> units;
    for (const xenos::SamplerBinding& sampler : program.source().samplers) {
        // Samplers without a file are render targets the game draws each
        // frame (mirrors) and read as black here. The shadow mask holds 1
        // where nothing shadows the car, and with no shadow pass that is
        // everywhere; the environment cubes are bound at draw time.
        const Texture* texture = &m_black;
        for (const auto& [name, index] : batch.textures) {
            if (name == sampler.name && index < m_textures.size()) {
                texture = &m_textures[index];
            }
        }
        GLuint id = texture->id();
        if (sampler.name == "envSampler") {
            id = kDynamicCubeTexture;
        } else if (sampler.name == "envStaticSampler") {
            id = kStaticCubeTexture;
        } else if (sampler.dimension == xenos::TextureDimension::Cube) {
            // No other cube the car effect samples is produced; an empty
            // unit reads as black.
            id = 0;
        } else if (sampler.name == "ShadowMaskSamp") {
            id = kShadowMaskTexture;
        }
        units.emplace_back(sampler.unit, id);
    }
    return units;
}

void CarRenderer::draw(const FrameParams& frame, const CarPose& pose, bool translucent)
{
    m_draws.clear();
    m_stream.clear();
    // Traffic spawns all round the player; most of it is off screen, and
    // sending its hundred or so draws would cost the CPU as much as a car
    // in view.
    if (!inView(frame, pose)) {
        return;
    }
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
                const std::size_t constants = m_stream.append(
                    *pass.defaults, [&](XenosConstants& c) { fillConstants(c, pass, frame, pose, batch, world); });
                m_draws.push_back({b, &pass, constants, mirrored});
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
    // A car's draws mostly repeat the state of the one before (the same
    // program, blending and textures across a node's batches), and the
    // driver's cost is in validating changed state at each draw, so only
    // what changes is set.
    const std::optional<carmodel::CarPass::Blend>* blend = nullptr;
    std::optional<bool> mirrored;
    bool culled = true;
    GLuint program = 0;
    std::array<GLuint, kTextureUnits> bound{};
    bound.fill(kUnknownTexture);
    for (const Draw& draw : m_draws) {
        const CarMesh::Batch& batch = m_batches[draw.batch];
        if (blend == nullptr || !sameBlend(*blend, draw.pass->blend)) {
            applyBlend(draw.pass->blend, translucent);
            blend = &draw.pass->blend;
        }
        if (culled != draw.pass->cull) {
            if (draw.pass->cull) {
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
            }
            culled = draw.pass->cull;
        }
        if (mirrored != draw.mirrored) {
            glFrontFace(draw.mirrored ? GL_CW : GL_CCW);
            mirrored = draw.mirrored;
        }
        if (program != draw.pass->program->id()) {
            program = draw.pass->program->id();
            glUseProgram(program);
        }
        for (auto [unit, texture] : draw.pass->textures) {
            // The mask holds 1 where nothing shadows the car, as white does
            // while there is no shadow pass.
            if (texture == kShadowMaskTexture) {
                texture = g_shadowMask != 0 ? g_shadowMask : m_white.id();
            } else if (texture == kDynamicCubeTexture) {
                texture = g_dynamicCube;
            } else if (texture == kStaticCubeTexture) {
                texture = g_staticCube;
            }
            if (unit >= bound.size() || bound[unit] != texture) {
                glBindTextureUnit(unit, texture);
                if (unit < bound.size()) {
                    bound[unit] = texture;
                }
            }
        }
        m_stream.bind(draw.constants);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
    }
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glBindVertexArray(0);
    if (translucent) {
        glDepthMask(GL_TRUE);
    }
}

void CarRenderer::setShadowMask(GLuint texture)
{
    g_shadowMask = texture;
}

void CarRenderer::setEnvironmentCubes(GLuint dynamicCube, GLuint staticCube)
{
    g_dynamicCube = dynamicCube;
    g_staticCube = staticCube;
}

std::optional<glm::vec4> CarRenderer::pixelConstant(std::string_view name) const
{
    for (const std::vector<Pass>& passes : m_passes) {
        for (const Pass& pass : passes) {
            const xenos::ConstantBinding* c = pass.program->constant(name, xenos::ShaderStage::Pixel);
            if (c != nullptr && c->set == xenos::RegisterSet::Float4 && c->index < pass.defaults->pixel.size()) {
                return pass.defaults->pixel[c->index];
            }
        }
    }
    return std::nullopt;
}

GLuint CarRenderer::boundTexture(std::string_view sampler) const
{
    for (const std::vector<Pass>& passes : m_passes) {
        for (const Pass& pass : passes) {
            for (const xenos::SamplerBinding& s : pass.program->source().samplers) {
                if (s.name != sampler) {
                    continue;
                }
                for (const auto& [unit, texture] : pass.textures) {
                    if (unit != s.unit) {
                        continue;
                    }
                    if (texture == kDynamicCubeTexture) {
                        return g_dynamicCube;
                    }
                    if (texture == kStaticCubeTexture) {
                        return g_staticCube;
                    }
                    if (texture == kShadowMaskTexture) {
                        return g_shadowMask;
                    }
                    return texture;
                }
            }
        }
    }
    return 0;
}

std::string CarRenderer::casterTechnique(const CarMesh::Batch& batch, CarMesh::NodeKind node)
{
    if (batch.motionBlur || carmodel::castsNoShadow(batch.material)) {
        return {};
    }
    if (batch.material == "badge") {
        return "shadowMap_badge";
    }
    if (carmodel::isTyreMaterial(batch.material)) {
        return "tireShadowMap";
    }
    if (carmodel::isRimMaterial(batch.material)) {
        return "rimShadowMap";
    }
    if (node == CarMesh::NodeKind::Rotor || node == CarMesh::NodeKind::Caliper) {
        return "brakeShadowMap";
    }
    return "shadowMap";
}

std::vector<CarRenderer::Pass> CarRenderer::casterPasses(const carmodel::CarModel& model, const std::string& technique)
{
    std::vector<Pass> passes;
    if (technique.empty() || !model.effect) {
        return passes;
    }
    for (const auto& source : model.effect->passes(technique)) {
        Pass pass;
        pass.program = m_cache->get(source, technique);
        if (!pass.program) {
            return {};
        }
        auto defaults = std::make_shared<XenosConstants>();
        pass.program->loadDefaults(*defaults);
        pass.defaults = std::move(defaults);
        auto slots = std::make_shared<ConstantSlots>();
        for (std::size_t i = 0; i < slots->size(); ++i) {
            (*slots)[i] = pass.program->floatSlot(kConstantNames[i]);
        }
        pass.slots = std::move(slots);
        passes.push_back(std::move(pass));
    }
    return passes;
}

bool CarRenderer::inBox(const glm::mat4& viewProjection, const CarPose& pose, bool orthographic) const
{
    const auto row = [&](int i) {
        return glm::vec4(viewProjection[0][i], viewProjection[1][i], viewProjection[2][i], viewProjection[3][i]);
    };
    std::array<glm::vec4, 6> planes{row(3) + row(0), row(3) - row(0), row(3) + row(1), row(3) - row(1), row(3), row(3)};
    std::size_t count = 5;
    if (orthographic) {
        planes[4] = row(2);
        planes[5] = row(3) - row(2);
        count = 6;
    }
    const glm::vec3 centre(pose.body * glm::vec4(m_boundsCentre, 1.0F));
    // The wheels lie within the body's sphere widened by their reach.
    const float radius = m_boundsRadius + m_wheelRadius;
    for (std::size_t i = 0; i < count; ++i) {
        const float length = glm::length(glm::vec3(planes[i]));
        if (length > 0.0F && glm::dot(glm::vec3(planes[i]), centre) + planes[i].w < -radius * length) {
            return false;
        }
    }
    return true;
}

void CarRenderer::drawDepth(const glm::mat4& viewProjection, const CarPose& pose, bool screen)
{
    if (!inBox(viewProjection, pose, !screen)) {
        return;
    }
    m_draws.clear();
    m_stream.clear();
    // fillConstants() multiplies projection, view and world with the Z
    // mirror between view and world twice, which cancels; an identity view
    // leaves the projection to carry the whole view-projection.
    FrameParams frame;
    frame.view = glm::mat4(1.0F);
    frame.projection = viewProjection;
    // The shadow map vertex shaders read only the matrices and the part's
    // packing, so batches of one part of one node under one program share
    // their constants.
    std::map<std::tuple<const XenosConstants*, std::uint32_t, std::uint32_t>, std::size_t> shared;
    for (std::uint32_t n = 0; n < m_nodes.size(); ++n) {
        const CarMesh::Node& node = m_nodes[n];
        // The cockpit lies inside the body and its glass, which cast in
        // front of it from any light, so it adds nothing to a shadow map.
        if (node.kind == CarMesh::NodeKind::Cockpit) {
            continue;
        }
        const glm::mat4 world = nodeTransform(node, pose) * node.placement;
        for (std::uint32_t b = node.firstBatch; b < node.firstBatch + node.batchCount; ++b) {
            const CarMesh::Batch& batch = m_batches[b];
            if (batch.indexCount == 0 || (screen && batch.translucent) || !wheelBatchVisible(batch, pose)) {
                continue;
            }
            for (const Pass& pass : m_casterPasses[b]) {
                const auto key = std::tuple(pass.defaults.get(), n, batch.range);
                auto known = shared.find(key);
                if (known == shared.end()) {
                    const std::size_t constants = m_stream.append(
                        *pass.defaults, [&](XenosConstants& c) { fillConstants(c, pass, frame, pose, batch, world); });
                    known = shared.emplace(key, constants).first;
                }
                m_draws.push_back({b, &pass, known->second, false});
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
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    // Draws sharing constants bind them once.
    std::sort(m_draws.begin(), m_draws.end(), [](const Draw& a, const Draw& b) {
        return std::tie(a.pass->program, a.constants, a.batch) < std::tie(b.pass->program, b.constants, b.batch);
    });
    GLuint program = 0;
    std::size_t bound = ~std::size_t{0};
    for (const Draw& draw : m_draws) {
        const CarMesh::Batch& batch = m_batches[draw.batch];
        if (program != draw.pass->program->id()) {
            program = draw.pass->program->id();
            glUseProgram(program);
        }
        if (bound != draw.constants) {
            m_stream.bind(draw.constants);
            bound = draw.constants;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(batch.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(batch.firstIndex) * sizeof(std::uint32_t)));
    }
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
}

} // namespace fh::render
