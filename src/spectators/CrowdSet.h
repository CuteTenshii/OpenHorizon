#pragma once

#include "CrowdAnimation.h"
#include "CrowdPlacement.h"
#include "SkinnedModel.h"
#include "SpectatorConfig.h"
#include "SpectatorSprites.h"

#include "world/TrackTextures.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

/// Spectators: the crowds of the festival, of race starts and along routes (draw rules in CrowdDraw.h).
/// Person, clip and frame are not stored: the set's init (CProceduralCharacters, 0x82EDD8A8) draws them from a
/// generator seeded with the set's person count, as placeSpectators() does.
namespace openhorizon::spectators {

/// In spectator space: the mesh files' coordinates mirrored in X, so a person faces +Z in the left-handed world
/// frame. Every mesh, skinned position and skinning matrix of this library is in it.
/// Vertices and winding as world::TileMesh, plus two bone indices and two weights; colour is opaque white.
struct SpectatorMesh {
    static constexpr int kFloatsPerVertex = 13;
    static constexpr int kNormal = 3;
    static constexpr int kTexcoord = 6;
    static constexpr int kColour = 8;
    static constexpr int kBones = 9;
    static constexpr int kWeights = 11;

    struct Batch {
        /// Index into CrowdSet::textures().
        std::uint32_t texture = 0;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    /// One batch per level of detail, finest first.
    std::vector<Batch> lods;

    std::size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
    std::size_t triangleCount(std::size_t lod) const { return lod < lods.size() ? lods[lod].indexCount / 3 : 0; }
};

/// Every level is drawn with `texture`.
SpectatorMesh buildSpectatorMesh(const SkinnedModel& model, std::uint32_t texture);

/// Converts skinMatrices() output into spectator space, in place.
void mirrorToSpectatorSpace(std::span<glm::mat4> matrices);

/// A person in one pose: a <models> entry of spectators.xml with the mesh of
/// one skeleton's suffix.
struct SpectatorModel {
    /// "Male01".
    std::string name;
    /// Index in spectators.xml's <models>, also its place in the sprite atlas.
    int configModel = 0;
    /// Index into CrowdSet::skeletons() of the first skeleton with the mesh's suffix.
    std::uint32_t skeleton = 0;
    bool seated = false;
    std::string path;
    /// Height of the mesh in its bind pose, metres.
    float height = 0.0F;
    SpectatorMesh mesh;
    /// The mesh as the file stores it, for CPU skinning.
    SkinnedModel source;
    /// Index into CrowdSet::textures().
    std::uint32_t texture = 0;
};

/// `chain.origin` is meaningless here; `chain.files` and `path` name the game file.
struct SpectatorTexture {
    std::string name;
    std::string path;
    world::TextureMipChain chain;
};

/// A <skeleton> of spectators.xml with the bones its clips carry.
struct CrowdSkeleton {
    std::string name;
    std::string suffix;
    bool seated = false;
    /// Empty when none of its clips could be read.
    Skeleton skeleton;
    /// Indices into CrowdSet::animations().
    std::vector<std::uint32_t> animations;
};

struct SpectatorAnimation {
    std::string name;
    std::string path;
    AnimationClip clip;
};

struct WeightedChoice {
    std::uint32_t index = 0;
    float weight = 1.0F;
};

/// A <crowdclass> resolved to indices.
struct CrowdClass {
    int id = 0;
    std::string name;
    /// Index into CrowdSet::skeletons(), -1 when spectators.xml names none it defines.
    int skeleton = -1;
    bool seated = false;
    /// Indices into CrowdSet::models() of the people it shows.
    std::vector<std::uint32_t> models;
    /// Indices into CrowdSet::animations(); clips the disc lacks are left out and reported.
    std::vector<WeightedChoice> idles;
    std::vector<WeightedChoice> cheers;
    glm::vec3 offset {0.0F};
    float billboardOffsetY = 0.0F;
    std::optional<bool> randomSize;
    /// draw::clipTable() of CrowdSet::animations() indices; empty when the class has no clip of the kind.
    std::vector<std::uint32_t> idleTable;
    std::vector<std::uint32_t> cheerTable;
    /// Six sprite views instead of three: the first class only (0x82EDDF08 compares the class with 0).
    bool sixSpriteViews = false;
};

/// One crowd placement set of the track.
struct CrowdGroup {
    std::string name;
    /// Its file in the track's archive.
    std::string file;
    bool hasEventBlock = false;
    std::string eventTag;
    /// HorizonEventID of the tag ("FR04"), empty without one.
    std::string eventId;
    std::vector<std::uint8_t> routes;
    glm::vec3 boundsMin {0.0F};
    glm::vec3 boundsMax {0.0F};
    /// Its people: CrowdSet::spectators()[first .. first + count).
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    /// The set's generator after placement; the game keeps drawing from it when clips end.
    std::uint32_t randomState = 0;

