#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include "spectators/CrowdDraw.h"
#include "spectators/CrowdSet.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openhorizon::world {
struct TrackSettings;
}

namespace openhorizon::render {

struct FrameParams;

/// Draws the track's crowds as CProceduralCharacters does (rules in
/// spectators/CrowdDraw.h): sprites for every person of a shown set, skinned
/// models for those the model queue takes. Shaders ported from
/// PROC_CHARACTER_SKIN_VS/PS and PROC_CHARACTER_BILLBOARD_VS/PS (0x82184970,
/// 0x82165788, 0x821839D0, 0x821652B0); alpha to mask (0x8248C6E0) without the dither.
///
/// Verified from the draws (0x8246C158, 0x8248A270): FadeValues, AnimInfo
/// and the sprite constants. Inferred or left out:
/// - Pixel c360_0.z (device constant +0x1550) is not applied (its writer,
///   view object +0xEE84, was not found).
/// - SKIN_PS's deferred headlight term: no deferred light buffer here.
/// - Bones are float matrices, not the game's 11:11:10 rows.
/// - The sprite's across direction from CamLook (0x8248A438) is inferred.
/// - No sun shadows (depth passes not traced) and no blob shadows (0x82EA1E98
///   draws only the track's `.crowd`, empty on every track).
class CrowdRenderer {
public:
    /// What the last draw() drew.
    struct Stats {
        /// Models by level of detail, walkers included.
        std::array<std::size_t, 4> models{};
        std::size_t walkers = 0;
        /// Sets shown by the session and in view.
        std::size_t visibleSets = 0;
        /// Of those, the sets with an event block.
        std::size_t visibleEventSets = 0;
        std::size_t spriteSets = 0;
        /// Sprite instances handed to the GPU, faded-out ones included.
        std::size_t spriteInstances = 0;
        /// Clip frames the queued people moved on by.
        int ticks = 0;
        std::size_t modelTotal() const { return models[0] + models[1] + models[2] + models[3]; }
    };

    /// Uploads the crowd's meshes, skins, sprite atlas, clips and sprite
    /// instances; needs the GL context. Throws DataError when the crowd has
    /// no sprite atlas or its skins do not share one size and format.
    explicit CrowdRenderer(const spectators::CrowdSet& crowd);
    ~CrowdRenderer();
    CrowdRenderer(const CrowdRenderer&) = delete;
    CrowdRenderer& operator=(const CrowdRenderer&) = delete;

    /// Which sets show (see spectators::CrowdSession).
    void setSession(const spectators::CrowdSession& session);
    /// The screen's sun shadow mask (ShadowPass::mask()), or 0 for none.
    void setShadowMask(GLuint texture) { m_shadowMask = texture; }

    /// Draws into the bound scene target. Clips advance at 30 Hz of
    /// frame.time, the game's render rate (0x82EB9DF0), so they stop on pause.
    void draw(const FrameParams& frame, const world::TrackSettings* settings);

    const Stats& stats() const { return m_stats; }
    /// Clip frames advanced since construction.
    std::uint64_t ticksApplied() const { return m_ticksApplied; }

private:
    struct Model {
        std::uint32_t layer = 0;
        std::uint32_t baseVertex = 0;
        std::array<std::uint32_t, 4> firstIndex{};
        std::array<std::uint32_t, 4> indexCount{};
        int lods = 0;
    };
    struct Person {
        /// Current clip and frame (the game's per-person state).
        int animation = -1;
        int frame = 0;
        float walked = 0.0F;
    };
    struct Queued {
        std::uint32_t spectator = 0;
        float distanceSquared = 0.0F;
        glm::vec3 position{0.0F};
        float heading = 0.0F;
        bool walker = false;
    };

    void uploadMeshes(const spectators::CrowdSet& crowd);
    void uploadSkins(const spectators::CrowdSet& crowd);
    void uploadClips(const spectators::CrowdSet& crowd);
    void uploadSprites(const spectators::CrowdSet& crowd);
    /// Moves a queued static person's clip on by `ticks` frames, choosing
    /// the next clip when it ends (0x82ECAB38's animation step).
    void advance(std::uint32_t spectator, int ticks);

    const spectators::CrowdSet& m_crowd;
    std::vector<Model> m_models;
    std::vector<Person> m_people;
    std::vector<std::uint32_t> m_randomState;
    spectators::draw::CrtRandom m_crt;
    std::vector<std::uint32_t> m_clipBase;
    std::vector<std::uint32_t> m_groupSpriteFirst;
    std::vector<std::uint32_t> m_groupSpriteCount;
    std::vector<bool> m_shown;
    spectators::CrowdSession m_session = spectators::CrowdSession::freeRoam();

    GLuint m_meshVertices = 0;
    GLuint m_meshIndices = 0;
    GLuint m_modelVao = 0;
    GLuint m_instances = 0;
    GLuint m_modelCommands = 0;
    GLuint m_bones = 0;
    GLuint m_skins = 0;
    GLuint m_spriteInstances = 0;
    GLuint m_spriteIndices = 0;
    GLuint m_spriteVao = 0;
    GLuint m_spriteCommands = 0;
    GLuint m_shadowMask = 0;
    Texture m_atlas;
    Texture m_white;
    int m_atlasRows = 1;
    Shader m_modelShader;
    Shader m_spriteShader;

    long long m_lastTick = -1;
    std::uint64_t m_ticksApplied = 0;
    float m_lastTime = 0.0F;
    Stats m_stats;
};

} // namespace openhorizon::render
