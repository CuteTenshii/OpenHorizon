#include "TrackPrograms.h"

#include "WorldRenderer.h"

#include "base/Error.h"
#include "base/Log.h"
#include "world/TrackSettings.h"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace openhorizon::render {

void TrackMaterial::setModel(const glm::mat4& matrix)
{
    const glm::mat4 inverse = glm::affineInverse(matrix);
    for (int i = 0; i < 3; ++i) {
        model[static_cast<std::size_t>(i)] = glm::row(matrix, i);
        modelInverse[static_cast<std::size_t>(i)] = glm::row(inverse, i);
    }
}

std::optional<std::uint32_t> materialSlot(std::string_view name, xenos::ShaderStage stage)
{
    // Vertex shaders name them "vc360_N" and a few (bunting) "c360_N".
    std::string_view prefix = "c360_";
    if (stage == xenos::ShaderStage::Vertex && name.starts_with("vc360_")) {
        prefix = "vc360_";
    }
    if (!name.starts_with(prefix)) {
        return std::nullopt;
    }
    std::uint32_t number = 0;
    const char* first = name.data() + prefix.size();
    const char* last = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(first, last, number);
    if (ec != std::errc() || ptr != last) {
        return std::nullopt;
    }
    if (stage == xenos::ShaderStage::Vertex) {
        if (number < world::ShadedMesh::kFirstVertexRegister) {
            return std::nullopt;
        }
        number = number - world::ShadedMesh::kFirstVertexRegister
            + static_cast<std::uint32_t>(world::ShadedMesh::kFirstVertexConstant);
    } else if (number >= world::ShadedMesh::kFirstVertexConstant) {
        return std::nullopt;
    }
    if (number >= TrackMaterial::kConstants) {
        return std::nullopt;
    }
    return number;
}

