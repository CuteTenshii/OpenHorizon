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
#include <vector>

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
inline constexpr std::uint32_t kFirstTrackTexture = 4;
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

    /// Fills the constants for a camera and lighting: the camera's matrix
    /// (world matrices are identity, as ShadedMesh bakes placements into its
    /// vertices), fog, sun and ambient colours. `model` places moving
    /// objects: it becomes WorldMatrix and joins WorldViewProjMatrix.
    static TrackFrameConstants make(const glm::mat4& viewProjection, const glm::mat4& inverseView,
        const glm::vec3& camera, const SceneLighting& lighting, const glm::mat4& model = glm::mat4(1.0F));
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
/// (0x821C8238, 0x821C7F7C: `mul r, r, r` on each fetched colour). The HDR
/// target here holds linear colour, so the programs square their output.
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

    TrackPrograms(const world::TrackShaders& shaders, bool bindless);
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
    std::mutex m_mutex;
    std::vector<std::shared_ptr<const Source>> m_sources;
    std::vector<std::unique_ptr<Program>> m_programs;
    std::vector<bool> m_failed;
    std::vector<Building> m_building;
    std::size_t m_compiled = 0;
};

} // namespace fh::render
