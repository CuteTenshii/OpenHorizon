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

/// An ActivitySpeedCamera of media/gamemodes/<mode>/speed_camera.xml; its posts are the GameObjs.xml objects
/// `<name>_left` and `<name>_right`.
struct SpeedTrap {
    std::string name;
    int scoreboardId = -1;
    float minimumSpeedMph = 0.0F;
    glm::vec3 left{0.0F};
    glm::vec3 right{0.0F};
    /// Passed at or above the minimum speed at least once.
    bool discovered = false;
    /// Metres per second.
    std::optional<float> best;

    /// Where the map entity goes (0x82973ba8).
    glm::vec3 centre() const { return (left + right) * 0.5F; }
    /// "IDS_SpeedCamera_%i" with the scoreboard id, in List_SpeedCameras (0x82976a74).
    std::string nameStringId() const;
};

/// A pass through a speed trap that counted.
struct SpeedTrapPass {
    std::size_t trap = 0;
    /// Metres per second.
    float speed = 0.0F;
    /// Metres per second; 0 before the first, as a fresh profile's record reads.
    float previousBest = 0.0F;
};

/// Verified: posts "%s_left" and "%s_right" (0x82973be4, 0x82973c1c), a trigger per direction between them
/// (0x82973e04, 0x82973e44); a pass below minimumSpeed mph counts for nothing (0x8296ce18), else it marks the
/// trap discovered (0x82958398) and goes to the HUD (0x82979af0).
/// Inferred: a pass is the ground point crossing the posts' segment (0x8296d1b8 is undecoded VMX128).
/// Missing: leaderboards, the "completed" state, the TutorialRadius tutorial and the FirstSpeedTrap100MPH award.
class SpeedTraps {
public:
    /// Activities missing a post in GameObjs.xml are left out, as the game's setup fails without them.
    static SpeedTraps build(const ActivityCatalogue& catalogue, const GameObjects& objects);

    /// `from` and `to` are the car's ground point; `velocity` in metres per second. Returns a counted pass.
    std::optional<SpeedTrapPass> update(const glm::vec3& from, const glm::vec3& to, const glm::vec3& velocity);

    const std::vector<SpeedTrap>& traps() const { return m_traps; }
    const SpeedTrap* find(std::string_view name) const;
    /// `personalBest` in metres per second, 0 for none as in a fresh CSpeedCameraState. False for an unknown trap.
    bool restore(std::string_view name, bool discovered, float personalBest);
    /// This session.
    std::size_t passCount() const { return m_passes; }
    const std::optional<SpeedTrapPass>& lastPass() const { return m_last; }

private:
    std::vector<SpeedTrap> m_traps;
    std::size_t m_passes = 0;
    std::optional<SpeedTrapPass> m_last;
};

} // namespace openhorizon