    /// Sets without an event block are shown whatever the player does.
    bool alwaysShown() const { return !hasEventBlock; }
    /// By tag, ignoring case.
    bool belongsToEvent(std::string_view horizonEventId) const;
    /// By tag, or by listing `routeId` (Tracks.RouteId) among its routes.
    bool belongsToRace(std::string_view horizonEventId, int routeId) const;
    /// Lists route 0, the free-roam session's.
    bool listsFreeRoam() const;
};

/// A placed person.
struct Spectator {
    glm::vec3 position {0.0F};
    /// Radians clockwise from north (see PlacedSpectator::headingRadians()).
    float heading = 0.0F;
    /// Index into CrowdSet::classes(), -1 when spectators.xml lacks the class.
    int crowdClass = -1;
    /// Index into CrowdSet::models(), -1 when the class has no loadable one.
    int model = -1;
    /// Index into CrowdSet::animations() of the idle clip, -1 without one.
    int animation = -1;
    /// The clip's frame at load, so neighbours do not move in step.
    int startFrame = 0;
    /// 0.95 to 1.05 by the size factor, a 16-bit float in the game (0x82EDDD90, 0x833A5690/94).
    float modelScale = 1.0F;
    /// A draw from 0.1 to 1 with randomsize (the default, 0x8320E4F8), else 1.
    float sizeFactor = 1.0F;
    /// Drawn only as a sprite (PlacedSpectator::flags).
    bool billboardOnly = false;
    /// The person's tint, 0 to 1 (PlacedSpectator::extra).
    glm::vec3 tint{0.0F};
    /// First atlas cell of the model; -1 for shoulder riders ("SatShoulders_sit"), who get no sprite (0x82EDE0D0).
    int spriteCell = -1;
    bool sixSpriteViews = false;
    /// For riders, the set's first carrier at the same x and z, whose clip and frame they play (0x82EDDCE4).
    int leader = -1;
    /// Index into CrowdSet::groups().
    std::uint32_t group = 0;
    bool seated = false;
    /// Walkers' path in CrowdSet::walkPaths() (-1: not walking) and start, a draw of 0 to 1 of its stated length
    /// (0x82EC06D0).
    int walkPath = -1;
    float walkStart = 0.0F;
    /// The record's flags and last bytes (see PlacedSpectator).
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 3> extra {0, 0, 0};

    /// Unit direction faced.
    glm::vec3 facing() const;
    /// Spectator space to world.
    glm::mat4 transform() const;
};

/// One walker per path is this library's choice; the files do not say how many walk it.
struct CrowdWalkPath {
    std::uint32_t group = 0;
    WalkPath path;
};

/// Sets without an event block always show; free roam adds those listing route 0, a race those of its event or
/// route. Inferred from the sets' data, not game code.
struct CrowdSession {
    bool race = false;
    std::string eventId;
    int routeId = 0;
    /// Free roam only: HorizonEventIDs whose tagged sets show; empty shows them all.
    std::vector<std::string> offeredEvents;

    static CrowdSession freeRoam(std::vector<std::string> offeredEvents = {});
    static CrowdSession raceOn(std::string eventId, int routeId);
    bool shows(const CrowdGroup& group) const;
};

/// A vertical quad standing on `bottomCentre`, turned to the camera.
struct SpriteBillboard {
    SpriteFrame frame;
    glm::vec3 bottomCentre {0.0F};
    float width = 0.0F;
    float height = 0.0F;
};

/// What a load found and what it could not read.
struct CrowdLoadReport {
    int placementSets = 0;
    int skippedEmptySets = 0;
    int unknownClassInstances = 0;
    std::optional<LegacyCrowdFile> legacyFile;
    /// Files that could not be read or entries that name missing data.
    std::vector<std::string> failures;
    std::vector<std::string> warnings;
};

/// The crowd of one track: people, meshes, textures, clips and placements.
class CrowdSet {
public:
    /// Throws DataError when spectators.xml or the track's archive is missing; unreadable files go to
    /// report().failures.
    static CrowdSet load(const Vfs& vfs, std::string_view track);

