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
    /// World position (X east, Y up, Z north), where the person's feet (or,
    /// for seated classes, seat) go.
    glm::vec3 position {0.0F};
    /// Facing as stored: b / 255 of a full turn (see headingRadians()).
    std::uint8_t heading = 0;
    /// Index of the crowd class: spectators.xml's `id` minus one (verified
    /// by the classes that come in pairs, such as SatShoulders_stand and
    /// _sit, whose instances coincide).
    std::uint8_t crowdClass = 0;
    /// The record's last four bytes. Verified in the game's person decoder
    /// (0x82EB99E0) and its callers: `flags` (0 or 1) marks a person the game
    /// only ever draws as a sprite (the "billboard-only" byte the crowd
    /// cull at 0x82ECAB38 skips and the sprite instance packer at
    /// 0x82EDE0D8 turns into a negative size); `extra` is the person's tint,
    /// red, green and blue, each byte / 255.
    std::uint8_t flags = 0;
    std::array<std::uint8_t, 3> extra {0, 0, 0};

    /// Direction faced, in radians clockwise from north (+Z) seen from above:
    /// the person looks along (sin h, 0, cos h), h = heading / 255 of a turn
    /// (the game's decoder, 0x82EB99E0, whose sine and cosine are 0x82420E30
    /// and 0x8242EBA8). Verified against the racing lines: of the standing
    /// people with one within 30 m, 93-95% face it (fh1crowd validate).
    float headingRadians() const;
};

/// A path that walking people follow, from the second layout of crowd
/// placement sets (see CrowdPlacementSet).
struct WalkPath {
    /// Crowd class index as PlacedSpectator::crowdClass (3, "Walkers", on
    /// every path of the disc).
    std::uint8_t crowdClass = 0;
    glm::vec3 boundsMin {0.0F};
    glm::vec3 boundsMax {0.0F};
    /// Length the file states, metres.
    float length = 0.0F;
    /// The points in walking order.
    std::vector<glm::vec3> points;
    /// Distance along the path of eight samples per stored segment,
    /// increasing from 0 to about `length`.
    std::vector<float> distances;

    /// Length of the closed polyline through `points`.
    float loopLength() const;
    /// Position `distance` metres along the closed polyline (wrapping), and
    /// the unit horizontal direction of travel there.
    glm::vec3 pointAt(float distance, glm::vec3* direction = nullptr) const;
};

/// A crowd placement set: a procedural set (`__R00G#####.pgeo` in a track's
/// bin.zip, magic "OEGP", big-endian) of type 3. On Colorado 1,783 of the
/// 7,970 sets are crowds, placing 149,126 people. Layout (verified on all of
/// them):
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
///     group f32 min x, y, z, pad; f32 max x, y, z, pad (the bounds above
///           grown by 1 m, and 1 m lower), u32 count, 28 bytes the game does
///           not read (the packing tool's pointers)
///     count x 12-byte records: u16 x, u16 y, u16 z, fractions of 65535 of
///           the group bounds; u8 heading; u8 crowd class; u8 flags; u8[3]
///
/// Five sets of Colorado (festival camp site, stalls, stages, garages, area
/// 3) have no records and hold walking paths instead (verified to fill the
/// files exactly): the group's bounds are empty (FLT_MAX, -FLT_MAX), its u32
/// at +40 is the path count p, and p 12-byte entries (u32 0, u32 0, u32 crowd
/// class) follow the group header; then, 16-byte aligned, each path: f32 min
/// x, y, z, pad; f32 max x, y, z, pad; u32 unused; u32 segment count s; f32
/// length; u32 unused; s segments of 96 bytes: three points (f32 x, y, z,
/// u32 not read here), 16 zero bytes, f32[8] distances along the path. The
/// walking order of a segment's points is second, first, third (inferred:
/// that order makes the points a chain about 2-10 m apart whose length
/// matches the stated one to within 2%). The paths close on themselves to
/// within a few metres.
///
/// The event block uses the same convention as the zone files' event props
/// (see world::Placement): the tag names a GameObjs.xml object whose id is
/// the event's HorizonEventID with a suffix ("_15" for crowds), and the
/// route ids list the races, or route 0 for free roam, that show the set.
/// Sets without the block are always shown. The quantisation and the layout
/// are verified; the flags byte and the three last bytes are the
/// billboard-only mark and the tint (see PlacedSpectator).
struct CrowdPlacementSet {
    std::string name;
    glm::vec3 boundsMin {0.0F};
    glm::vec3 boundsMax {0.0F};
    bool hasEventBlock = false;
    std::string eventTag;
    std::vector<std::uint8_t> routes;
    std::vector<PlacedSpectator> spectators;
    std::vector<WalkPath> walkPaths;

    /// The HorizonEventID in the tag: the tag without its last "_suffix"
    /// ("FR04_15" gives "FR04"); empty without a tag.
    std::string eventId() const;
};

/// The set type at 0x30 of a procedural set, from its first 0x34 bytes;
/// nothing if the data is not one.
std::optional<std::uint32_t> placementSetType(ByteView prefix);

/// Type of crowd placement sets.
inline constexpr std::uint32_t kCrowdSetType = 3;
/// Bytes placementSetType() needs.
inline constexpr std::size_t kPlacementSetPrefixBytes = 0x34;

/// Parses a crowd placement set. Throws DataError naming `source` if the
/// data does not have the layout above.
CrowdPlacementSet parseCrowdPlacementSet(ByteView data, const std::string& source);

/// A track's `Ribbon_00/<Track>_track_00.crowd` (big-endian): u32 version,
/// u32 entry count, u32. The loader in the executable reads it, but every
/// track of the disc ships version 2 with no entries (12 bytes), so the
/// layout of an entry is unknown and FH1 places its crowds with the sets
/// above instead.
struct LegacyCrowdFile {
    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t reserved = 0;
};

/// Parses the header. Throws DataError naming `source` if it is shorter
/// than 12 bytes.
LegacyCrowdFile parseLegacyCrowdFile(ByteView data, const std::string& source);

} // namespace openhorizon::spectators
