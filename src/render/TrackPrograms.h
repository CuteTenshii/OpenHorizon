#pragma once

#include "Gl.h"
#include "GlProgram.h"
#include "Shader.h"

#include "base/JobPool.h"
#include "world/TrackShaders.h"
#include "xenos/ShaderProgram.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::world {
struct TrackSettings;
} // namespace openhorizon::world

namespace openhorizon::render {

struct SceneLighting;

/// Binding points beyond the translator's own (0-3 constant blocks, storage
/// binding 0 the vertex stream).
namespace trackbinding {
/// The frame's scene constants (XeFrameConstants, 256 vec4, std140).
inline constexpr GLuint kFrame = 4;
/// A batch's material (XeMaterialConstants, std140), or with bindless textures
/// every batch's (storage buffer XeMaterials, indexed by base instance).
inline constexpr GLuint kMaterial = 5;
/// Bindless texture handle table (storage buffer XeTextureHandles, uvec2 each).
inline constexpr GLuint kTextureHandles = 6;
/// Layered cube faces (XeCubeFaces, std140: six mat4, then six ivec4 whose x
/// is the layer), indexed by instance.
inline constexpr GLuint kCubeFaces = 7;
} // namespace trackbinding

/// A batch's material; `base` is its first vertex in 32-bit words, `model`
/// rows 0-2 of its model matrix. Same layout under std140 and std430.
/// Effects reading vertex material constants draw props in model space.
struct TrackMaterial {
    static constexpr std::size_t kConstants = 15;
    static constexpr std::uint32_t kEngineTextureEntry = 0xFFFFFFFF;
    std::array<glm::vec4, kConstants> constants{};
    std::array<std::uint32_t, 4> base{};
    std::array<glm::vec4, 3> model{
        glm::vec4(1.0F, 0.0F, 0.0F, 0.0F), glm::vec4(0.0F, 1.0F, 0.0F, 0.0F), glm::vec4(0.0F, 0.0F, 1.0F, 0.0F)};
    std::array<glm::vec4, 3> modelInverse = model;
    std::array<std::uint32_t, 16> textures{};

    /// Sets model and modelInverse from `matrix`, an affine transform.
    void setModel(const glm::mat4& matrix);
};
static_assert(sizeof(TrackMaterial) == 416);
/// The bytes a program reads without bindless textures.
inline constexpr std::size_t kTrackMaterialBlockBytes = offsetof(TrackMaterial, textures);

/// The slot a constant named "c360_N" (pixel) or "vc360_N" / "c360_N"
/// (vertex, slot N - 3; the bunting effects use "c360_N") reads.
std::optional<std::uint32_t> materialSlot(std::string_view name, xenos::ShaderStage stage);

/// Fixed entries of the handle table: engine textures first, the track's after.
namespace texturehandle {
inline constexpr std::uint32_t kBlack = 0;
inline constexpr std::uint32_t kWhite = 1;
inline constexpr std::uint32_t kBlackCube = 2;
inline constexpr std::uint32_t kBlack3D = 3;
/// ShadowMaskSamp: the screen's sun shadow mask, white without one.
inline constexpr std::uint32_t kShadowMask = 4;
inline constexpr std::uint32_t kFirstTrackTexture = 5;
} // namespace texturehandle

/// Per-scene float constants by register, the same in both stages. Written
/// by 0x82443D50 (vertex at shadow offset 1920 + 16 r, pixel 6016 + 16 r);
/// colours sampled from the time-of-day curves at 0x8265A7CC.
struct TrackFrameConstants {
    std::array<glm::vec4, 256> c{};

    /// Seconds after which TimeGain.x wraps to zero.
    static constexpr float kPresentationClockWrap = 28800.0F;

    /// `presentationSeconds`: unpaused world time since load. `model` places
    /// moving objects. Without `settings` (TrackSettings.xml), SkyScale,
    /// RoadSpecularParams2 and V2LightmapColor1/2 are 0.
    static TrackFrameConstants make(const glm::mat4& viewProjection, const glm::mat4& inverseView,
        const glm::vec3& camera, const SceneLighting& lighting, float presentationSeconds,
        const glm::mat4& model = glm::mat4(1.0F), const world::TrackSettings* settings = nullptr);

    /// SwitchOnLights below this counts as 0 (0x82000CD0, used at 0x8265AF78).
    static constexpr float kSwitchOnLightsFloor = 0.05F;

