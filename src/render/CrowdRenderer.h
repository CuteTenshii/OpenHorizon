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

/// Draws the crowds of the track's crowd placement sets as the game's
/// CProceduralCharacters does (see spectators/CrowdDraw.h for the rules and
/// their evidence): every person of a shown, visible set as a sprite, and
/// the people the model queue takes as skinned models with their clips.
///
/// The shaders follow the game's PROC_CHARACTER_SKIN_VS/PS and
/// PROC_CHARACTER_BILLBOARD_VS/PS (the executable's PROC_CHARACTER_*.updb
/// blobs at 0x82184970, 0x82165788, 0x821839D0 and 0x821652B0), step by
/// step:
///
/// - Models: each vertex is the blend of its two bones by the second
///   bone's weight, each bone a 3x4 matrix of the clip's frame; then the
///   person's placement (the draw record's rows: right, up and facing,
///   scaled by the person's scale for people who stand, and the position).
///   The pixel is the skin texture lit by `ambColor` plus `sunColor` times
///   the sun's cosine times the screen's sun shadow mask, fogged, its alpha
///   the texture's times the fade.
/// - Sprites: a quad standing on the person's feet, turned about the
///   vertical to face along the camera's view (not towards the camera),
///   0.9 m wide and 1.9 m tall at size 1, showing the cell
///   spectators::draw::spriteColumn() picks. The vertex lights it with
///   `ambColor`, the tint squared times EmissiveSwitchOnThreshold.y and
///   `sunColor` times the sun's cosine against the view's backward
///   direction; the pixel shadows all but the ambient with the mask.
/// - Both write the square root of the colour, as the scene target holds
///   (see FrameRenderer), and draw with alpha to mask, which on the
///   single-sample target keeps pixels whose alpha reaches one half as
///   WorldRenderer emulates it (the draws set RB alpha-to-mask with offsets
///   0x87, 0x8246C158 and 0x8248C6E0; the dither itself is not reproduced).
///
/// Verified from the draws (0x8246C158, 0x8248A270): FadeValues, AnimInfo
/// (the clip's first bone row plus frame times bones), the sprite constants
/// (32 cells a row, the atlas's rows, 0.5 m, 2 m, 0.9, 0.95, 0.95, 1.05) and
/// that every instance of a set is drawn. Inferred or left out:
/// - The pixel shaders' c360_0.z multiplies the texture; the skinned draw
///   fills that register slot (the device constant at +0x1550, taken to be
///   pixel c0) with the tint squared times a factor from the view's object
///   (+0xEE84) whose writer was not found, plus the scene ambient, and the
///   sprite draw leaves whatever was there. Neither is reproduced: the
///   texture is used as it is.
/// - SKIN_PS's headlight term (bEnableDeferredLightContribution) needs the
///   deferred light buffer this renderer does not have, as for the track
///   (see TODO.md); it is left out.
/// - The bone rows are float matrices rather than the game's 11:11:10
///   rows, which differ from them by the rows' quantisation.
/// - The sprite quad's across direction: the shader takes it from CamLook,
///   row 2 of the camera's matrix at view block +0x110 (0x8248A438), as
///   (-CamLook.z, 0, CamLook.x) and lights with -CamLook. Taking CamLook as
///   the view direction (its name, and the lighting then faces the camera)
///   puts the quad's +x corner on the screen's left.
/// - People do not cast sun shadows: the depth passes that could draw them
///   (SKIN_DEPTH, BILLBOARD_DEPTH) were not traced to the shadow maps. Nor
///   do they get blob shadows: PROC_CHARACTER_SHADOW is never used and
///   CCrowdPresentation's blob shadows (0x82EA1E98) draw only the track's
///   `.crowd` file, which is empty on every track of the disc.
class CrowdRenderer {
public:
    /// What the last draw() drew.
    struct Stats {
        /// Models by level of detail, walkers included.
        std::array<std::size_t, 4> models{};
        std::size_t walkers = 0;
        /// Sets shown by the session and in view, and those drawn as
        /// sprites (within the sprites' reach).
        std::size_t visibleSets = 0;
        /// Of those, the sets of events (with an event block): a race's own
        /// crowds, or free roam's at the events' starts.
        std::size_t visibleEventSets = 0;
        std::size_t spriteSets = 0;
        /// Sprite instances handed to the GPU; the shader collapses those
        /// outside their fade.
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

    /// Culls, queues and animates the people for this frame's camera and
    /// draws them into the bound scene target. The clips advance one frame
    /// per 30th of a second of frame.time, the presentation clock, which
    /// stops while the game is paused: the game advances a queued person's
    /// frame once per frame it renders (0x82EB9DF0) and renders at 30 Hz
    /// (FrameRate 30 in the in-game render scenario), the clips' own rate.
    void draw(const FrameParams& frame, const world::TrackSettings* settings);

    const Stats& stats() const { return m_stats; }
    /// Clip frames the presentation clock has moved the crowd on by since
    /// the renderer was made.
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