    const SpectatorConfig& config() const { return m_config; }
    const std::vector<Spectator>& spectators() const { return m_spectators; }
    const std::vector<CrowdGroup>& groups() const { return m_groups; }
    const std::vector<CrowdClass>& classes() const { return m_classes; }
    const std::vector<SpectatorModel>& models() const { return m_models; }
    const std::vector<CrowdSkeleton>& skeletons() const { return m_skeletons; }
    const std::vector<CrowdWalkPath>& walkPaths() const { return m_walkPaths; }
    const std::vector<SpectatorAnimation>& animations() const { return m_animations; }
    const std::vector<SpectatorTexture>& textures() const { return m_textures; }
    /// Indices into textures(), -1 when the file could not be read.
    int spriteTexture() const { return m_spriteTexture; }
    int shadowTexture() const { return m_shadowTexture; }
    const SpriteAtlasLayout& spriteLayout() const { return m_spriteLayout; }
    const SpriteMetrics& spriteMetrics() const { return m_spriteMetrics; }
    const CrowdLoadReport& report() const { return m_report; }

    /// The animation named `name` (ignoring case), or -1.
    int findAnimation(std::string_view name) const;

    /// Horizontal distance, nearest first.
    std::vector<std::uint32_t> spectatorsNear(float x, float z, float radius) const;
    /// Groups `session` shows.
    std::vector<std::uint32_t> groupsFor(const CrowdSession& session) const;
    /// Thinned to about `crowdSize` (0 to 1) by a per-person hash, so a smaller crowd is a subset of a larger.
    /// Events.CrowdSize is 1.0 for every event on the disc.
    std::vector<std::uint32_t> spectatorsFor(const CrowdSession& session, float crowdSize = 1.0F) const;

    /// In spectator space, looping. Throws DataError when the clip's skeleton differs from the model's.
    std::vector<glm::mat4> skinMatrices(std::uint32_t model, std::uint32_t animation, float time) const;
    /// Skinned like skinMatrices(), in vertex order.
    std::vector<glm::vec3> skinnedPositions(std::uint32_t model, std::uint32_t animation, float time) const;
    /// For the models of bindSkeleton(animation). `out` needs one entry per bone of the clip.
    void frameSkinMatrices(std::uint32_t animation, int frame, std::span<glm::mat4> out) const;
    /// The first skeleton with the suffix of the first skeleton listing the clip.
    std::uint32_t bindSkeleton(std::uint32_t animation) const;

    /// Walkers move along their path at kWalkingSpeed; others stay at their placement.
    glm::vec3 walkerAt(std::uint32_t spectator, float time, float& heading) const;
    /// Metres a second (0x82EDD9E0, applied per frame at 0x82EC0630).
    static constexpr float kWalkingSpeed = 1.0F;
    /// The sprite to draw for person `spectator` seen from `camera`.
    SpriteBillboard spriteFor(std::uint32_t spectator, const glm::vec3& camera) const;

private:
    void loadAnimations(const Vfs& vfs);
    void loadModels(const Vfs& vfs);
    void loadSprites(const Vfs& vfs);
    void resolveClasses();
    void loadPlacements(const Vfs& vfs, std::string_view track);
    void placeSpectators(const std::vector<CrowdPlacementSet>& sets, const std::vector<std::string>& files);
    std::uint32_t addTexture(const Vfs& vfs, const std::string& name, const std::string& path, bool xds);

    SpectatorConfig m_config;
    std::vector<Spectator> m_spectators;
    std::vector<CrowdGroup> m_groups;
    std::vector<CrowdClass> m_classes;
    std::vector<SpectatorModel> m_models;
    std::vector<CrowdSkeleton> m_skeletons;
    std::vector<SpectatorAnimation> m_animations;
    std::vector<CrowdWalkPath> m_walkPaths;
    std::vector<SpectatorTexture> m_textures;
    std::unordered_map<std::string, std::uint32_t> m_textureByPath;
    int m_spriteTexture = -1;
    int m_shadowTexture = -1;
    SpriteAtlasLayout m_spriteLayout;
    SpriteMetrics m_spriteMetrics;
    CrowdLoadReport m_report;
    /// People by grid cell (kCellSize metres), keyed by packed cell.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_grid;
};

/// Missing mip levels are filtered from the top one. Throws DataError on failure.
world::TextureMipChain loadXdsMipChain(ByteView data, const std::string& source);
/// DXT1, DXT3 or DXT5 (`shadow.dds` is a PC DDS). Throws DataError on failure.
world::TextureMipChain loadDdsMipChain(ByteView data, const std::string& source);

} // namespace openhorizon::spectators