namespace {

constexpr std::size_t kBlockBytes = 256 * sizeof(glm::vec4);

/// The scene constants and the registers every track effect declaring them
/// uses (verified over all 173 Colorado effects; SunGainScale shares c214
/// with FogConsts2 in the six effects that declare it and is left alone).
struct FrameName {
    std::string_view name;
    std::uint32_t reg = 0;
};
constexpr std::array kFrameNames{
    FrameName{"WorldViewProjMatrix", 128},
    FrameName{"wvp", 128},
    FrameName{"worldI", 132},
    FrameName{"WorldInvTransMatrix", 136},
    FrameName{"worldIT", 136},
    FrameName{"WorldMatrix", 140},
    FrameName{"world", 140},
    FrameName{"CamPosWorld", 158},
    FrameName{"CamPos", 158},
    FrameName{"skyColor", 196},
    FrameName{"groundColor", 197},
    FrameName{"EmissiveSwitchOnThreshold", 213},
    FrameName{"FogConsts2", 214},
    FrameName{"FogColor2", 215},
    FrameName{"ambColor", 221},
    FrameName{"SkyScale", 222},
    FrameName{"RoadSpecularParams2", 224},
    FrameName{"FogConsts", 241},
    FrameName{"FogColor", 242},
    FrameName{"V2LightmapColor1", 247},
    FrameName{"V2LightmapColor2", 248},
    FrameName{"TimeGain", 249},
    FrameName{"HackMatrix", 208},
    FrameName{"ViewInvMatrix", 243},
    FrameName{"viewInv", 243},
    FrameName{"sunDir", 250},
    FrameName{"sunColor", 251},
};

struct Redirects {
    std::set<std::uint32_t> frame;
    std::map<std::uint32_t, std::uint32_t> material;
};

Redirects redirectsOf(const xenos::ShaderProgramSource& source, xenos::ShaderStage stage)
{
    Redirects r;
    for (const xenos::ConstantBinding& c : source.constants) {
        if (c.stage != stage || c.set != xenos::RegisterSet::Float4) {
            continue;
        }
        if (const std::optional<std::uint32_t> number = materialSlot(c.name, stage)) {
            r.material.emplace(c.index, *number);
            continue;
        }
        for (const FrameName& frame : kFrameNames) {
            if (frame.name == c.name && frame.reg == c.index) {
                for (std::uint32_t k = 0; k < c.count; ++k) {
                    r.frame.insert(c.index + k);
                }
            }
        }
    }
    return r;
}

/// Varying location of the batch number the vertex stage passes on with
/// bindless textures, after the translator's 16 exports.
constexpr int kBatchLocation = 16;

/// The material declarations: one block per batch, or with bindless
/// textures every material of the mesh and the texture handle table.
std::string materialDeclarations(bool bindless, xenos::ShaderStage stage)
{
    if (!bindless) {
        return std::format("layout(std140, binding = {}) uniform XeMaterialConstants\n{{\n    vec4 c[{}];\n    uvec4 "
                           "base;\n    vec4 model[3];\n    vec4 modelInverse[3];\n}} xe_material;\n",
            trackbinding::kMaterial, TrackMaterial::kConstants);
    }
    std::string d = std::format("struct XeMaterial\n{{\n    vec4 c[{}];\n    uvec4 base;\n    vec4 model[3];\n    vec4 "
                                "modelInverse[3];\n    uvec4 tex[4];\n}};\n",
        TrackMaterial::kConstants);
    d += std::format(
        "layout(std430, binding = {}) readonly buffer XeMaterials\n{{\n    XeMaterial xe_materials[];\n}};\n",
        trackbinding::kMaterial);
    d += std::format(
        "layout(std430, binding = {}) readonly buffer XeTextureHandles\n{{\n    uvec2 xe_handles[];\n}};\n",
        trackbinding::kTextureHandles);
    if (stage == xenos::ShaderStage::Vertex) {
        d += std::format(
            "layout(location = {}) flat out uint xe_batchOut;\n#define xe_batch uint(gl_BaseInstanceARB)\n",
            kBatchLocation);
    } else {
        d += std::format(
            "layout(location = {}) flat in uint xe_batchIn;\n#define xe_batch xe_batchIn\n", kBatchLocation);
    }
    d += "#define xe_material xe_materials[xe_batch]\n";
    d += std::format(
        "uvec2 xe_handle(uint reg, uint engine)\n{{\n    uint entry = xe_material.tex[reg >> 2u][reg & 3u];\n"
        "    return xe_handles[entry == {}u ? engine : entry];\n}}\n",
        TrackMaterial::kEngineTextureEntry);
    return d;
}

/// Turns the translator's texture units into samplers built from the handle
/// table: a pixel sampler, 2D or cube, takes its batch's texture for its
/// register, or the engine's; vertex and volume samplers always take the
/// engine's, as WorldRenderer binds them without bindless textures.
std::string bindlessSamplers(
    std::string glsl, const std::vector<xenos::SamplerBinding>& samplers, xenos::ShaderStage stage)
{
    constexpr std::string_view kVersion = "#version 450 core\n";
    if (!glsl.starts_with(kVersion)) {
        throw DataError("generated shader does not start with {}", kVersion);
    }
    std::string extensions = "#extension GL_ARB_bindless_texture : require\n";
    if (stage == xenos::ShaderStage::Vertex) {
        extensions += "#extension GL_ARB_shader_draw_parameters : require\n";
    }
    glsl.insert(kVersion.size(), extensions);
    for (const xenos::SamplerBinding& s : samplers) {
        if (s.stage != stage) {
            continue;
        }
        const std::string declared = std::format(" {};", s.glslName);
        const std::size_t name = glsl.find(declared);
        if (name == std::string::npos) {
            continue;
        }
        const std::size_t lineStart = glsl.rfind('\n', name) + 1;
        const std::size_t lineEnd = glsl.find('\n', name);
        const char* type = s.dimension == xenos::TextureDimension::D3 ? "sampler3D"
            : s.dimension == xenos::TextureDimension::Cube            ? "samplerCube"
                                                                      : "sampler2D";
        std::string handle;
        if (s.dimension == xenos::TextureDimension::Cube) {
            // A pixel cube sampler takes its batch's cube map (the track's
            // own, see WorldRenderer), black where it has none.
            handle = stage == xenos::ShaderStage::Pixel && s.unit < TrackMaterial{}.textures.size()
                ? std::format("xe_handle({}u, {}u)", s.unit, texturehandle::kBlackCube)
                : std::format("xe_handles[{}u]", texturehandle::kBlackCube);
        } else if (s.dimension == xenos::TextureDimension::D3) {
            handle = std::format("xe_handles[{}u]", texturehandle::kBlack3D);
        } else if (stage != xenos::ShaderStage::Pixel || s.unit >= TrackMaterial{}.textures.size()) {
            handle = std::format("xe_handles[{}u]", texturehandle::kBlack);
        } else {
            const std::uint32_t engine
                = s.name == "ShadowMaskSamp" ? texturehandle::kShadowMask : texturehandle::kBlack;
            handle = std::format("xe_handle({}u, {}u)", s.unit, engine);
        }
        glsl.replace(lineStart, lineEnd - lineStart, std::format("#define {} {}({})", s.glslName, type, handle));
    }
    if (stage == xenos::ShaderStage::Vertex) {
        constexpr std::string_view kMain = "void main()\n{\n";
        const std::size_t main = glsl.find(kMain);
        if (main == std::string::npos) {
            throw DataError("generated vertex shader has no main function");
        }
        glsl.insert(main + kMain.size(), "    xe_batchOut = xe_batch;\n");
    }
    return glsl;
}

/// The vertex shader drawing an instance per face of a layered cube target
/// (see the constructor): the face's transform applied to the clip
/// position the translated shader computes, and the face's layer.
std::string layeredCubeVertex(const std::string& glsl)
{
    constexpr std::string_view kPosition
        = "    gl_Position = vec4(xe_oPos.xyz * xe_clipScale.xyz + xe_oPos.w * xe_clipOffset.xyz, xe_oPos.w);\n";
    const std::size_t position = glsl.find(kPosition);
    const std::size_t version = glsl.find('\n');
    if (position == std::string::npos || version == std::string::npos || !glsl.starts_with("#version")) {
        throw DataError("the translated vertex shader has no clip position to place on a cube face");
    }
    std::string out = glsl.substr(0, version + 1);
    out += "#extension GL_ARB_shader_viewport_layer_array : require\n";
    out += std::format("layout(std140, binding = {}) uniform XeCubeFaces\n{{\n    mat4 xe_faceTransform[6];\n"
                       "    ivec4 xe_faceLayer[6];\n}};\n",
        trackbinding::kCubeFaces);
    const std::size_t end = position + kPosition.size();
    out += glsl.substr(version + 1, end - version - 1);
    out += "    gl_Position = xe_faceTransform[gl_InstanceID] * gl_Position;\n";
    out += "    gl_Layer = xe_faceLayer[gl_InstanceID].x;\n";
    out += glsl.substr(end);
    return out;
}

/// The first and last frame registers of the matrices a vertex stage
/// drawing in a model's space composes with the batch's model matrix: wvp,
/// worldI, worldIT and world (WorldViewProjMatrix, WorldInvTransMatrix and
/// WorldMatrix in some effects; see TrackFrameConstants::make()).
constexpr std::uint32_t kFirstModelRegister = 128;
constexpr std::uint32_t kLastModelRegister = 143;

/// Rows of the frame's matrices times the batch's model matrix
/// (TrackMaterial::model): row i of F·M is F's row i applied to M's rows;
/// row i of (F·M)⁻¹ = M⁻¹·F⁻¹ is M⁻¹'s row i applied to F⁻¹'s rows, of
/// which the frame block holds 0 to 2 (worldI); row i of its transpose is
/// its column i. The last rows of the inverses are an affine transform's.
constexpr std::string_view kModelRows = R"(vec4 xe_modelTimes(vec4 row)
{
    return row.x * xe_material.model[0] + row.y * xe_material.model[1] + row.z * xe_material.model[2]
        + vec4(0.0, 0.0, 0.0, row.w);
}
vec4 xe_modelInverseRow(uint i)
{
    vec4 n = i < 3u ? xe_material.modelInverse[min(i, 2u)] : vec4(0.0, 0.0, 0.0, 1.0);
    return n.x * xe_frame.c[132] + n.y * xe_frame.c[133] + n.z * xe_frame.c[134] + vec4(0.0, 0.0, 0.0, n.w);
}
vec4 xe_modelRow(uint reg)
{
    uint i = reg & 3u;
    if (reg < 132u || reg >= 140u) {
        return xe_modelTimes(xe_frame.c[reg]);
    }
    if (reg < 136u) {
        return xe_modelInverseRow(i);
    }
    return vec4(xe_modelInverseRow(0u)[i], xe_modelInverseRow(1u)[i], xe_modelInverseRow(2u)[i], i == 3u ? 1.0 : 0.0);
}
)";

