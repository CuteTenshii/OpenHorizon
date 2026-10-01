#pragma once

#include "base/Bytes.h"

#include <glm/vec3.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

struct ScatterSet;

/// Where one draw record places its render object.
struct Placement {
    /// World position of the model's origin.
    glm::vec3 position{0.0F};
    /// On file coordinates (Z not negated): world = x·row0 + y·row1 + z·row2 + position; rows may carry scale.
    /// Row 0 is CollObjs.xml's XAxis, row 2 its negated ZAxis; world-space models have identity with Z mirrored.
    std::array<float, 9> rows{};
    /// Metres, negative when unused; likely the next-LOD and stop-drawing distances. Inferred.
    std::array<float, 3> ranges{};
    /// An event block (u32 0xFFFFFFFF), whose routes and tag decide when it shows; breakables have other values.
    /// Verified: three gates (0x82E3F980); the route gate (0x82E3FA00, 0x82E439B0, rerun 0x82E439D8, same test
    /// 0x82CECF78) shows the prop if its list is empty or holds the loaded route, or the route is -1.
    bool eventProp = false;
    /// A HorizonEventID ("FR04"), optionally suffixed ("FR04_01", "FR04_NODE"), or a barn find, gas station or
    /// outpost name; often empty. Activation zones show and hide props by it (see ActivityProps).
    std::string eventTag;
    /// Tracks.RouteId values (gamedb); 0 is free roam. Verified: missions.xml load_freeroam_route and 0x8298D2D8
    /// load route 0, and ColoradoFreeRoam (Tracks 317) has RouteId 0.
    std::vector<std::uint8_t> eventRoutes;
    /// The `Ribbon_00/GameObjs.xml` GameplayID a non-event block names ("flyer_001", "speed_camera_NNN",
    /// "average_speed_NNN"). Verified on Colorado against the GameObjs.xml positions and axes.
    std::string gameplayId;

    /// By eventTag, ignoring case.
    bool belongsToEvent(std::string_view eventId) const;
    /// The route gate (see eventProp): the list is empty or holds `routeId`.
    bool onRoute(int routeId) const;

    /// Takes a parsed point (Z already negated) to world coordinates.
    glm::vec3 apply(const glm::vec3& parsed) const;
};

/// Where a track places each draw record's model, read from its zone files
/// (`__R00Z#####.pvsz` in bin.zip, big-endian). A zone file holds, each as a
/// u32 count followed by its entries:
///
///     n u32 draws: the low 16 bits index the PVS file's draw records
///     u16 values, 16-byte entries, u32 values, bytes, u16 values, u32
///     values, bytes, 16-byte entries, n bytes     (not needed here)
///     n transform records, one per draw above, in the same order:
///         3 half floats (ranges), 3 f32 position, 9 half floats (rows),
///         16 zero bytes, u8 c, then c blocks of u32 a, u8 m, m bytes
///         and a 32-byte NUL-padded name; a is 0xFFFFFFFF for event props
///         (see Placement::eventProp), whose m bytes are
///         Placement::eventRoutes and whose name, when not empty, is
///         Placement::eventTag; in other blocks a non-empty name is
///         Placement::gameplayId
///
/// A zone file lists the draws visible from that zone (see ZoneGrid), so a draw appears in several with the same
/// transform; each LOD is its own draw. Inferred from the data; reads every Colorado zone to its end.
/// Trees, bushes, rocks and many fences come from procedural sets instead (see ScatterSet).
class TrackPlacements {
public:
    /// Runs on all hardware threads. Throws DataError if the PVS file cannot be read; unreadable zones and sets
    /// are counted. Nothing if `cancel` was set.
    static std::optional<TrackPlacements> load(
        ByteView pvs, const lzx::ZipArchive& archive, const std::atomic<bool>* cancel = nullptr);

    TrackPlacements();
    ~TrackPlacements();
    TrackPlacements(TrackPlacements&&) noexcept;
    TrackPlacements& operator=(TrackPlacements&&) noexcept;
    TrackPlacements(const TrackPlacements&) = delete;
    TrackPlacements& operator=(const TrackPlacements&) = delete;

    /// Throws DataError if the zone file does not have the layout above.
    static std::vector<std::pair<std::uint32_t, Placement>> readZone(ByteView zone);

    std::size_t drawCount() const { return m_drawObjects.size(); }
    /// The render object that draw record `draw` places.
    std::uint16_t drawObject(std::size_t draw) const { return m_drawObjects[draw]; }
    /// Where draw record `draw` is placed, or nullptr if no zone says.
    const Placement* placement(std::size_t draw) const;
    /// In increasing order; zone n is `__R00Z<n>.pvsz`.
    const std::vector<std::uint16_t>& zonesListing(std::size_t draw) const;
    int zoneCount() const { return m_zones; }
    int failedZones() const { return m_failedZones; }

    /// Kind 7 procedural sets, event-gated ones included.
    const std::vector<ScatterSet>& scatterSets() const { return m_scatterSets; }
    /// Copies placed by all procedural sets together.
    std::size_t scatterInstanceCount() const;
    /// Sets that could not be read.
    int failedScatterSets() const { return m_failedScatterSets; }
    /// A template the procedural sets copy, not drawn at its own placement.
    bool isScatterTemplate(std::size_t draw) const;

private:
    std::vector<std::uint16_t> m_drawObjects;
    std::vector<std::optional<Placement>> m_placements;
    std::vector<std::vector<std::uint16_t>> m_zonesListing;
    int m_zones = 0;
    int m_failedZones = 0;
    std::vector<ScatterSet> m_scatterSets;
    int m_failedScatterSets = 0;
    std::vector<bool> m_scatterTemplates;
};

} // namespace openhorizon::world
