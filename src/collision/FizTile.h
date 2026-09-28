#pragma once

#include "base/Bytes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fh::collision {

/// Flag bits of FizTriangle::flags. Every triangle has an even value; the
/// meanings are inferred from which triangles carry each bit, as the code
/// that reads them has not been traced.
namespace TriangleFlags {
/// Set on ground-like triangles (terrain, road tops) and clear on walls:
/// every triangle without it slopes by 30 degrees or more, and most with it
/// face up (inferred).
inline constexpr std::uint8_t kGround = 0x02;
/// Blocks the camera: set on every "CameraCollision" triangle and on most
/// terrain, clear on invisible walls and on some ground detail (inferred).
inline constexpr std::uint8_t kCamera = 0x04;
/// Blocks cars: clear only on "CameraCollision" triangles, which cars pass
/// through (inferred).
inline constexpr std::uint8_t kSolid = 0x08;
/// Common on invisible walls, safety plastic and wood; meaning unknown.
inline constexpr std::uint8_t kUnknown10 = 0x10;
/// Meaning unknown.
inline constexpr std::uint8_t kUnknown20 = 0x20;
} // namespace TriangleFlags

/// For FizTile::present() and QueryFilter::activeRoute: every route's
/// barriers count as present, as for inspecting the data.
inline constexpr int kAllRoutes = -2;

/// One collision triangle.
struct FizTriangle {
    /// Indices into FizTile::positions, wound so that
    /// cross(b - a, c - a) points along `normal`.
    std::array<std::uint16_t, 3> vertices{};
    /// Unit face normal as stored, pointing out of the solid side.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    /// TriangleFlags bits.
    std::uint8_t flags = 0;
    /// Index into the SurfaceTable (surfaceTypes.xml list order).
    std::uint8_t surface = 0;
    /// Which of FizTile::routes the triangle belongs to, one bit per list
    /// entry; kAlwaysPresent for ordinary world geometry.
    std::uint16_t routeMask = kAlwaysPresent;
    /// Per edge (vertex k to k+1): 255 where the edge is flat, concave or
    /// unmarked; on convex edges roughly 255·cos(half the angle between
    /// the two faces' normals), and near 180 on some open edges. Likely
    /// used to smooth contacts across edges (inferred).
    std::array<std::uint8_t, 3> edgeCosines{255, 255, 255};

    static constexpr std::uint16_t kAlwaysPresent = 0x8000;

    bool belongsToRoutes() const { return routeMask != kAlwaysPresent; }
};

/// The trailing `OWBB` block of a tile, kept as stored. It is not collision
/// geometry: the loader hands it to a different subsystem (0x82C12A90).
struct OwbbBlock {
    /// Name hash; 0x87D73394 in every Colorado tile.
    std::uint32_t hash = 0;
    /// Equals the tile header's word at 0x10.
    std::uint32_t id = 0;
    /// Cell of the track's collision grid (CollisionIndex), as in `.col`.
    std::uint32_t cell = 0;
    /// The block's bytes after its 16-byte header, up to its 16-byte
    /// trailer (which repeats the header).
    Bytes payload;
};

