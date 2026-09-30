#pragma once

#include "ProfileSchema.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh::profile {

/// The game keeps what the player has done in free roam in a tree of named
/// state objects (namespace HorizonGameState), which its profile object owns
/// (+36, created at 0x825831f8) and saves after the property bags into the
/// same stream (0x825659e8 serializes the bags, then this tree and the rally
/// expansion's through 0x825bdb70). Each node's serializer takes the stream
/// and the version the node was saved with and fails for any other version.
///
/// Verified: a container node (0x825bc978, version 2) writes its child count
/// and then, per child, the child's name, its type's registered name
/// ("CFlyerState" and so on, 0x8201f858 on), its version, its byte size and
/// its data; reading, it creates the child by type name and skips to the
/// saved size. The tree's entry (0x825bdb70) writes the version 2 first. In
/// the binary stream (IOSys::CBinaryStream, vtable 0x8223e724) a Bool is
/// one byte and an Int32, UInt32 or Float32 four.
///
/// The leaf states this engine has systems for are the ones below; the
/// others (CPlayerState, CMissionState, CBarnFindState, the challenge and
/// career states) are read and written back unchanged.

/// CActivityState's one field, which CGasStationState (vtable 0x82021404)
/// uses as it is (serializer 0x82598360, version 1): the activity was
/// discovered. Verified: an activity's discovery (0x82956a20, which the
/// outpost's discovery 0x82972ec8 calls) sets it through 0x82c65210, a store
/// to +40.
struct GasStationState {
    static constexpr std::string_view kType = "CGasStationState";
    static constexpr int kVersion = 1;
    bool discovered = false;
};

/// CSpeedCameraState (vtable 0x82021414, serializer 0x82597e80, version 1).
/// Verified: +40 is CActivityState's discovered flag (above); +44 is set by
/// the first pass at speed (0x82958398, which also shows the trap's
/// "speed_camera_discovered" map entity); +48 is the personal best, in
/// metres per second: a pass faster than it (0x8296f728 compares the car's
/// speed in m/s) stores the speed there (0x82c1fa30). A fresh state has all
/// three zero (0x82597e60). When the game sets +40 for a speed trap was not
/// traced, so it is kept as loaded.
struct SpeedCameraState {
    static constexpr std::string_view kType = "CSpeedCameraState";
    static constexpr int kVersion = 1;
    bool activityDiscovered = false;
    bool discovered = false;
    float personalBest = 0.0F;
};

/// CFlyerState (vtable 0x82020ee8, serializer 0x82597de8, version 1):
/// CActivityState's discovered flag (+40), then whether the sign was
/// smashed (+44), which is what the collected count reads (the predicate
/// 0x8297a2f8 over the CFlyerState objects, 0x8298ac58). When the game sets
/// +40 for a sign was not traced, so it is kept as loaded.
struct FlyerState {
    static constexpr std::string_view kType = "CFlyerState";
    static constexpr int kVersion = 1;
    bool activityDiscovered = false;
    bool collected = false;
};

/// CRoadDiscovery (vtable 0x8201f934, serializer 0x8259b800, version 2),
/// the node "RoadDiscovery" under "Player0" (0x825bcecc): which nodes of
/// the road network have been counted. Verified: it saves the node count
/// and then one bit per node, 32 to a word, bit i of word k for node
/// 32k + i, set when the node's visit count is not zero; loading sets the
/// count of each node to its bit. So visit counts above one are not kept.
struct RoadDiscoveryState {
    static constexpr std::string_view kType = "CRoadDiscovery";
    static constexpr std::string_view kName = "RoadDiscovery";
    static constexpr int kVersion = 2;
    std::vector<bool> visited;
};

/// A state of a type this engine has no system for, kept as read so that
/// saving writes it back: its name, the game's type name and version, and
/// its fields as properties in the order read.
struct OtherState {
    std::string name;
    std::string type;
    int version = 0;
    std::vector<Property> fields;
};

/// The game state this engine reads and writes. Activity states are keyed by
/// the activity's name: the activity asks for its state with its name
/// string (+16, passed to 0x82965530 at 0x82972e7c); that the name is the
/// node's name in the tree is inferred from that call.
struct GameState {
    /// The tree's version (0x825bdb70 writes 2).
    static constexpr int kVersion = 2;
    std::optional<RoadDiscoveryState> roads;
    std::map<std::string, SpeedCameraState, std::less<>> speedCameras;
    std::map<std::string, FlyerState, std::less<>> flyers;
    std::map<std::string, GasStationState, std::less<>> gasStations;
    /// States of other types, in the order read.
    std::vector<OtherState> otherStates;
};

/// One player's profile: the schema's properties with their current values,
/// and the game state.
class Profile {
public:
    /// A profile with the schema's defaults and no game state, as a new
    /// profile starts.
    explicit Profile(const ProfileSchema& schema);

    const std::vector<Property>& bags() const { return m_bags; }
    std::vector<Property>& bags() { return m_bags; }
    const Property* find(std::string_view path) const { return findProperty(m_bags, path); }
    /// The value at `path`, or nothing when there is no such value.
    std::optional<PropertyValue> value(std::string_view path) const;
    /// Sets the value at `path`. False, and nothing changes, when there is
    /// no such value or `value` does not fit its type.
    bool set(std::string_view path, const PropertyValue& value);

    GameState state;

private:
    std::vector<Property> m_bags;
};

} // namespace fh::profile
