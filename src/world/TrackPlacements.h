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
    /// 3x3 matrix, row by row, applied to the model's coordinates as stored
    /// in its file (Z not yet negated): world = x·row0 + y·row1 + z·row2 +
    /// position. The rows can carry scale as well as rotation. Row 0 is the
    /// XAxis of the matching CollObjs.xml entry and row 2 its negated ZAxis;
    /// world-space models have an identity with Z mirrored.
    std::array<float, 9> rows{};
    /// The first three values of the record, in metres; they look like the
    /// distances at which the game switches to the next level of detail and
    /// stops drawing the model. Negative when unused. Inferred from the data.
    std::array<float, 3> ranges{};
    /// True if the prop carries an event block (u32 0xFFFFFFFF), whose
    /// routes and tag decide when the game draws it: race barriers, chevrons
    /// and banners, festival set dressing, the forests of free roam, the
    /// open and closed states of barn-find barns. Breakable props (road
    /// signs, cones, bins) carry blocks with other values and are always
    /// there.
    ///
    /// Verified in the executable: the track keeps three gates for such
    /// blocks (0x82E3F980): one for blocks whose u32 is not 0xFFFFFFFF, one
    /// for route lists and one for tags. The route gate (0x82E3FA00, set up
    /// at 0x82E439B0) shows a prop when its list is empty, when the loaded
    /// route is -1, or when the list holds the loaded route, and hides it
    /// otherwise; it runs when the prop is added and, for props with a list,
    /// whenever the route changes (0x82E439D8). The same test is 0x82CECF78.
    /// See onRoute().
    bool eventProp = false;
    /// For some event props, the event that shows them: its HorizonEventID
    /// ("FR04"), or that ID with a suffix naming a part of its set dressing
    /// ("FR04_01", "FR04_NODE"). Barn finds, gas stations and outposts carry
    /// names of their own. Empty when the record names nothing, as for the
    /// props of street races and much of the festival. The activation zones
    /// show and hide props by these names (see ActivityProps).
    std::string eventTag;
    /// The routes the prop is drawn on (see eventProp): Tracks.RouteId in
    /// gamedb for a race's route, whose start gantries, barriers and
    /// chevrons every race run on it shares, and 0 for free roam. Verified:
    /// the free-roam flows load route 0 (media/gamemodes/missions.xml,
    /// load_freeroam_route; 0x8298D2D8 goes back to it), and Colorado's
    /// free-roam track row (Tracks 317, ColoradoFreeRoam) has RouteId 0.
    std::vector<std::uint8_t> eventRoutes;
    /// The GameplayID of the `Ribbon_00/GameObjs.xml` entry the prop stands
    /// for, named by a block that is not an event block: "flyer_001" to
    /// "flyer_100" (the discount signs), "speed_camera_NNN",
    /// "average_speed_NNN". Empty for other props. Verified on Colorado:
    /// each of the 100 flyer names is on the placements of one model only,
    /// at its GameObjs.xml position to the millimetre, with the object's
    /// XAxis as row 0 and its ZAxis as row 2 negated.
    std::string gameplayId;

    /// True if the prop belongs to the event whose HorizonEventID is
    /// `eventId`, by its eventTag (ignoring case).
    bool belongsToEvent(std::string_view eventId) const;
    /// Whether the route gate (see eventProp) shows the prop while route
    /// `routeId` is loaded: its list is empty or holds the route.
    bool onRoute(int routeId) const;

    /// Maps a point of the parsed model (whose Z the parser already negated)
    /// to world coordinates.
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
/// A zone file lists the draws the game draws while the camera is in that
/// zone (ZoneGrid says where each zone is), so zones overlap and a draw
/// appears in several; its transform is the same in each. Each level of
/// detail of a prop is a draw record of its own, with the same transform as
/// its other levels; about 2,500 draws of Colorado (mostly LOD00) are in no
/// zone file. The layout reads every zone of Colorado to its end and was
/// matched against CollObjs.xml and against the world-space models; it is
/// inferred from the data, not from game code.
///
/// Trees, bushes, rocks and many fences are not placed by zone files but by
/// procedural sets (see ScatterSet), which copy the models of template draw
/// records placed at the world origin.
class TrackPlacements {
public:
    /// Reads the draw table of `pvs` and every zone file and procedural set
    /// in `archive`, on all hardware threads. Throws DataError if the PVS
    /// file cannot be read; zone files and sets that cannot be read are
    /// counted in failedZones() and failedScatterSets(). Returns nothing if
    /// `cancel` was set while it ran.
    static std::optional<TrackPlacements> load(
        ByteView pvs, const lzx::ZipArchive& archive, const std::atomic<bool>* cancel = nullptr);

    TrackPlacements();
    ~TrackPlacements();
    TrackPlacements(TrackPlacements&&) noexcept;
    TrackPlacements& operator=(TrackPlacements&&) noexcept;
    TrackPlacements(const TrackPlacements&) = delete;
    TrackPlacements& operator=(const TrackPlacements&) = delete;

    /// Parses one zone file into (draw record, placement) pairs. Throws
    /// DataError if it does not have the layout above.
    static std::vector<std::pair<std::uint32_t, Placement>> readZone(ByteView zone);

    std::size_t drawCount() const { return m_drawObjects.size(); }
    /// The render object that draw record `draw` places.
    std::uint16_t drawObject(std::size_t draw) const { return m_drawObjects[draw]; }
    /// Where draw record `draw` is placed, or nullptr if no zone says.
    const Placement* placement(std::size_t draw) const;
    /// The zones whose files list draw record `draw`, in increasing order;
    /// zone n is `__R00Z<n>.pvsz`.
    const std::vector<std::uint16_t>& zonesListing(std::size_t draw) const;
    int zoneCount() const { return m_zones; }
    int failedZones() const { return m_failedZones; }

    /// The procedural model sets of the track (kind 7, see ScatterSet),
    /// those with an event block included.
    const std::vector<ScatterSet>& scatterSets() const { return m_scatterSets; }
    /// Copies placed by all procedural sets together.
    std::size_t scatterInstanceCount() const;
    /// Sets that could not be read.
    int failedScatterSets() const { return m_failedScatterSets; }
    /// True if draw record `draw` is a template the procedural sets copy,
    /// which the game does not draw where its own placement puts it.
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