/// Points the generated code's reads of the redirected registers at the
/// frame and material blocks; with `modelSpace` (a vertex stage drawing in
/// a model's space, see TrackMaterial), the reads of the frame's matrices at
/// their products with the batch's model matrix.
std::string redirect(
    const std::string& glsl, const Redirects& redirects, bool bindless, xenos::ShaderStage stage, bool modelSpace)
{
    constexpr std::string_view kBlockEnd = "} xe_consts;\n";
    constexpr std::string_view kRead = "xe_consts.c[";
    std::string out;
    out.reserve(glsl.size() + 256);
    const std::size_t blockEnd = glsl.find(kBlockEnd);
    if (blockEnd == std::string::npos) {
        throw DataError("generated shader has no constant block");
    }
    std::size_t at = 0;
    const auto copyUntil = [&](std::size_t end) {
        while (at < end) {
            const std::size_t read = glsl.find(kRead, at);
            if (read == std::string::npos || read >= end) {
                out.append(glsl, at, end - at);
                at = end;
                return;
            }
            out.append(glsl, at, read - at);
            const std::size_t digits = read + kRead.size();
            std::size_t close = digits;
            while (close < glsl.size() && glsl[close] >= '0' && glsl[close] <= '9') {
                ++close;
            }
            std::uint32_t reg = 0;
            const bool direct = close > digits && close < glsl.size() && glsl[close] == ']'
                && std::from_chars(glsl.data() + digits, glsl.data() + close, reg).ec == std::errc();
            if (direct && redirects.frame.contains(reg)) {
                out += modelSpace && reg >= kFirstModelRegister && reg <= kLastModelRegister
                    ? std::format("xe_modelRow({}u)", reg)
                    : std::format("xe_frame.c[{}]", reg);
                at = close + 1;
            } else if (const auto m = redirects.material.find(reg); direct && m != redirects.material.end()) {
                out += std::format("xe_material.c[{}]", m->second);
                at = close + 1;
            } else {
                out.append(kRead);
                at = digits;
            }
        }
    };
    copyUntil(blockEnd + kBlockEnd.size());
    out += std::format("layout(std140, binding = {}) uniform XeFrameConstants\n{{\n    vec4 c[256];\n}} xe_frame;\n",
        trackbinding::kFrame);
    out += materialDeclarations(bindless, stage);
    if (modelSpace) {
        out += kModelRows;
    }
    copyUntil(glsl.size());
    // The batch's first vertex, in 32-bit words of the tile's vertex
    // buffer, comes with its material block, so the whole buffer stays
    // bound for every batch of a tile.
    constexpr std::string_view kStreamBase = "xe_streamBase[0u][0u]";
    for (std::size_t use = out.find(kStreamBase); use != std::string::npos; use = out.find(kStreamBase, use)) {
        out.replace(use, kStreamBase.size(), "xe_material.base.x");
    }
    return out;
}