/// A decoded `.fiz` collision tile: the collision triangles around one
/// 100 m cell of a track's grid, in world coordinates (x east, y up, z as in
/// the track's placement files; unlike render models, Z is not negated).
///
/// Layout (big-endian; "A" is the section that starts at file offset 0x20,
/// and offsets written A+n are relative to it):
///
///     0x00  char[4] "fiz "                                       verified
///     0x04  u32 version, 1 (the loader rejects others)           verified
///     0x08  u32 size of section A (a multiple of 0x800)          verified
///     0x0C  u32 size of section B, 0 if absent                   verified
///     0x10  u32 id repeated in the OWBB block                    verified
///     0x14  12 zero bytes
///     A+00  u32 vertex count, u32 triangle count                 verified
///     A+08  u32 A-offset of the vertices, u32 of the triangles   verified
///     A+10  u32 grid cells along X, u32 along Z                  verified
///     A+18  f32 minX, minZ, maxX, maxZ: the cell's square grown by
///           the track's margin (8 m) on every side               verified
///     A+28  f32 grid cell width (X), depth (Z); 10 m             verified
///     A+30  f32 their inverses                                   verified
///     A+38  u32 A-offset of the grid's cell table                verified
///     A+3C  u16[30] route ids (see `routes`), then padding       verified
///     A+78  u32 route id count, at most 15                       verified
///     A+7C  u32 0x80000000 in every Colorado tile                unknown
///     A+80  16 zero bytes
///     vertices, 16 bytes each:
///       f32 x, y, z (world coordinates)                          verified
///       u32 unit vertex normal, packed 11:11:10 from the low
///           bit: x and y signed 11-bit over 1023, z signed
///           10-bit over 511                                      verified
///     triangles, 32 bytes each:
///       u8  flags (TriangleFlags)                                inferred
///       u8  surface id                                           verified
///       u16 route mask, 0x8000 for ordinary geometry             verified
///       u32 A-offset of each of the three vertices               verified
///       f32 normal x, y, z                                       verified
///       u8  0, then u8 per edge (see edgeCosines)                inferred
///     cell table: u32 per cell, row-major (z · cellsX + x), holding the
///       A-offset of the cell's list or 0 when it is empty       verified
///     cell lists: u16 count, u16 triangle indices, padded to 4   verified
///     section B (optional): an OWBB block:
///       char[4] "OWBB", u32 0x24, u32 hash, u32 payload size (B's size
///       minus 0x20), then the payload (u32 id, u32 cell, ...) and a copy
///       of the first 16 bytes                                    verified
///
/// A tile holds, whole, every triangle that touches its cell, and some that
/// only reach its margin, so triangles near a cell edge are in both tiles.
/// Every triangle is listed in at least one grid cell, and each list holds
/// the triangles whose bounds overlap its cell.
///
/// The tiles cover only the drivable area: the roads, the ground beside
/// them and the open spaces cars may enter, fenced by walls of the
/// "Invisible" surface. On Colorado about a quarter of the ground inside
/// the tiles' cells has collision; the rest of the rendered terrain has
/// none. Heights agree with the render geometry to within millimetres.
///
/// Route ids are the numbers of the track's race routes
/// (`Ribbon_00/TrackRouteNNN.xml`): the triangles of a route's barriers
/// match its checkpoints' extent. A triangle's mask bit k marks it as part
/// of route `routes[k]`, so the game can raise the barriers of the race
/// being driven; outside races they are presumably absent.
struct FizTile {
    std::uint32_t version = 1;
    /// XZ bounds (x = world X, y = world Z).
    glm::vec2 boundsMin{0.0F};
    glm::vec2 boundsMax{0.0F};
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> vertexNormals;
    std::vector<FizTriangle> triangles;
    /// Race route ids the route masks refer to.
    std::vector<std::uint16_t> routes;

    /// The triangle grid over the bounds, starting at boundsMin.
    std::uint32_t cellsX = 0;
    std::uint32_t cellsZ = 0;
    glm::vec2 cellSize{10.0F};
    /// Per cell, the start of its run in cellTriangles; cellsX·cellsZ + 1
    /// entries.
    std::vector<std::uint32_t> cellStart;
    std::vector<std::uint16_t> cellTriangles;

    std::optional<OwbbBlock> owbb;

    /// The id the header stores at 0x10.
    std::uint32_t headerId = 0;

    /// Triangles listed in grid cell (x, z).
    std::span<const std::uint16_t> cell(std::uint32_t x, std::uint32_t z) const
    {
        const std::size_t index = static_cast<std::size_t>(z) * cellsX + x;
        return std::span<const std::uint16_t>(cellTriangles).subspan(
            cellStart[index], cellStart[index + 1] - cellStart[index]);
    }
    /// Whether the triangle is present when `activeRoute` is being raced
    /// (-1: no race; kAllRoutes: every route at once).
    bool present(const FizTriangle& triangle, int activeRoute) const;
};

/// Decodes a `.fiz` file. `source` names it in errors. Throws DataError if
/// the data does not have the layout FizTile describes: bad magic or
/// version, sizes past the end, offsets or indices out of range.
FizTile parseFizTile(ByteView data, const std::string& source);

/// Unpacks a vertex normal stored as FizTile documents.
glm::vec3 unpackFizNormal(std::uint32_t packed);

} // namespace fh::collision
