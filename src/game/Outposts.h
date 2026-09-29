#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fh {

class ActivityCatalogue;
class GameObjects;

/// One Horizon outpost: an ActivityGasStation of the game mode
/// (media/gamemodes/<mode>/gas_stations.xml). The Activities string
/// IDS_Description_GasStation, the prompt title of every one, reads
/// "HORIZON OUTPOST".
struct Outpost {
    std::string name;
    /// The zone's GameObjs.xml object ("GASSTATION_001") and where it is.
    std::string object;
    glm::vec3 position{0.0F};
    /// The activation zone: its radius (metres), speed limit (metres per
    /// second) and prompt texts (Activities string ids).
    float radius = 0.0F;
    std::optional<float> maxSpeed;
    std::string promptTitle;
    std::string prompt;
    /// The Activities string naming it on the map.
    std::string mapDescription;
    /// The discovery zone: `<object>_DISCOVERY`, or the zone's own object
    /// when there is none (0x8296e858), and its radius (triggerZoneRadius).
    glm::vec3 discoveryPosition{0.0F};
    float discoveryRadius = 0.0F;
    /// The missions it offers (`<Missions><Mission name>`).
    std::vector<std::string> missions;
    bool discovered = false;
};

/// The track's Horizon outposts and which the player has found.
///
/// Verified in the executable: each outpost builds a discovery trigger
/// around `<object>_DISCOVERY` with the zone's triggerZoneRadius on all
/// three axes (0x8296e858). Entering it the first time (0x82972f50) marks
/// the outpost discovered, posts the notification Missions
/// IDS_DiscoverGasStation_Title "HORIZON OUTPOST DISCOVERED" with
/// IDS_DiscoverGasStation_Msg1 "NEW FAST TRAVEL DESTINATION UNLOCKED" for
/// Notifications/DurationsInSeconds/GasStationDiscovered (10 s, the
/// executable's default; globalregistry.xml does not set it), and enables
/// the activation zone; discovery also creates the outpost's "gas_station"
/// map entity at the zone's object (0x82972da0). The activation zone is the
/// TriggerZone (radius, maxMPH, promptTitle, prompt; is_activation false, so
/// it shows no set dressing).
///
/// Inferred: the discovery trigger is a sphere (its three equal extents),
/// and so is the activation zone. Missing: what entering the zone opens
/// (CShowGasStationScreen: fast travel for Activities/Outposts/TotalCost
/// credits less the Discount_N_Missions share, and the outpost's missions),
/// the discovery notification on the HUD (the combined notification
/// control's queue was not traced), the PR hub tutorial within
/// tutorialRadius and its cutscene.
class Outposts {
public:
    /// The catalogue's ActivityGasStation activities whose zone object
    /// GameObjs.xml has.
    static Outposts build(const ActivityCatalogue& catalogue, const GameObjects& objects);

    /// The car is at `position`. Returns the outpost it discovered by being
    /// there, the first time only.
    std::optional<std::size_t> updateDiscovery(const glm::vec3& position);
    /// The discovered outpost whose activation zone holds a car at
    /// `position` moving at `speed` (metres per second), if any.
    std::optional<std::size_t> zoneAt(const glm::vec3& position, float speed) const;

    const std::vector<Outpost>& all() const { return m_outposts; }
    std::size_t discoveredCount() const;

private:
    std::vector<Outpost> m_outposts;
};

} // namespace fh