void loadDefaults(const xenos::ShaderProgramSource& source, xenos::ShaderStage stage, std::array<glm::vec4, 256>& block)
{
    for (const xenos::ConstantBinding& c : source.constants) {
        if (c.stage != stage || c.set != xenos::RegisterSet::Float4 || c.defaultValue.empty()) {
            continue;
        }
        for (std::size_t k = 0; k * 4 < c.defaultValue.size() && c.index + k < block.size(); ++k) {
            glm::vec4& v = block[c.index + k];
            for (std::size_t i = 0; i < 4 && k * 4 + i < c.defaultValue.size(); ++i) {
                v[static_cast<glm::length_t>(i)] = c.defaultValue[k * 4 + i];
            }
        }
    }
    const std::vector<xenos::LiteralConstant>& literals
        = stage == xenos::ShaderStage::Vertex ? source.vertexLiterals : source.pixelLiterals;
    for (const xenos::LiteralConstant& literal : literals) {
        if (literal.reg < block.size()) {
            block[literal.reg] = glm::vec4(literal.value[0], literal.value[1], literal.value[2], literal.value[3]);
        }
    }
}

glm::vec4 row(const glm::mat4& m, int i)
{
    return glm::row(m, i);
}

} // namespace

glm::vec4 TrackFrameConstants::timeGain(float presentationSeconds)
{
    const float clock
        = presentationSeconds - kPresentationClockWrap * std::floor(presentationSeconds / kPresentationClockWrap);
    return {clock, 0.0F, 1.0F, 1.0F};
}

