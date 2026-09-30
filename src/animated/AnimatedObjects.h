#pragma once

#include "AnimatedSets.h"
#include "Granny.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace fh::animated {

/// An animated object ready to play: its set, its Granny file, and for each
/// animation, entry and bone the transform track that moves the bone.
struct AnimatedObject {
    /// Number of the set in its track (`__R00G<number>.pgeo`).
    std::uint32_t setNumber = 0;
    AnimatedObjectSet set;
    granny::FileInfo granny;
    /// tracks[animation][entry][bone]: index into the animation's group for
    /// that entry's model, -1 where no track names the bone. Tracks bind to
    /// bones by name, as their order differs from the skeleton's.
    std::vector<std::vector<std::vector<int>>> tracks;
    /// groups[animation][entry]: the track group that moves the entry's
    /// model (the one named after it), -1 for none.
    std::vector<std::vector<int>> groups;

    /// Builds the bindings. Throws DataError if an entry has no model.
    static AnimatedObject make(std::uint32_t setNumber, AnimatedObjectSet set, const std::string& source);
};

/// Where the camera is, and what the time of day and the loaded route are,
/// for AnimatedObjects::update().
struct AnimatedView {
    glm::vec3 camera{0.0F};
    /// Maps world positions to clip space, for the visibility test.
    glm::mat4 viewProjection{1.0F};
    /// The time-of-day SwitchOnLights curve as sampled (0 by day).
    float switchOnLights = 0.0F;
    /// The loaded route (world::Placement::eventRoutes), -1 for any.
    int route = 0;
};

/// The track's animated objects, where they stand and how far each has
/// played.
///
/// How the game runs them, verified in the executable unless said:
///  - Placements: every kind-5 set places objects of the track's kind-4 sets
///    (AnimatedPlacementSet). Sets with an event block are gated by the
///    loaded route as zone-file props are (world::Placement::onRoute);
///    that the same gate applies is inferred, as the block is parsed by the
///    shared header parser. On Colorado 97 sets carry one, none of whose
///    route lists holds free roam's route 0: the air dancers and
///    inflatables of race routes.
///  - Visibility (0x82EC85A8): an instance is drawn when its bounds (the
///    set's box through the placement, 0x82EB8970) are in view and it is
///    no farther than its cull distance; its level of detail is 0 up to
///    the first switch distance, 1 up to the second, else 2. The game
///    measures from the camera to the placement (inferred: the distance
///    code's operands were not traced), and chooses the level in one view
///    (view 9) whose relation to the main camera was not traced.
///  - Clock (0x82427848, reached once a frame through 0x8241D3B0): each
///    visible instance's clock advances by the presentation's time step,
///    the same as TrackFrameConstants::timeGain's, which is 0 while the
///    game is paused (0x824217E0, 0x82491608). Instances out of view keep
///    their clock and resume where they stopped.
///  - Clips: an instance with no clip playing that is ready (below) and,
///    when its trigger distance is not negative, no farther than it, picks
///    one of its animations at random with a random phase within its
///    duration, and plays it looping for ever (0x82EB8638, loop count 0).
///    The game's random numbers (0x82DBE490, 0x82DBE4F0) are not
///    reproduced: the choice here comes from a hash of the instance, so a
///    load always looks the same.
///  - Ready (0x8241FE60): the time-of-day gate against the SwitchOnLights
///    curve (taken as -1 when 0, 0x8265AFB8): an instance with a night
///    threshold above 0 plays only while the curve is at least that (the
///    fireworks, stage lights, lasers), one with a day threshold below 0.95
///    only while the curve is at most that (the tractors and the crop
///    duster). An instance that stops being ready drops its clip; drawn
///    then, it shows its rest pose (inferred: the draw does not read the
///    flag).
///  - Instances placed as shared (placement +80 = 0) play the set's single
///    instance.
/// Missing: the 0.5 s ease-in of a clip picked after the first frame (the
/// blend curve was not traced); the attached sounds and particle effects
/// (batch strings `Audio: ...`, the effect queue 0x82EB9328); the objects
/// the gameplay spawns by name from media/animatedobjects (checkpoint
/// lasers, start gantry sparks, confetti), whose triggers were not traced.
class AnimatedObjects {
public:
    struct Instance {
        /// Index into objects().
        std::uint32_t object = 0;
        AnimatedPlacement placement;
        /// Maps the object's coordinates to the world's.
        glm::mat4 world{1.0F};
        /// The world box of the object's bounds.
        glm::vec3 boundsMin{0.0F};
        glm::vec3 boundsMax{0.0F};
        bool eventProp = false;
        std::vector<std::uint8_t> eventRoutes;
        /// Index into the play states: the instance's own, or the one its
        /// set shares.
        std::uint32_t state = 0;
    };

