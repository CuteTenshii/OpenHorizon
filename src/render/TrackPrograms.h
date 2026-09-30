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
#include <string>
#include <string_view>
#include <vector>

namespace fh::world {
struct TrackSettings;
} // namespace fh::world

namespace fh::render {

struct SceneLighting;

/// Binding points the track programs use beyond the translator's own (see
/// xenos/GlslTranslator.h: 0-3 are its constant blocks, storage binding 0
/// the vertex stream).
namespace trackbinding {
/// The frame's scene constants (XeFrameConstants, 256 vec4, std140).
inline constexpr GLuint kFrame = 4;
/// A batch's material constants (XeMaterialConstants, 16 vec4, std140),
/// or with bindless textures every batch's material of the mesh (storage
/// buffer XeMaterials, one TrackMaterial each, indexed by the draw's base
/// instance).
inline constexpr GLuint kMaterial = 5;
/// With bindless textures: the texture handle table (storage buffer
/// XeTextureHandles, one uvec2 handle per entry).
inline constexpr GLuint kTextureHandles = 6;
/// With layered cube faces (see TrackPrograms): the faces' transforms and
/// layers (uniform block XeCubeFaces, std140: six mat4, then six ivec4
/// whose x is the layer), indexed by the draw's instance.
inline constexpr GLuint kCubeFaces = 7;
} // namespace trackbinding

/// A batch's material as the programs read it: the material constants, the
/// batch's first vertex in 32-bit words of its mesh's vertex buffer, and
/// with bindless textures the texture handle table entry of each sampler
/// register (kEngineTextureEntry where the engine supplies the texture).
/// The layout is the same under std140 and std430.
struct TrackMaterial {
    static constexpr std::size_t kConstants = 15;
    static constexpr std::uint32_t kEngineTextureEntry = 0xFFFFFFFF;
    std::array<glm::vec4, kConstants> constants{};
    std::array<std::uint32_t, 4> base{};
    std::array<std::uint32_t, 16> textures{};
};
static_assert(sizeof(TrackMaterial) == 320);

/// Fixed entries of the texture handle table: the textures the engine
/// supplies (see WorldRenderer) come first, the track's own after them.
namespace texturehandle {
inline constexpr std::uint32_t kBlack = 0;
inline constexpr std::uint32_t kWhite = 1;
inline constexpr std::uint32_t kBlackCube = 2;
inline constexpr std::uint32_t kBlack3D = 3;
/// ShadowMaskSamp: the screen's sun shadow mask, white without one.
inline constexpr std::uint32_t kShadowMask = 4;
inline constexpr std::uint32_t kFirstTrackTexture = 5;
} // namespace texturehandle

/// The float constants the game sets once per scene for every track effect,
/// by register (the same register in every track effect that declares the
/// name, and the same in both stages). The values follow the executable:
/// the scene constant writer at 0x82443D50 copies them from one lighting
/// block into both stages (vertex register r at shadow offset 1920 + 16 r,
/// pixel at 6016 + 16 r), and 0x8265A7CC samples that block's colours from
/// the time-of-day curves.
struct TrackFrameConstants {
    std::array<glm::vec4, 256> c{};

    /// Seconds after which the presentation's clock (TimeGain.x) starts
    /// again from zero.
    static constexpr float kPresentationClockWrap = 28800.0F;

    /// Fills the constants for a camera and lighting: the camera's matrix
    /// (world matrices are identity, as ShadedMesh bakes placements into its
    /// vertices), fog, sun and ambient colours, and TimeGain from
    /// `presentationSeconds`, the seconds the world has run unpaused since
    /// the track was loaded (see timeGain()). `model` places moving objects:
    /// it becomes WorldMatrix and joins WorldViewProjMatrix.
    /// `settings`, the track's TrackSettings.xml values, gives SkyScale,
    /// RoadSpecularParams2 and V2LightmapColor1 and 2; without it they are
    /// 0.
    static TrackFrameConstants make(const glm::mat4& viewProjection, const glm::mat4& inverseView,
        const glm::vec3& camera, const SceneLighting& lighting, float presentationSeconds,
        const glm::mat4& model = glm::mat4(1.0F), const world::TrackSettings* settings = nullptr);