    /// EmissiveSwitchOnThreshold (c213). Verified: x the curve, -1 when 0
    /// (0x8265AF60, 0x82E113F0, 0x82469E68); y min(max(x - 0.5, 0) * 4, 1) *
    /// lane (0x82443FE8-0x82444058; 0.5, 0, 4, 1 at 0x82000D38, 0x82000E40,
    /// 0x82000D70, 0x82000D28); z 1 (0x82000D28); w the second clock (+10152,
    /// 0x82491660). The front end's x = 1 (0x8265AF94) is not the world's.
    /// Missing: per-model override (0x82444B40, record +48), clock reset and
    /// 3600 s raise (0x82E11418, 0x82E11450).
    static glm::vec4 emissiveSwitchOn(float switchOnLights, float lightmapScale, float presentationSeconds);

    /// TimeGain (c249); x is the animation time. Verified: written at
    /// 0x824441B8, x from the presentation clock (0x82469F40; 0 at creation,
    /// 0x82E93DF0), advanced by CPresentation (0x82491618) at game speed
    /// (0x824141A8, 0x824142A8), stopped while paused (0x8241419C,
    /// 0x824C5138). Missing: w from 0x8246A01C, unknown.
    static glm::vec4 timeGain(float presentationSeconds);

    /// HackMatrix (c208-c211), the depth techniques' view-projection of
    /// camera-relative positions. Use verified in microcode; contents inferred.
    void setHackMatrix(const glm::mat4& viewProjection, const glm::vec3& camera);
};

/// The track effects' passes translated to GLSL, built on first use. Frame
/// and material registers are redirected to shared blocks so batches share
/// one frame block. Output stays sqrt colour, squared back by post (0x821C8238,
/// 0x821C7F7C).
class TrackPrograms {
public:
    struct Sampler {
        std::string name;
        GLuint unit = 0;
        /// Pixel sampler register, or -1 for vertex-stage samplers.
        int reg = -1;
        bool cube = false;
        /// The screen-space shadow mask (ShadowMaskSamp).
        bool shadowMask = false;
    };
    struct Program {
        std::string name;
        Shader shader;
        /// Program defaults for constant blocks 0 (vertex) and 1 (pixel).
        GLuint defaults = 0;
        std::vector<Sampler> samplers;
        /// The pass's render states (see world::TrackShaders::RenderState).
        world::TrackShaders::RenderState state;
    };

    /// The name of each effect's technique the programs run.
    static constexpr std::string_view kDefault = "Default";
    /// Screen depth and shadow maps; they take HackMatrix (c208).
    static constexpr std::string_view kDepthOnly = "DepthOnly";
    static constexpr std::string_view kShadowDepthOnly = "ShadowDepthOnly";
    /// The dynamic environment cube's faces.
    static constexpr std::string_view kRealtimeCubemap = "RealtimeCubemap";

    /// `layeredCube`: one instance per cube face, sent to its layer (needs
    /// GL_ARB_shader_viewport_layer_array).
    TrackPrograms(const world::TrackShaders& shaders, bool bindless, std::string_view technique = kDefault,
        bool layeredCube = false);
    ~TrackPrograms();
    TrackPrograms(const TrackPrograms&) = delete;
    TrackPrograms& operator=(const TrackPrograms&) = delete;

    /// Translates effect `effect` if not done yet. Safe from any thread.
    void prepare(std::uint32_t effect);
    /// The jobs refer to this object: clear the pool before destroying it.
    void prepareAll(JobPool& jobs, float priority);
    /// Starts and adopts builds until `budget` passes; returns whether effects
    /// remain to link.
    bool compileSome(std::chrono::microseconds budget);
    /// Effects neither linked nor failed yet.
    std::size_t pendingCount() const;
    /// Compiles now if needed (GL thread); nullptr if it cannot be built.
    const Program* program(std::uint32_t effect);
    std::size_t compiledCount() const { return m_compiled; }
    bool bindless() const { return m_bindless; }
    bool layeredCube() const { return m_layeredCube; }

private:
    struct Source {
        bool failed = false;
        std::string vertex;
        std::string fragment;
        std::array<glm::vec4, 256> vertexDefaults{};
        std::array<glm::vec4, 256> pixelDefaults{};
        std::vector<Sampler> samplers;
    };

    struct Building {
        std::uint32_t effect = 0;
        ProgramBuild build;
    };

    std::shared_ptr<const Source> source(std::uint32_t effect);
    bool building(std::uint32_t effect) const;
    /// nullptr, with the effect marked failed, when it does not link.
    const Program* adopt(std::uint32_t effect, ProgramBuild& build);

    const world::TrackShaders& m_shaders;
    bool m_bindless = false;
    bool m_layeredCube = false;
    std::string m_technique;
    std::mutex m_mutex;
    std::vector<std::shared_ptr<const Source>> m_sources;
    std::vector<std::unique_ptr<Program>> m_programs;
    std::vector<bool> m_failed;
    std::vector<Building> m_building;
    std::size_t m_compiled = 0;
};

} // namespace openhorizon::render