    /// What update() left for the renderer about one instance.
    struct Visible {
        std::uint32_t instance = 0;
        /// Level of detail, 0 finest.
        int lod = 0;
        /// First of the instance's composite bone matrices (see pose()).
        std::uint32_t firstMatrix = 0;
    };

    /// Reads the kind-4 and kind-5 sets of a track's archive on all hardware
    /// threads. Sets that cannot be read are counted in failedSets().
    /// Returns nothing if `cancel` was set while it ran.
    static std::optional<AnimatedObjects> load(
        const lzx::ZipArchive& archive, const std::atomic<bool>* cancel = nullptr);

    /// Builds the instances of `placements` over `objects`, leaving out
    /// placements whose object is missing (counted in missingObjects()).
    AnimatedObjects(std::vector<AnimatedObject> objects, const std::vector<AnimatedPlacementSet>& placements);

    const std::vector<AnimatedObject>& objects() const { return m_objects; }
    const std::vector<Instance>& instances() const { return m_instances; }
    int failedSets() const { return m_failedSets; }
    int missingObjects() const { return m_missingObjects; }

    /// Culls the instances for `view`, advances the clocks of the visible
    /// ones by `dt` seconds of presentation time and poses them.
    void update(float dt, const AnimatedView& view);

    /// The instances update() found visible, with their level of detail.
    const std::vector<Visible>& visible() const { return m_visible; }
    /// The composite matrices of the visible instances: for each entry of
    /// its object, one per bone of the entry's model (entries in order),
    /// mapping the object's rest-pose coordinates to its posed ones.
    const std::vector<glm::mat4>& pose() const { return m_pose; }
    /// Offset of entry `entry`'s matrices from a Visible's firstMatrix.
    std::uint32_t entryMatrixOffset(std::uint32_t object, std::size_t entry) const
    {
        return m_entryOffsets[object][entry];
    }

    /// The seconds instance `instance` has played its clip (its clip's
    /// local time before wrapping), and the clip, -1 when none plays.
    float clipTime(std::size_t instance) const;
    int clip(std::size_t instance) const;

    /// The local transforms of `entry`'s bones at `time` seconds into
    /// animation `animation` of `object`, from the skeleton's rest pose and
    /// the tracks; -1 gives the rest pose.
    static void sampleLocals(const AnimatedObject& object, int animation, std::size_t entry, float time,
        std::vector<granny::Transform>& out);

private:
    struct PlayState {
        /// Seconds of presentation time played since the clip started.
        double elapsed = 0.0;
        int clip = -1;
        float phase = 0.0F;
        std::uint32_t seed = 0;
        std::uint32_t picks = 0;
    };

    std::vector<AnimatedObject> m_objects;
    std::vector<Instance> m_instances;
    std::vector<PlayState> m_states;
    /// Per object, the offset of each entry's matrices.
    std::vector<std::vector<std::uint32_t>> m_entryOffsets;
    /// Per object, the matrices of all its entries.
    std::vector<std::uint32_t> m_matrixCounts;
    std::vector<Visible> m_visible;
    std::vector<glm::mat4> m_pose;
    std::vector<std::uint8_t> m_advanced;
    std::vector<granny::Transform> m_locals;
    std::vector<glm::mat4> m_world;
    int m_failedSets = 0;
    int m_missingObjects = 0;
};

} // namespace fh::animated