glm::vec4 TrackFrameConstants::emissiveSwitchOn(float switchOnLights, float lightmapScale, float presentationSeconds)
{
    const float lights = switchOnLights < kSwitchOnLightsFloor ? 0.0F : switchOnLights;
    const float threshold = lights == 0.0F ? -1.0F : lights;
    const float lightmap = std::min(std::max(threshold - 0.5F, 0.0F) * 4.0F, 1.0F) * lightmapScale;
    return {threshold, lightmap, 1.0F, timeGain(presentationSeconds).x};
}

TrackFrameConstants TrackFrameConstants::make(const glm::mat4& viewProjection, const glm::mat4& inverseView,
    const glm::vec3& camera, const SceneLighting& lighting, float presentationSeconds, const glm::mat4& model,
    const world::TrackSettings* settings)
{
    TrackFrameConstants f;
    // The shaders compute dp4(register, position) per clip component, so
    // register i holds row i of the matrix.
    const glm::mat4 wvp = viewProjection * model;
    const glm::mat4 inverseModel = glm::inverse(model);
    const glm::mat4 inverseTransposeModel = glm::transpose(inverseModel);
    for (int i = 0; i < 4; ++i) {
        f.c[128 + static_cast<std::size_t>(i)] = row(wvp, i);
        f.c[140 + static_cast<std::size_t>(i)] = row(model, i);
    }
    for (int i = 0; i < 3; ++i) {
        f.c[132 + static_cast<std::size_t>(i)] = row(inverseModel, i);
        f.c[136 + static_cast<std::size_t>(i)] = row(inverseTransposeModel, i);
    }
    // The inverse of the game's Direct3D view: its columns are the camera's
    // right, up and forward axes and its position, of which the effects
    // read only the position (the w of each register). The OpenGL view
    // looks down -Z, hence the negated third axis.
    const glm::vec3 right(inverseView[0]);
    const glm::vec3 up(inverseView[1]);
    const glm::vec3 forward = -glm::vec3(inverseView[2]);
    for (int i = 0; i < 3; ++i) {
        f.c[243 + static_cast<std::size_t>(i)] = glm::vec4(right[i], up[i], forward[i], camera[i]);
    }
    f.c[158] = glm::vec4(camera, 1.0F);
    // The effects light with dot(normal, sunDir): it points towards the sun
    // (the constant table's default is straight up).
    f.c[250] = glm::vec4(-lighting.sunDirection, 0.0F);
    f.c[251] = glm::vec4(lighting.sunColour, 1.0F);
    f.c[221] = glm::vec4(lighting.ambientColour, 1.0F);
    f.c[196] = glm::vec4(lighting.ambientTop, 1.0F);
    f.c[197] = glm::vec4(lighting.ambientBottom, 1.0F);
    // The fog writer at 0x82429780.
    f.c[241] = glm::vec4(lighting.fogStart, 0.0F, 0.0F, lighting.fogDensity);
    f.c[242] = glm::vec4(lighting.fogColour, lighting.fogScatterStart);
    f.c[214] = glm::vec4(
        lighting.fogHeightFadeStart, lighting.fogHeightFadeEnd, lighting.fogHeightFadeAmount, lighting.fogScatterPower);
    f.c[215] = glm::vec4(lighting.fogScatterColour, lighting.fogScatterRamp);
    f.c[249] = timeGain(presentationSeconds);
    f.c[213] = emissiveSwitchOn(
        lighting.switchOnLights, settings != nullptr ? settings->trackLightMap.z : 0.0F, presentationSeconds);
    // The scene writer's uploads from the track's settings (0x824442AC,
    // 0x824443CC, 0x82444620, 0x82444680; see world::TrackSettings).
    if (settings != nullptr) {
        f.c[222] = settings->skyScale;
        f.c[224] = settings->roadSpecular2;
        f.c[247] = settings->darkColor;
        f.c[248] = settings->lightColor;
    }
    return f;
}

void TrackFrameConstants::setHackMatrix(const glm::mat4& viewProjection, const glm::vec3& camera)
{
    const glm::mat4 m = viewProjection * glm::translate(glm::mat4(1.0F), camera);
    for (int i = 0; i < 4; ++i) {
        c[208 + static_cast<std::size_t>(i)] = row(m, i);
    }
}

