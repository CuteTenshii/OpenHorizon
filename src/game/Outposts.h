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

/// One Horizon outpost: an ActivityGasStation of gas_stations.xml.
struct Outpost {
    std::string name;
    std::string object;
    glm::vec3 position{0.0F};
    /// The activation zone, in metres and m/s. Verified: the prompt shows
    /// promptTitle and prompt as read (0x82980e70, 0x825c4a60, 0x828a25f0,
    /// 0x82981200).
    float radius = 0.0F;
    std::optional<float> maxSpeed;
    std::string promptTitle;
    std::string prompt;
    /// The Activities string naming it on the map.
    std::string mapDescription;
    /// `<object>_DISCOVERY`, else the zone's object (0x8296e858); radius is
    /// triggerZoneRadius.
    glm::vec3 discoveryPosition{0.0F};
    float discoveryRadius = 0.0F;
    std::vector<std::string> missions;
    bool discovered = false;
};

/// The track's Horizon outposts and which the player has found.
/// Verified: first entry into the discovery trigger (0x82972f50) marks it
/// discovered, notifies IDS_DiscoverGasStation_Title for 10 s, enables the
/// zone and adds the map entity (0x82972da0). Inferred: both zones are
/// spheres. Missing: CShowGasStationScreen, the HUD notification, the tutorial.
class Outposts {
public:
    /// Suffix of the prompt's SET_EVENTICON_* event. Verified: discovery sets
    /// icon 33 (0x82972dcc, 0x82c94f08), which maps to PRHUB (0x8289acf8,
    /// table 0x820775b8).
    static constexpr std::string_view kPromptIcon = "PRHUB";

    /// Outposts whose zone object is missing from GameObjs.xml are left out.
    static Outposts build(const ActivityCatalogue& catalogue, const GameObjects& objects);

    /// Returns an outpost the first time the car discovers it.
    std::optional<std::size_t> updateDiscovery(const glm::vec3& position);
    /// The discovered outpost whose zone holds the car; `speed` in m/s.
    std::optional<std::size_t> zoneAt(const glm::vec3& position, float speed) const;

    const std::vector<Outpost>& all() const { return m_outposts; }
    /// Marks outpost `name` discovered, as the profile saved it. False when
    /// there is no such outpost.
    bool restoreDiscovered(std::string_view name);
    std::size_t discoveredCount() const;

private:
    std::vector<Outpost> m_outposts;
};

} // namespace openhorizon
