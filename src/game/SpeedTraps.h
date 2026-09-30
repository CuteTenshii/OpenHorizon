#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class ActivityCatalogue;
class GameObjects;

/// One speed trap: an ActivitySpeedCamera of the game mode
/// (media/gamemodes/<mode>/speed_camera.xml), whose camera posts are the
/// GameObjs.xml objects `<name>_left` and `<name>_right`.
struct SpeedTrap {
    std::string name;
    int scoreboardId = -1;
    float minimumSpeedMph = 0.0F;
    glm::vec3 left{0.0F};
    glm::vec3 right{0.0F};
    /// Passed at or above the minimum speed at least once.
    bool discovered = false;
    /// The fastest pass so far, in metres per second.
    std::optional<float> best;

    /// Where the activity puts its map entity: halfway between the posts
    /// (0x82973ba8 takes 0.5 of their sum).
    glm::vec3 centre() const { return (left + right) * 0.5F; }
    /// The List_SpeedCameras string naming the trap on the map
    /// ("IDS_SpeedCamera_%i" with the scoreboard id, 0x82976a74).
    std::string nameStringId() const;
};

/// A pass through a speed trap that counted.
struct SpeedTrapPass {
    std::size_t trap = 0;
    /// The car's speed through the gate, metres per second.
    float speed = 0.0F;
    /// The trap's best before this pass, metres per second; 0 before the
    /// first, as a fresh profile's record reads.
    float previousBest = 0.0F;
};

/// The track's speed traps and what the player has done at them this
/// session.
///
/// Verified in the executable: the activity finds its posts as "%s_left"
/// and "%s_right" (0x82973be4, 0x82973c1c) and sets two triggers between
/// them, one per direction (0x82973e04, 0x82973e44). When one fires
/// (0x8296ce18) the activity takes the length of the player's car's
/// velocity, multiplies it by 2.2369 (metres per second to miles per hour)
/// and does nothing below the activity's minimumSpeed; otherwise it posts
/// the speed to its scoreboard, marks the trap discovered the first time
/// (0x82958398, which also shows its "speed_camera_discovered" map entity)
/// and hands the HUD a CHudStatusSpeedCamera with the speed in metres per
/// second and the profile's previous best (0x82979af0).
///
/// Inferred: the triggers are boxes spanning the gap between the posts
/// (0x8296d1b8 builds them with VMX128 code the disassembler does not
/// decode); a pass is taken as the car's ground point crossing the segment
/// between the posts on the ground plane, whatever its height. Also
/// inferred: the previous best shown is the trap's best before the pass.
/// Missing: the leaderboards (online), the "completed" state and its map
/// entity, the tutorial within Activity/SpeedCamera/TutorialRadius, and
/// the FirstSpeedTrap100MPH award.
class SpeedTraps {
public:
    /// The catalogue's ActivitySpeedCamera activities whose two posts
    /// GameObjs.xml has; others are left out, as the activity's setup fails
    /// without them.
    static SpeedTraps build(const ActivityCatalogue& catalogue, const GameObjects& objects);

    /// The car's ground point moved from `from` to `to` with `velocity`
    /// (metres per second). Returns the pass when that crossed a trap's
    /// gate fast enough, and records it.
    std::optional<SpeedTrapPass> update(const glm::vec3& from, const glm::vec3& to, const glm::vec3& velocity);

    const std::vector<SpeedTrap>& traps() const { return m_traps; }
    const SpeedTrap* find(std::string_view name) const;
    /// Sets trap `name` as the profile saved it: discovered or not and its
    /// personal best in metres per second, 0 standing for none as in a
    /// fresh CSpeedCameraState. False when there is no such trap.
    bool restore(std::string_view name, bool discovered, float personalBest);
    /// Passes counted so far this session.
    std::size_t passCount() const { return m_passes; }
    /// The latest counted pass, if any.
    const std::optional<SpeedTrapPass>& lastPass() const { return m_last; }

private:
    std::vector<SpeedTrap> m_traps;
    std::size_t m_passes = 0;
    std::optional<SpeedTrapPass> m_last;
};

} // namespace openhorizon