TrackPrograms::TrackPrograms(
    const world::TrackShaders& shaders, bool bindless, std::string_view technique, bool layeredCube)
    : m_shaders(shaders)
    , m_bindless(bindless)
    , m_layeredCube(layeredCube)
    , m_technique(technique)
    , m_sources(shaders.size())
    , m_programs(shaders.size())
    , m_failed(shaders.size(), false)
{
}

TrackPrograms::~TrackPrograms()
{
    for (const std::unique_ptr<Program>& program : m_programs) {
        if (program) {
            glDeleteBuffers(1, &program->defaults);
        }
    }
}

std::shared_ptr<const TrackPrograms::Source> TrackPrograms::source(std::uint32_t effect)
{
    {
        const std::lock_guard lock(m_mutex);
        if (effect >= m_sources.size()) {
            return nullptr;
        }
        if (m_sources[effect]) {
            return m_sources[effect];
        }
    }
    // Translated outside the lock; two threads may occasionally translate
    // the same effect, and the first result is kept.
    auto built = std::make_shared<Source>();
    const world::TrackShaders::Entry& entry = m_shaders.at(effect);
    try {
        const xenos::Technique* technique = m_technique == kDefault ? &entry.effect.techniques[entry.technique]
                                                                    : entry.effect.findTechnique(m_technique);
        if (technique == nullptr || technique->passes.empty()) {
            throw DataError("no {} technique", m_technique);
        }
        const xenos::Pass& pass = technique->passes.front();
        const xenos::ShaderProgramSource program = xenos::buildProgram(entry.effect, pass, &entry.renderDeclaration);
        built->vertex = redirect(program.vertexGlsl, redirectsOf(program, xenos::ShaderStage::Vertex), m_bindless,
            xenos::ShaderStage::Vertex, entry.readsVertexMaterial);
        if (pass.pixelShader < 0) {
            // Depth only: the translator's empty pixel shader.
            built->fragment = program.fragmentGlsl;
        } else {
            built->fragment = redirect(program.fragmentGlsl, redirectsOf(program, xenos::ShaderStage::Pixel),
                m_bindless, xenos::ShaderStage::Pixel, false);
        }
        if (m_layeredCube) {
            built->vertex = layeredCubeVertex(built->vertex);
        }
        if (m_bindless) {
            built->vertex = bindlessSamplers(std::move(built->vertex), program.samplers, xenos::ShaderStage::Vertex);
            if (pass.pixelShader >= 0) {
                built->fragment
                    = bindlessSamplers(std::move(built->fragment), program.samplers, xenos::ShaderStage::Pixel);
            }
        }
        loadDefaults(program, xenos::ShaderStage::Vertex, built->vertexDefaults);
        loadDefaults(program, xenos::ShaderStage::Pixel, built->pixelDefaults);
        for (const xenos::SamplerBinding& s : program.samplers) {
            Sampler sampler;
            sampler.name = s.name;
            sampler.unit = s.unit;
            sampler.reg = s.stage == xenos::ShaderStage::Pixel ? static_cast<int>(s.unit) : -1;
            sampler.cube = s.dimension == xenos::TextureDimension::Cube;
            sampler.shadowMask = s.name == "ShadowMaskSamp";
            built->samplers.push_back(std::move(sampler));
        }
        if (!program.unsupported.empty()) {
            logWarning("track effect {}: {} instructions not translated faithfully, first: {}", entry.name,
                program.unsupported.size(), program.unsupported.front());
        }
    } catch (const std::exception& e) {
        logWarning("track effect {} cannot be translated: {}", entry.name, e.what());
        built->failed = true;
    }
    const std::lock_guard lock(m_mutex);
    if (!m_sources[effect]) {
        m_sources[effect] = std::move(built);
    }
    return m_sources[effect];
}

void TrackPrograms::prepare(std::uint32_t effect)
{
    source(effect);
}

void TrackPrograms::prepareAll(JobPool& jobs, float priority)
{
    for (std::uint32_t effect = 0; effect < m_shaders.size(); ++effect) {
        jobs.submit(priority, [this, effect] { source(effect); });
    }
}

