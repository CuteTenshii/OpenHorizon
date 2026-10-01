#pragma once

#include "base/Bytes.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openhorizon::spectators {

/// One spectator as a crowd placement set stores it.
struct PlacedSpectator {
    /// World position of the feet (seated classes: the seat).
    glm::vec3 position {0.0F};
    /// Facing as stored: b / 255 of a full turn (see headingRadians()).
    std::uint8_t heading = 0;
    /// spectators.xml's `id` minus one. Verified.
    std::uint8_t crowdClass = 0;
    /// `flags` 1: billboard-only, skipped by the cull (0x82ECAB38), a negative size in the sprite packer
    /// (0x82EDE0D8). `extra`: RGB tint, byte / 255. Verified in the decoder (0x82EB99E0).
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 3> extra {0, 0, 0};

    /// Radians clockwise from north (+Z): the person looks along (sin h, 0, cos h) (0x82EB99E0; sine 0x82420E30,
    /// cosine 0x8242EBA8). Verified against the racing lines.
    float headingRadians() const;
};

/// A path that walking people follow, from the second layout of crowd
/// placement sets (see CrowdPlacementSet).
struct WalkPath {
    /// As PlacedSpectator::crowdClass; always 3, "Walkers", on the disc.
    std::uint8_t crowdClass = 0;
    glm::vec3 boundsMin {0.0F};
    glm::vec3 boundsMax {0.0F};
    /// Length the file states, metres.
    float length = 0.0F;
    /// The points in walking order.
    std::vector<glm::vec3> points;
    /// Eight samples per stored segment, increasing from 0 to about `length`.
    std::vector<float> distances;

    /// Length of the closed polyline through `points`.
    float loopLength() const;
    /// Position `distance` metres along the closed polyline (wrapping), and
    /// the unit horizontal direction of travel there.
    glm::vec3 pointAt(float distance, glm::vec3* direction = nullptr) const;
};

/// A procedural set of type 3 (`__R00G#####.pgeo` in a track's bin.zip, big-endian). Layout, verified on every
/// crowd set of Colorado:
///
///     0x00  "OEGP", u32 42, f32 60, f32 -1
///     0x10  f32 min x, y, z, pad        bounds of the people
///     0x20  f32 max x, y, z, pad
///     0x30  u32 3 (set type), f32 -1, f32 -1, u32 name length (with NUL)
///     0x48  u32 file size
///     0x58  u32 non-zero when an event block follows
///     0x60  optional event block, 56 bytes: u32 0xFFFFFFFF, char[32] event
///           tag ("FR04_15", "FESTIVAL_FIRST_15"), u8 route count n, n x u8
///           route ids (Tracks.RouteId), padding
///           then the name ("crowd_proc_clrd_festival_crowd_fest_008_3"),
///           NUL-terminated, padded to 16 bytes from the file start
///     group f32 min x, y, z, pad; f32 max x, y, z, pad (bounds above grown by 1 m, and 1 m lower), u32 count,
///           28 unread bytes
///     count x 12-byte records: u16 x, u16 y, u16 z, fractions of 65535 of
///           the group bounds; u8 heading; u8 crowd class; u8 flags; u8[3]
///
/// Walker sets have no records (verified): empty group bounds (FLT_MAX, -FLT_MAX), path count p at +40, p 12-byte
/// entries (u32 0, u32 0, u32 class); then, 16-byte aligned, each path: f32 min x, y, z, pad; f32 max x, y, z,
/// pad; u32; u32 segment count s; f32 length; u32; s 96-byte segments: three points (f32 x, y, z, u32), 16 zero
/// bytes, f32[8] distances. Paths close to within a few metres. Inferred: a segment's points walk second, first,
/// third.
///
/// The event block works as world::Placement's: the tag is a GameObjs.xml id (HorizonEventID plus "_15"), the
/// routes the races (0: free roam) that show the set. Sets without it always show.
struct CrowdPlacementSet {
    std::string name;
    glm::vec3 boundsMin {0.0F};
    glm::vec3 boundsMax {0.0F};
    bool hasEventBlock = false;
    std::string eventTag;
    std::vector<std::uint8_t> routes;
    std::vector<PlacedSpectator> spectators;
    std::vector<WalkPath> walkPaths;

    /// The tag without its last "_suffix" ("FR04_15" gives "FR04"); empty without a tag.
    std::string eventId() const;
};

/// The set type at 0x30 of a procedural set, from its first 0x34 bytes;
/// nothing if the data is not one.
std::optional<std::uint32_t> placementSetType(ByteView prefix);

/// Type of crowd placement sets.
inline constexpr std::uint32_t kCrowdSetType = 3;
/// Bytes placementSetType() needs.
inline constexpr std::size_t kPlacementSetPrefixBytes = 0x34;

/// Throws DataError naming `source` if the data does not have the layout above.
CrowdPlacementSet parseCrowdPlacementSet(ByteView data, const std::string& source);

/// `Ribbon_00/<Track>_track_00.crowd` (big-endian): u32 version, u32 entry count, u32. Every track ships version 2
/// with no entries, so the entry layout is unknown; crowds come from the sets above.
struct LegacyCrowdFile {
    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t reserved = 0;
};

/// Throws DataError naming `source` if it is shorter than 12 bytes.
LegacyCrowdFile parseLegacyCrowdFile(ByteView data, const std::string& source);

} // namespace openhorizon::spectators