    /// Below this the SwitchOnLights curve counts as 0 (the constant at
    /// 0x82000CD0 the time-of-day sampler compares with at 0x8265AF78).
    static constexpr float kSwitchOnLightsFloor = 0.05F;

    /// EmissiveSwitchOnThreshold (c213) for the sampled SwitchOnLights
    /// curve, the third lane of the track's TrackLightMapParams vector (see
    /// world::TrackSettings::trackLightMap) and the presentation's clock.
    ///
    /// Verified in the executable:
    ///  - x: the time-of-day sampler (0x8265AF60) takes the curve, sets it
    ///    to 0 below kSwitchOnLightsFloor (0x8265AF78) and hands it to the
    ///    track presentation (slot 32, 0x82E113F0), which keeps -1 in its
    ///    place when it is 0; the caller of the scene writer copies that
    ///    (0x82469E68). Effects with emissive maps light them where x is at
    ///    least their ModelData.x (`sge r, c213.x, c148.x`), so they are off
    ///    by day and on by night.
    ///  - y: min(max(x - 0.5, 0) * 4, 1) times the lane (0x82443FE8-
    ///    0x82444058, constants 0.5, 0, 4 and 1 at 0x82000D38, 0x82000E40,
    ///    0x82000D70, 0x82000D28). The lightmapped effects light their
    ///    lightmap by max(c213.y, the material's c6.z).
    ///  - z: 1 (0x82000D28), the scale of the emissive maps.
    ///  - w: the presentation's second clock (+10152), advanced with the
    ///    TimeGain clock and wrapped the same way (0x82491660), read by the
    ///    countdown effect's vertex shader.
    /// Missing: the draw loop (0x82444B40) replaces c213 per model when the
    /// model's instance record holds a non-negative value at +48, with (10,
    /// y, that value, w), which forces its emissive maps on at that
    /// strength; where those records come from was not traced, so models
    /// keep the scene's value. The second clock's reset to 0 (0x82E11418)
    /// and raise to 3600 s (0x82E11450) are not done, as their callers
    /// were not traced. The front end's background scene forces x to 1
    /// (0x8265AF94), which does not apply to the world.
    static glm::vec4 emissiveSwitchOn(float switchOnLights, float lightmapScale, float presentationSeconds);

    /// TimeGain (c249) for `presentationSeconds`, which every animated
    /// track effect (flags, water, waterfalls, flipbook screens, swaying
    /// trees) reads in its x as the time its animation has run.
    ///
    /// Verified in the executable: the scene writer (0x824441B8) stores
    /// c249 from its scene block, whose x the track presentation fills
    /// (0x82469F40) from the presentation's float clock. The presentation's
    /// update (CPresentation, 0x82491618) adds the frame's time to that
    /// clock, subtracting 28800 once it reaches 28800 s; it starts at 0
    /// when the presentation is made (0x82E93DF0). The game loop hands the
    /// update the frame's time scaled by the game speed (0x824141A8,
    /// 0x824142A8) and, while the game is paused (CPauseGame, byte 12 of
    /// the game object, 0x8241419C), 0 (0x824C5138), so the clock stops
    /// with the pause. Over Colorado's 173 effects only x and w of c249 are
    /// read; y, z and w keep the effects' own default (0, 1, 1), which all
    /// 369 constant tables declaring TimeGain give it. Missing: the game
    /// writes into w a value of the presentation's settings (+596 of the
    /// block at CTrackPresentation + 192, 0x8246A01C) whose meaning was not
    /// found, and into y and z values no effect reads.
    static glm::vec4 timeGain(float presentationSeconds);