bool TrackPrograms::compileSome(std::chrono::microseconds budget)
{
    const auto start = std::chrono::steady_clock::now();
    // Builds the driver has finished first, then new ones: started
    // together, they compile on the driver's threads at once.
    for (std::size_t i = 0; i < m_building.size();) {
        if (!m_building[i].build.ready()) {
            ++i;
            continue;
        }
        Building done = std::move(m_building[i]);
        m_building.erase(m_building.begin() + static_cast<std::ptrdiff_t>(i));
        adopt(done.effect, done.build);
    }
    for (std::uint32_t effect = 0; effect < m_programs.size(); ++effect) {
        if (std::chrono::steady_clock::now() - start >= budget) {
            break;
        }
        if (m_programs[effect] || m_failed[effect] || building(effect)) {
            continue;
        }
        std::shared_ptr<const Source> src;
        {
            const std::lock_guard lock(m_mutex);
            src = m_sources[effect];
        }
        if (!src) {
            continue;
        }
        if (src->failed) {
            m_failed[effect] = true;
            continue;
        }
        try {
            m_building.push_back({effect, ProgramBuild(m_shaders.at(effect).name, src->vertex, src->fragment)});
        } catch (const std::exception& e) {
            logWarning("track effect {} does not link: {}", m_shaders.at(effect).name, e.what());
            m_failed[effect] = true;
        }
    }
    return pendingCount() > 0;
}

std::size_t TrackPrograms::pendingCount() const
{
    std::size_t pending = 0;
    for (std::size_t effect = 0; effect < m_programs.size(); ++effect) {
        pending += !m_programs[effect] && !m_failed[effect] ? 1 : 0;
    }
    return pending;
}

bool TrackPrograms::building(std::uint32_t effect) const
{
    return std::any_of(
        m_building.begin(), m_building.end(), [effect](const Building& b) { return b.effect == effect; });
}

const TrackPrograms::Program* TrackPrograms::program(std::uint32_t effect)
{
    if (effect >= m_programs.size() || m_failed[effect]) {
        return nullptr;
    }
    if (m_programs[effect]) {
        return m_programs[effect].get();
    }
    const auto started = std::find_if(
        m_building.begin(), m_building.end(), [effect](const Building& b) { return b.effect == effect; });
    if (started != m_building.end()) {
        Building done = std::move(*started);
        m_building.erase(started);
        return adopt(effect, done.build);
    }
    const std::shared_ptr<const Source> src = source(effect);
    if (!src || src->failed) {
        m_failed[effect] = true;
        return nullptr;
    }
    try {
        ProgramBuild build(m_shaders.at(effect).name, src->vertex, src->fragment);
        return adopt(effect, build);
    } catch (const std::exception& e) {
        logWarning("track effect {} does not link: {}", m_shaders.at(effect).name, e.what());
        m_failed[effect] = true;
        return nullptr;
    }
}

const TrackPrograms::Program* TrackPrograms::adopt(std::uint32_t effect, ProgramBuild& build)
{
    std::shared_ptr<const Source> src;
    {
        const std::lock_guard lock(m_mutex);
        src = m_sources[effect];
    }
    const std::string& name = m_shaders.at(effect).name;
    try {
        auto program = std::make_unique<Program>();
        program->name = name;
        program->shader = Shader(build);
        program->samplers = src->samplers;
        const world::TrackShaders::Entry& entry = m_shaders.at(effect);
        const std::optional<world::TrackShaders::RenderState>& other = m_technique == kDepthOnly
            ? entry.depthOnlyState
            : (m_technique == kRealtimeCubemap ? entry.realtimeCubemapState : entry.shadowDepthOnlyState);
        program->state = m_technique == kDefault ? entry.state : other.value_or(world::TrackShaders::RenderState{});
        glCreateBuffers(1, &program->defaults);
        std::vector<std::uint8_t> bytes(2 * kBlockBytes);
        std::memcpy(bytes.data(), src->vertexDefaults.data(), kBlockBytes);
        std::memcpy(bytes.data() + kBlockBytes, src->pixelDefaults.data(), kBlockBytes);
        glNamedBufferStorage(program->defaults, static_cast<GLsizeiptr>(bytes.size()), bytes.data(), 0);
        m_programs[effect] = std::move(program);
        ++m_compiled;
        return m_programs[effect].get();
    } catch (const std::exception& e) {
        logWarning("track effect {} does not link: {}", name, e.what());
        m_failed[effect] = true;
        return nullptr;
    }
}

} // namespace openhorizon::render