    /// HackMatrix (c208-c211), the depth techniques' view-projection: the
    /// DepthOnly and ShadowDepthOnly vertex shaders subtract the camera
    /// position (the w of ViewInvMatrix, c243-c245) from the world position
    /// and multiply by it. Verified in Colorado's effects' microcode; that
    /// the game fills it with the pass's view-projection around the camera
    /// is inferred from that use.
    void setHackMatrix(const glm::mat4& viewProjection, const glm::vec3& camera);
};

/// The track effects' Default passes translated to GLSL and linked, one per
/// effect, built on first use and kept.
///
/// Each program reads the frame constants from its own block
/// (trackbinding::kFrame) and the material constants from another
/// (trackbinding::kMaterial) instead of the translator's per-stage block,
/// so that thousands of batches share one frame block and bind 256 bytes of
/// their own: the generated code's reads of those registers are redirected.
/// Every other register keeps its value from the effect: literal constants
/// and the constant table's defaults, uploaded once per program.
///
/// With bindless textures (GL_ARB_bindless_texture and
/// GL_ARB_shader_draw_parameters) the programs take no texture units and no
/// per-batch block: each sampler is built from the handle table entry its
/// batch's material names, and the batch is the draw's base instance, so a
/// draw needs no binding at all between batches of one mesh.
///
/// The pixel shaders write the square root of the colour (every track
/// effect's Default pass ends in `sqrt oC0`), which the game's scene target
/// stores and its post-processing squares back when it reads the target
/// (0x821C8238, 0x821C7F7C: `mul r, r, r` on each fetched colour). The
/// scene target here holds the same, so the output is written as it is.
class TrackPrograms {
public:
    struct Sampler {
        std::string name;
        GLuint unit = 0;
        /// Pixel sampler register (the material's texture slot), or -1 for
        /// vertex-stage samplers.
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
    /// Drawn by the game into the screen depth and into the shadow maps
    /// (see ShadowPass); they take HackMatrix (c208) as the view-projection
    /// of camera-relative positions.
    static constexpr std::string_view kDepthOnly = "DepthOnly";
    static constexpr std::string_view kShadowDepthOnly = "ShadowDepthOnly";
    /// Drawn into the faces of the dynamic environment cube (see
    /// WorldRenderer::drawCubeFace()).
    static constexpr std::string_view kRealtimeCubemap = "RealtimeCubemap";

    /// With `layeredCube` the vertex shaders draw one instance per face of a
    /// layered cube target: each instance's clip position is multiplied by
    /// its face's transform and sent to its face's layer (XeCubeFaces; needs
    /// GL_ARB_shader_viewport_layer_array). The frame constants then map
    /// positions to the cube's centre only (see WorldRenderer::
    /// drawCubeFaces()).
    TrackPrograms(const world::TrackShaders& shaders, bool bindless, std::string_view technique = kDefault,
        bool layeredCube = false);
    ~TrackPrograms();
    TrackPrograms(const TrackPrograms&) = delete;
    TrackPrograms& operator=(const TrackPrograms&) = delete;

    /// Translates effect `effect` if not done yet. Safe from any thread.
    void prepare(std::uint32_t effect);
    /// Queues the translation of every effect of the track on `jobs`, so
    /// compileSome() can link them all before the world is first drawn.
    /// The jobs refer to this object: its owner must clear the pool before
    /// destroying it.
    void prepareAll(JobPool& jobs, float priority);
    /// Starts building translated effects on the GL thread until `budget`
    /// has passed, and adopts the builds the driver has finished; builds run
    /// on the driver's threads where it compiles in parallel (see
    /// GlProgram.h). Returns whether effects remain to link, translated or
    /// not.
    bool compileSome(std::chrono::microseconds budget);
    /// Effects neither linked nor failed yet.
    std::size_t pendingCount() const;
    /// The linked program of `effect`, compiling it now if needed (GL
    /// thread); nullptr if it cannot be built.
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

    /// A program build under way for an effect.
    struct Building {
        std::uint32_t effect = 0;
        ProgramBuild build;
    };

    std::shared_ptr<const Source> source(std::uint32_t effect);
    bool building(std::uint32_t effect) const;
    /// Finishes `build` into the program of `effect`; nullptr, with the
    /// effect marked failed, when it does not link.
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

} // namespace fh::render
