// Tests of the collision formats and queries: tiles, indexes and surface
// tables built here in the layouts the headers document, and, when
// FH1_GAME_DIR names an extracted disc, Colorado's own collision.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "collision/CollisionIndex.h"
#include "collision/FizTile.h"
#include "collision/SurfaceTable.h"
#include "collision/TrackCollision.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"

#include <glm/geometric.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using fh::Bytes;
namespace collision = fh::collision;
namespace Flags = fh::collision::TriangleFlags;

/// Builds big-endian test data.
struct Writer {
    Bytes data;

    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint16_t v)
    {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v)
    {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(std::string_view s) { data.insert(data.end(), s.begin(), s.end()); }
    void padTo(std::size_t size) { data.resize(std::max(size, data.size()), 0); }
    void alignTo(std::size_t alignment) { padTo((data.size() + alignment - 1) / alignment * alignment); }
    void patch32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        }
    }
};

struct TriangleSpec {
    std::array<glm::vec3, 3> corners;
    std::uint8_t flags = Flags::kGround | Flags::kCamera | Flags::kSolid;
    std::uint8_t surface = 0;
    std::uint16_t routeMask = collision::FizTriangle::kAlwaysPresent;
};

struct TileSpec {
    glm::vec2 boundsMin{0.0F};
    glm::vec2 boundsMax{116.0F};
    std::vector<std::uint16_t> routes;
    std::vector<TriangleSpec> triangles;
    bool owbb = false;
};

constexpr std::uint32_t kCells = 12;
constexpr float kCellSize = 10.0F;
constexpr std::size_t kSectionHeader = 0x90;

std::uint32_t packNormal(glm::vec3 n)
{
    const auto field = [](float v, float scale, int bits) {
        const auto value = static_cast<std::int32_t>(std::lround(v * scale));
        return static_cast<std::uint32_t>(value) & ((1U << bits) - 1U);
    };
    return field(n.x, 1023.0F, 11) | (field(n.y, 1023.0F, 11) << 11) | (field(n.z, 511.0F, 10) << 22);
}

/// A .fiz file in the documented layout: each triangle's corners become
/// vertices of their own, and triangles are listed in every grid cell their
/// bounds overlap.
Bytes buildFiz(const TileSpec& spec, std::uint32_t headerId = 7, std::uint32_t cell = 3)
{
    Writer a;
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(spec.triangles.size() * 3);
    const std::uint32_t triangleCount = static_cast<std::uint32_t>(spec.triangles.size());
    const std::uint32_t vertexOffset = kSectionHeader;
    const std::uint32_t triangleOffset = vertexOffset + vertexCount * 16;
    const std::uint32_t gridOffset = triangleOffset + triangleCount * 32;
    a.u32(vertexCount);
    a.u32(triangleCount);
    a.u32(vertexOffset);
    a.u32(triangleOffset);
    a.u32(kCells);
    a.u32(kCells);
    a.f32(spec.boundsMin.x);
    a.f32(spec.boundsMin.y);
    a.f32(spec.boundsMax.x);
    a.f32(spec.boundsMax.y);
    a.f32(kCellSize);
    a.f32(kCellSize);
    a.f32(1.0F / kCellSize);
    a.f32(1.0F / kCellSize);
    a.u32(gridOffset);
    for (const std::uint16_t route : spec.routes) {
        a.u16(route);
    }
    a.padTo(0x78);
    a.u32(static_cast<std::uint32_t>(spec.routes.size()));
    a.u32(0x80000000);
    a.padTo(kSectionHeader);

    std::vector<glm::vec3> normals;
    for (const TriangleSpec& t : spec.triangles) {
        const glm::vec3 n = glm::normalize(glm::cross(t.corners[1] - t.corners[0], t.corners[2] - t.corners[0]));
        normals.push_back(n);
        for (const glm::vec3& p : t.corners) {
            a.f32(p.x);
            a.f32(p.y);
            a.f32(p.z);
            a.u32(packNormal(n));
        }
    }
    for (std::uint32_t i = 0; i < triangleCount; ++i) {
        const TriangleSpec& t = spec.triangles[i];
        a.u8(t.flags);
        a.u8(t.surface);
        a.u16(t.routeMask);
        for (std::uint32_t k = 0; k < 3; ++k) {
            a.u32(vertexOffset + (i * 3 + k) * 16);
        }
        a.f32(normals[i].x);
        a.f32(normals[i].y);
        a.f32(normals[i].z);
        a.u32(0x00FFFFFF);
    }

    std::vector<std::vector<std::uint16_t>> lists(kCells * kCells);
    for (std::uint32_t i = 0; i < triangleCount; ++i) {
        glm::vec2 lo(1e9F);
        glm::vec2 hi(-1e9F);
        for (const glm::vec3& p : spec.triangles[i].corners) {
            lo = glm::min(lo, glm::vec2(p.x, p.z));
            hi = glm::max(hi, glm::vec2(p.x, p.z));
        }
        for (std::uint32_t z = 0; z < kCells; ++z) {
            for (std::uint32_t x = 0; x < kCells; ++x) {
                const glm::vec2 c0
                    = spec.boundsMin + glm::vec2(static_cast<float>(x), static_cast<float>(z)) * kCellSize;
                const glm::vec2 c1 = c0 + glm::vec2(kCellSize);
                if (lo.x <= c1.x && hi.x >= c0.x && lo.y <= c1.y && hi.y >= c0.y) {
                    lists[z * kCells + x].push_back(static_cast<std::uint16_t>(i));
                }
            }
        }
    }
    const std::size_t table = a.data.size();
    a.padTo(table + lists.size() * 4);
    for (std::size_t cellIndex = 0; cellIndex < lists.size(); ++cellIndex) {
        if (lists[cellIndex].empty()) {
            continue;
        }
        a.patch32(table + cellIndex * 4, static_cast<std::uint32_t>(a.data.size()));
        a.u16(static_cast<std::uint16_t>(lists[cellIndex].size()));
        for (const std::uint16_t triangle : lists[cellIndex]) {
            a.u16(triangle);
        }
        a.alignTo(4);
    }
    a.alignTo(0x800);

    Writer b;
    if (spec.owbb) {
        b.text("OWBB");
        b.u32(0x24);
        b.u32(0x87D73394);
        b.u32(16);
        b.u32(headerId);
        b.u32(cell);
        b.f32(1.0F);
        b.f32(2.0F);
        b.data.insert(b.data.end(), b.data.begin(), b.data.begin() + 16);
    }

    Writer file;
    file.text("fiz ");
    file.u32(1);
    file.u32(static_cast<std::uint32_t>(a.data.size()));
    file.u32(static_cast<std::uint32_t>(b.data.size()));
    file.u32(headerId);
    file.padTo(0x20);
    file.data.insert(file.data.end(), a.data.begin(), a.data.end());
    file.data.insert(file.data.end(), b.data.begin(), b.data.end());
    return file.data;
}

struct IndexTile {
    std::uint32_t id;
    std::uint32_t cell;
};

Bytes buildCol(std::uint32_t columns, std::uint32_t rows, glm::vec2 origin, const std::vector<IndexTile>& tiles)
{
    Writer w;
    w.u32(columns);
    w.u32(rows);
    w.f32(origin.x);
    w.f32(origin.y);
    w.f32(origin.x + 100.0F * static_cast<float>(columns));
    w.f32(origin.y + 100.0F * static_cast<float>(rows));
    w.padTo(0x20);
    w.f32(0.01F);
    w.f32(0.01F);
    w.u32(0);
    w.u32(4);
    w.u32(0xABCD1234);
    w.f32(100.0F);
    w.f32(100.0F);
    w.f32(8.0F);
    w.f32(8.0F);
    w.padTo(0x4C);
    w.u32(static_cast<std::uint32_t>(tiles.size()));
    w.u32(0x54);
    w.u32(0x13000);
    std::uint32_t offset = 0x13000;
    for (const IndexTile& tile : tiles) {
        w.u32(tile.id);
        w.u32(tile.cell);
        w.u32(offset);
        w.u32(0x800);
        w.u32(0);
        w.u32(offset + 0x800);
        offset += 0x800;
    }
    return w.data;
}

TriangleSpec ground(glm::vec3 a, glm::vec3 b, glm::vec3 c, std::uint8_t surface)
{
    TriangleSpec t;
    t.corners = {a, b, c};
    t.surface = surface;
    return t;
}

/// Ground quad over x in [x0, x1], z in [z0, z1] at height y, facing up.
void addQuad(std::vector<TriangleSpec>& out, float x0, float x1, float z0, float z1, float y, std::uint8_t surface)
{
    out.push_back(ground({x0, y, z0}, {x0, y, z1}, {x1, y, z0}, surface));
    out.push_back(ground({x1, y, z0}, {x0, y, z1}, {x1, y, z1}, surface));
}

/// A two-tile world: cells (0, 0) and (1, 0) of 100 m from the origin. The
/// ground is asphalt (0) at y = 0 for x < 104 and dirt (1) at y = 1 beyond;
/// both quads reach into both tiles, so both tiles hold them, as real tiles
/// share the triangles near their common edge. Tile 0 also has a guard rail
/// (3) at x = 50 facing -x, a camera-only roof at y = 5 over x, z in
/// [10, 20] x [60, 70], and a barrier of route 42 at z = 80 facing -z.
std::map<std::uint32_t, Bytes> twoTileWorld()
{
    std::vector<TriangleSpec> shared;
    addQuad(shared, -8.0F, 104.0F, -8.0F, 108.0F, 0.0F, 0);
    addQuad(shared, 104.0F, 208.0F, -8.0F, 108.0F, 1.0F, 1);

    TileSpec first;
    first.boundsMin = glm::vec2(-8.0F);
    first.boundsMax = glm::vec2(108.0F);
    first.routes = {7, 42};
    first.triangles = shared;
    TriangleSpec rail;
    rail.corners = {glm::vec3(50.0F, 0.0F, 0.0F), glm::vec3(50.0F, 0.0F, 40.0F), glm::vec3(50.0F, 10.0F, 0.0F)};
    rail.flags = Flags::kSolid;
    rail.surface = 3;
    first.triangles.push_back(rail);
    TriangleSpec roof = ground({10.0F, 5.0F, 60.0F}, {10.0F, 5.0F, 70.0F}, {20.0F, 5.0F, 60.0F}, 49);
    roof.flags = Flags::kCamera;
    first.triangles.push_back(roof);
    TriangleSpec barrier;
    barrier.corners = {glm::vec3(20.0F, 0.0F, 80.0F), glm::vec3(20.0F, 10.0F, 80.0F), glm::vec3(40.0F, 0.0F, 80.0F)};
    barrier.flags = Flags::kSolid;
    barrier.routeMask = 0x0002;
    first.triangles.push_back(barrier);

    TileSpec second;
    second.boundsMin = glm::vec2(92.0F, -8.0F);
    second.boundsMax = glm::vec2(208.0F, 108.0F);
    second.triangles = shared;
    second.owbb = true;

    return {{0, buildFiz(first, 0, 0)}, {1, buildFiz(second, 1, 1)}};
}

const char* kSurfaceXml = R"(<?xml version="1.0" ?>
<Root>
  <SurfaceTypesList>
    <SurfaceType value="Asphalt" />
    <SurfaceType value="Dirt" />
    <SurfaceType value="Kerb" />
    <SurfaceType value="GuardRail" />
  </SurfaceTypesList>
  <SurfaceCategories><Car /><HardWorld /><SoftWorld /><GuardRail /></SurfaceCategories>
  <CategoryToCategoryParams>
    <Node Category0="Default" Category1="Default" FrictionCoefficient="0.5" Elasticity="0.1" />
    <Node Category0="Car" Category1="HardWorld" FrictionCoefficient="0.5" LowSpeedFrictionCoefficient="0.1"
          Elasticity="-0.9" />
  </CategoryToCategoryParams>
  <SurfaceTypes>
    <Asphalt Category="HardWorld">
      <Friction><FrictionScale value="1" /><OffRoadness value="0" /></Friction>
      <Aerodynamics><LinearDrag value="0.03" /></Aerodynamics>
      <Bumpiness><NoiseType value="Simplex" /><Scale value="-0.009" /></Bumpiness>
      <IsPenaltySurface value="true" />
      <DebugColor><ColorRed value="80" /><ColorGreen value="81" /><ColorBlue value="82" /></DebugColor>
    </Asphalt>
    <Dirt Category="SoftWorld">
      <Friction>
        <FrictionScale value="0.95" /><OffRoadness value="1.0" />
        <VelDepFriction><VelPeak value="35" /><LateralCoeff value="0.35" /><LongCoeff value="0.1" /></VelDepFriction>
      </Friction>
      <Springiness><IsSpringy value="true" /><SpringK value="291.0" /></Springiness>
    </Dirt>
    <GuardRail Category="GuardRail" />
  </SurfaceTypes>
</Root>
)";

collision::CollisionIndex twoTileIndex()
{
    return collision::CollisionIndex::parse(buildCol(2, 1, glm::vec2(0.0F), {{0, 0}, {1, 1}}), "test.col");
}

std::unique_ptr<collision::TrackCollision> twoTileCollision(std::map<std::uint32_t, Bytes> tiles)
{
    auto shared = std::make_shared<std::map<std::uint32_t, Bytes>>(std::move(tiles));
    return std::make_unique<collision::TrackCollision>(twoTileIndex(), collision::SurfaceTable::parse(kSurfaceXml),
        [shared](std::uint32_t id) {
            const auto found = shared->find(id);
            if (found == shared->end()) {
                throw fh::DataError("{}.fiz: missing", id);
            }
            return found->second;
        });
}

} // namespace

TEST_CASE("unpacksVertexNormals", "[collision]")
{
    const glm::vec3 up = collision::unpackFizNormal(packNormal(glm::vec3(0.0F, 1.0F, 0.0F)));
    CHECK_THAT(up.x, WithinAbs(0.0, 1e-6));
    CHECK_THAT(up.y, WithinAbs(1.0, 1e-6));
    CHECK_THAT(up.z, WithinAbs(0.0, 1e-6));
    // Word from Colorado's tile 100: a wall vertex facing mostly +z.
    const glm::vec3 wall = collision::unpackFizNormal(0x7DBFA74C);
    CHECK_THAT(wall.x, WithinAbs(-0.18, 0.01));
    CHECK_THAT(wall.y, WithinAbs(-0.01, 0.01));
    CHECK_THAT(wall.z, WithinAbs(0.98, 0.01));
}

TEST_CASE("parsesSyntheticTile", "[collision]")
{
    const std::map<std::uint32_t, Bytes> world = twoTileWorld();
    const collision::FizTile tile = collision::parseFizTile(world.at(0), "0.fiz");
    CHECK(tile.version == 1U);
    CHECK(tile.headerId == 0U);
    CHECK(tile.triangles.size() == std::size_t{7});
    CHECK(tile.positions.size() == std::size_t{21});
    CHECK(tile.cellsX == kCells);
    CHECK_THAT(tile.cellSize.x, WithinAbs(10.0, 1e-6));
    CHECK_THAT(tile.boundsMin.x, WithinAbs(-8.0, 1e-6));
    CHECK_THAT(tile.boundsMax.y, WithinAbs(108.0, 1e-6));
    CHECK(tile.routes.size() == std::size_t{2});
    CHECK(tile.routes[1] == std::uint16_t{42});
    CHECK(!tile.owbb.has_value());

    const collision::FizTriangle& rail = tile.triangles[4];
    CHECK(rail.surface == std::uint8_t{3});
    CHECK(rail.flags == Flags::kSolid);
    CHECK_THAT(rail.normal.x, WithinAbs(-1.0, 1e-6));
    CHECK(rail.edgeCosines[0] == std::uint8_t{255});
    const glm::vec3 n = glm::normalize(glm::cross(tile.positions[rail.vertices[1]] - tile.positions[rail.vertices[0]],
        tile.positions[rail.vertices[2]] - tile.positions[rail.vertices[0]]));
    CHECK_THAT(glm::dot(n, rail.normal), WithinAbs(1.0, 1e-5));
    CHECK_THAT(tile.vertexNormals[rail.vertices[0]].x, WithinAbs(-1.0, 1e-3));

    const collision::FizTriangle& barrier = tile.triangles[6];
    CHECK(barrier.belongsToRoutes());
    CHECK(!tile.present(barrier, -1));
    CHECK(!tile.present(barrier, 7));
    CHECK(tile.present(barrier, 42));
    CHECK(tile.present(barrier, collision::kAllRoutes));
    CHECK(tile.present(rail, -1));

    // The rail at x = 50 spans z 0..40: cell column 5 (x 42..52), rows 0..4.
    bool listed = false;
    for (const std::uint16_t t : tile.cell(5, 2)) {
        listed = listed || t == 4;
    }
    CHECK(listed);
    for (const std::uint16_t t : tile.cell(0, 11)) {
        CHECK(t != 4);
    }

    const collision::FizTile second = collision::parseFizTile(world.at(1), "1.fiz");
    CHECK(second.owbb.has_value());
    CHECK(second.owbb->hash == 0x87D73394U);
    CHECK(second.owbb->id == 1U);
    CHECK(second.owbb->cell == 1U);
    CHECK(second.owbb->payload.size() == std::size_t{16});
}

TEST_CASE("rejectsDamagedTiles", "[collision]")
{
    const Bytes good = twoTileWorld().at(0);
    CHECK_THROWS_AS(collision::parseFizTile(Bytes(good.begin(), good.begin() + 100), "short.fiz"), std::exception);

    Bytes magic = good;
    magic[0] = 'x';
    CHECK_THROWS_AS(collision::parseFizTile(magic, "magic.fiz"), std::exception);

    Bytes version = good;
    version[7] = 2;
    CHECK_THROWS_AS(collision::parseFizTile(version, "version.fiz"), std::exception);

    // The first triangle's first vertex offset, moved off a vertex.
    const std::size_t triangles = 0x20 + fh::loadBe32(good.data() + 0x20 + 0x0C);
    Bytes vertex = good;
    vertex[triangles + 7] = static_cast<std::uint8_t>(vertex[triangles + 7] + 4);
    CHECK_THROWS_AS(collision::parseFizTile(vertex, "vertex.fiz"), std::exception);

    // A cell list naming a triangle past the end.
    const std::size_t grid = 0x20 + fh::loadBe32(good.data() + 0x20 + 0x38);
    const std::size_t list = 0x20 + fh::loadBe32(good.data() + grid);
    Bytes cell = good;
    cell[list + 2] = 0x7F;
    CHECK_THROWS_AS(collision::parseFizTile(cell, "cell.fiz"), std::exception);

    Bytes routes = good;
    routes[0x20 + 0x78 + 3] = 16;
    CHECK_THROWS_AS(collision::parseFizTile(routes, "routes.fiz"), std::exception);

    Bytes size = good;
    size[0x0A] = 0x7F;
    CHECK_THROWS_AS(collision::parseFizTile(size, "size.fiz"), std::exception);
}

TEST_CASE("parsesCollisionIndex", "[collision]")
{
    const Bytes data = buildCol(3, 2, glm::vec2(-150.0F, -100.0F), {{0, 0}, {1, 4}, {2, 5}});
    const collision::CollisionIndex index = collision::CollisionIndex::parse(data, "test.col");
    CHECK(index.columns() == 3U);
    CHECK(index.rows() == 2U);
    CHECK_THAT(index.margin(), WithinAbs(8.0, 1e-6));
    CHECK_THAT(index.cellSize().x, WithinAbs(100.0, 1e-6));
    CHECK(index.tiles().size() == std::size_t{3});

    CHECK(index.tileAt(-100.0F, -50.0F) == std::optional<std::size_t>(0));
    CHECK(index.tileAt(0.0F, 50.0F) == std::optional<std::size_t>(1));
    CHECK(!index.tileAt(0.0F, -50.0F).has_value());
    CHECK(!index.tileAt(1000.0F, 0.0F).has_value());
    CHECK(!index.cellAt(-151.0F, 0.0F).has_value());

    const auto [min, max] = index.tileBounds(index.tiles()[1]);
    CHECK_THAT(min.x, WithinAbs(-58.0, 1e-4));
    CHECK_THAT(max.y, WithinAbs(108.0, 1e-4));

    // Near the corner of cells 4 and 5 both tiles' margins cover the point.
    const std::vector<std::size_t> both = index.tilesOverlapping(glm::vec2(49.0F, 10.0F), glm::vec2(49.0F, 10.0F));
    CHECK(both.size() == std::size_t{2});
    const std::vector<std::size_t> one = index.tilesOverlapping(glm::vec2(20.0F, 10.0F), glm::vec2(20.0F, 10.0F));
    CHECK(one.size() == std::size_t{1});
    CHECK(index.tilesOverlapping(glm::vec2(500.0F), glm::vec2(600.0F)).empty());

    CHECK(collision::CollisionIndex::pathOf("colorado")
        == std::string("media/tracks/colorado/Ribbon_00/colorado_track_00.col"));

    CHECK_THROWS_AS(
        collision::CollisionIndex::parse(buildCol(2, 1, glm::vec2(0.0F), {{0, 0}, {1, 0}}), "dup.col"), std::exception);
    CHECK_THROWS_AS(
        collision::CollisionIndex::parse(buildCol(2, 1, glm::vec2(0.0F), {{0, 2}}), "outside.col"), std::exception);
    Bytes magic = data;
    magic[0x30] = 0;
    CHECK_THROWS_AS(collision::CollisionIndex::parse(magic, "magic.col"), std::exception);
    CHECK_THROWS_AS(
        collision::CollisionIndex::parse(Bytes(data.begin(), data.begin() + 0x60), "short.col"), std::exception);
}

TEST_CASE("parsesSurfaceTable", "[collision]")
{
    const collision::SurfaceTable table = collision::SurfaceTable::parse(kSurfaceXml);
    CHECK(table.surfaces().size() == std::size_t{4});
    CHECK(table.categories().size() == std::size_t{4});
    CHECK(table.nameOf(1) == std::string("Dirt"));
    CHECK(table.nameOf(9) == std::string("surface9"));
    CHECK(table.categoryOf(0) == "HardWorld");
    CHECK(table.categoryOf(2).empty());
    CHECK(table.idOf("guardrail") == std::optional<std::size_t>(3));

    const collision::SurfaceType& asphalt = *table.find(0);
    CHECK(asphalt.hasParameters);
    CHECK(asphalt.penalty);
    CHECK(asphalt.bumpiness.noiseType == std::string("Simplex"));
    CHECK_THAT(asphalt.bumpiness.scale, WithinAbs(-0.009, 1e-6));
    CHECK(asphalt.debugColour[1] == std::uint8_t{81});

    const collision::SurfaceType& dirt = *table.find(1);
    CHECK_THAT(dirt.frictionScale, WithinAbs(0.95, 1e-6));
    CHECK_THAT(dirt.offRoadness, WithinAbs(1.0, 1e-6));
    CHECK_THAT(dirt.velocityFriction.lateralCoeff, WithinAbs(0.35, 1e-6));
    CHECK_THAT(dirt.velocityFriction.longitudinalCoeff, WithinAbs(0.1, 1e-6));
    CHECK(dirt.springiness.springy);
    CHECK(!dirt.penalty);

    CHECK(!table.find(2)->hasParameters);
    CHECK(table.find(2)->category == collision::SurfaceTable::kNoCategory);
    CHECK(!table.find(3)->hasParameters);
    CHECK(table.find(4) == nullptr);

    const collision::CategoryPair* pair = table.pair("HardWorld", "Car");
    CHECK((pair != nullptr && pair->lowSpeedFriction.has_value()));
    CHECK((pair != nullptr && std::abs(pair->elasticity + 0.9F) < 1e-6F));
    const collision::CategoryPair* fallback = table.pair("SoftWorld", "Car");
    CHECK((fallback != nullptr && fallback->category0 == "Default"));

    CHECK_THROWS_AS(collision::SurfaceTable::parse("<Root/>"), std::exception);
    CHECK_THROWS_AS(collision::SurfaceTable::parse("not xml <"), std::exception);
}

TEST_CASE("raycastsFindGroundWallsAndSurfaces", "[collision]")
{
    const auto world = twoTileCollision(twoTileWorld());
    const glm::vec3 down(0.0F, -1.0F, 0.0F);

    const auto asphalt = world->raycast(glm::vec3(30.0F, 10.0F, 20.0F), down, 100.0F);
    CHECK(asphalt.has_value());
    if (asphalt) {
        CHECK_THAT(asphalt->distance, WithinAbs(10.0, 1e-4));
        CHECK_THAT(asphalt->point.y, WithinAbs(0.0, 1e-4));
        CHECK_THAT(asphalt->normal.y, WithinAbs(1.0, 1e-5));
        CHECK(asphalt->frontFace);
        CHECK(asphalt->surface == std::uint8_t{0});
        CHECK(asphalt->tile == std::size_t{0});
    }
    const auto dirt = world->raycast(glm::vec3(150.0F, 10.0F, 20.0F), down * 3.0F, 100.0F);
    CHECK((dirt.has_value() && std::abs(dirt->point.y - 1.0F) < 1e-4F && dirt->surface == 1));
    CHECK(!world->raycast(glm::vec3(150.0F, 10.0F, 20.0F), down, 5.0F).has_value());
    CHECK(!world->raycast(glm::vec3(30.0F, 10.0F, 20.0F), glm::vec3(0.0F), 100.0F).has_value());
    CHECK(!world->raycast(glm::vec3(30.0F, 10.0F, 20.0F), down, 0.0F).has_value());
    // Beyond the grid there is no collision.
    CHECK(!world->raycast(glm::vec3(30.0F, 10.0F, 500.0F), down, 100.0F).has_value());

    const auto rail = world->raycast(glm::vec3(0.0F, 1.0F, 10.0F), glm::vec3(1.0F, 0.0F, 0.0F), 200.0F);
    CHECK(rail.has_value());
    if (rail) {
        CHECK_THAT(rail->distance, WithinAbs(50.0, 1e-3));
        CHECK(rail->surface == std::uint8_t{3});
        CHECK(rail->frontFace);
        CHECK_THAT(rail->normal.x, WithinAbs(-1.0, 1e-5));
    }
    const auto behind = world->raycast(glm::vec3(60.0F, 1.0F, 10.0F), glm::vec3(-1.0F, 0.0F, 0.0F), 200.0F);
    CHECK((behind.has_value() && !behind->frontFace && std::abs(behind->normal.x - 1.0F) < 1e-5F));

    // A long ray across both tiles, starting in the second, finds the rail
    // in the first.
    const auto across = world->raycast(glm::vec3(180.0F, 1.5F, 10.0F), glm::vec3(-1.0F, 0.0F, 0.0F), 300.0F);
    CHECK(across.has_value());
    if (across) {
        // The dirt step at x = 104 rises to y = 1, below the ray.
        CHECK_THAT(across->distance, WithinAbs(130.0, 1e-3));
        CHECK(across->surface == std::uint8_t{3});
    }

    // Camera-only geometry is skipped unless asked for.
    const auto underRoof = world->raycast(glm::vec3(12.0F, 10.0F, 62.0F), down, 100.0F);
    CHECK((underRoof.has_value() && std::abs(underRoof->point.y) < 1e-4F));
    collision::QueryFilter everything;
    everything.requiredFlags = 0;
    const auto roof = world->raycast(glm::vec3(12.0F, 10.0F, 62.0F), down, 100.0F, everything);
    CHECK((roof.has_value() && std::abs(roof->point.y - 5.0F) < 1e-4F && roof->surface == 49));

    // Route barriers stand only while their route is raced.
    const glm::vec3 north(0.0F, 0.0F, 1.0F);
    CHECK(!world->raycast(glm::vec3(30.0F, 1.0F, 70.0F), north, 30.0F).has_value());
    collision::QueryFilter race;
    race.activeRoute = 42;
    const auto barrier = world->raycast(glm::vec3(30.0F, 1.0F, 70.0F), north, 30.0F, race);
    CHECK((barrier.has_value() && std::abs(barrier->distance - 10.0F) < 1e-3F));
    race.activeRoute = 7;
    CHECK(!world->raycast(glm::vec3(30.0F, 1.0F, 70.0F), north, 30.0F, race).has_value());

    const auto below = world->groundBelow(glm::vec3(150.0F, 3.0F, 50.0F));
    CHECK((below.has_value() && std::abs(below->point.y - 1.0F) < 1e-4F));
}

TEST_CASE("sphereContactsAreReportedOnce", "[collision]")
{
    const auto world = twoTileCollision(twoTileWorld());
    // On the asphalt just inside the second tile's cell, where both tiles
    // hold the same ground triangle.
    const std::vector<collision::SphereContact> contacts = world->sphereContacts(glm::vec3(100.0F, 0.5F, 20.0F), 1.0F);
    CHECK(contacts.size() == std::size_t{1});
    if (!contacts.empty()) {
        CHECK_THAT(contacts[0].depth, WithinAbs(0.5, 1e-4));
        CHECK_THAT(contacts[0].normal.y, WithinAbs(1.0, 1e-5));
        CHECK_THAT(contacts[0].point.y, WithinAbs(0.0, 1e-5));
        CHECK(contacts[0].surface == std::uint8_t{0});
    }
    CHECK(world->sphereContacts(glm::vec3(100.0F, 2.0F, 20.0F), 1.0F).empty());

    // Against the rail and the ground at once, deepest first.
    const auto corner = world->sphereContacts(glm::vec3(49.5F, 0.8F, 20.0F), 1.0F);
    CHECK(corner.size() == std::size_t{2});
    if (corner.size() == 2) {
        CHECK_THAT(corner[0].depth, WithinAbs(0.5, 1e-4));
        CHECK_THAT(corner[0].normal.x, WithinAbs(-1.0, 1e-5));
        CHECK(corner[0].surface == std::uint8_t{3});
        CHECK_THAT(corner[1].depth, WithinAbs(0.2, 1e-4));
    }

    // Sunk below the ground over its face: pushed back up.
    const auto sunk = world->sphereContacts(glm::vec3(30.0F, -0.5F, 20.0F), 1.0F);
    CHECK((!sunk.empty() && std::abs(sunk[0].depth - 1.5F) < 1e-4F && sunk[0].normal.y > 0.99F));
    CHECK(world->sphereContacts(glm::vec3(30.0F, -2.0F, 20.0F), 1.0F).empty());

    // At the dirt step's edge: the step's top edge from beside it.
    const auto edge = world->sphereContacts(glm::vec3(103.5F, 1.5F, 20.0F), 1.0F);
    bool touchesEdge = false;
    for (const collision::SphereContact& c : edge) {
        touchesEdge = touchesEdge || (c.surface == 1 && std::abs(c.point.x - 104.0F) < 1e-4F);
    }
    CHECK(touchesEdge);
}

TEST_CASE("verticalHitsAndTriangleWalksSkipDuplicates", "[collision]")
{
    const auto world = twoTileCollision(twoTileWorld());
    collision::QueryFilter everything;
    everything.requiredFlags = 0;
    const auto hits = world->verticalHits(12.0F, 62.0F, everything);
    CHECK(hits.size() == std::size_t{2});
    if (hits.size() == 2) {
        CHECK_THAT(hits[0].point.y, WithinAbs(5.0, 1e-4));
        CHECK_THAT(hits[1].point.y, WithinAbs(0.0, 1e-4));
    }
    CHECK(world->verticalHits(12.0F, 62.0F).size() == std::size_t{1});

    everything.activeRoute = collision::kAllRoutes;
    std::size_t count = 0;
    world->forEachTriangle(glm::vec2(-50.0F), glm::vec2(300.0F), everything,
        [&](std::size_t, const collision::FizTile&, std::uint16_t) { ++count; });
    // Four shared ground triangles, the rail, the roof and the barrier.
    CHECK(count == std::size_t{7});
}

TEST_CASE("cachesTilesAndSurvivesBadOnes", "[collision]")
{
    std::map<std::uint32_t, Bytes> tiles = twoTileWorld();
    tiles[1] = Bytes{'j', 'u', 'n', 'k'};
    const auto world = twoTileCollision(std::move(tiles));
    CHECK(world->tryTile(0) != nullptr);
    CHECK(world->tryTile(1) == nullptr);
    CHECK(world->tileError(1).has_value());
    CHECK(!world->tileError(0).has_value());
    CHECK_THROWS_AS(world->tile(1), std::exception);
    CHECK_THROWS_AS(world->tile(5), std::exception);
    CHECK(!world->raycast(glm::vec3(150.0F, 10.0F, 20.0F), glm::vec3(0.0F, -1.0F, 0.0F), 100.0F).has_value());
    CHECK(world->residentCount() == std::size_t{1});
    world->evictOutside(glm::vec2(500.0F), 10.0F);
    CHECK(world->residentCount() == std::size_t{0});

    const auto good = twoTileCollision(twoTileWorld());
    CHECK(good->loadAround(glm::vec2(100.0F, 50.0F), 20.0F) == std::size_t{2});
    CHECK(good->residentCount() == std::size_t{2});
    good->evictOutside(glm::vec2(20.0F, 50.0F), 5.0F);
    CHECK(good->residentCount() == std::size_t{1});
    good->evictAll();
    CHECK(good->loadAroundAsync(glm::vec2(20.0F, 50.0F), 5.0F).get() == std::size_t{1});

    // Many threads asking for the same tile decode it once and agree.
    const auto shared = twoTileCollision(twoTileWorld());
    std::atomic<int> same{0};
    const std::shared_ptr<const collision::FizTile> first = shared->tile(0);
    fh::world::parallelFor(64, [&](std::size_t) {
        if (shared->tile(0) == first) {
            same.fetch_add(1);
        }
    });
    CHECK(same.load() == 64);
}

TEST_CASE("coloradoCollision", "[collision]")
{
    const char* gameDir = fh::test::gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(gameDir);
    const auto world = collision::TrackCollision::open(vfs, "colorado");
    const collision::CollisionIndex& index = world->index();
    CHECK(index.columns() == 133U);
    CHECK(index.rows() == 100U);
    CHECK(index.tiles().size() == std::size_t{3201});

    const collision::SurfaceTable& surfaces = world->surfaces();
    CHECK(surfaces.surfaces().size() == std::size_t{68});
    CHECK(surfaces.categories().size() == std::size_t{13});
    CHECK(surfaces.nameOf(9) == std::string("Invisible"));
    CHECK(surfaces.categoryOf(9) == "HardWorld");
    CHECK(surfaces.categoryOf(1) == "SoftWorld");
    CHECK_THAT(surfaces.find(2)->frictionScale, WithinAbs(0.85, 1e-6));
    CHECK_THAT(surfaces.find(0)->chaseCamSpringMultiplier, WithinAbs(1.0, 1e-6));
    CHECK_THAT(surfaces.find(1)->chaseCamSpringMultiplier, WithinAbs(0.8, 1e-6));

    std::atomic<std::size_t> failures{0};
    std::atomic<std::size_t> triangles{0};
    std::atomic<std::size_t> outOfTable{0};
    fh::world::parallelFor(index.tiles().size(), [&](std::size_t i) {
        const auto tile = world->tryTile(i);
        if (!tile) {
            failures.fetch_add(1);
            return;
        }
        triangles.fetch_add(tile->triangles.size());
        for (const collision::FizTriangle& t : tile->triangles) {
            if (t.surface >= surfaces.surfaces().size()) {
                outOfTable.fetch_add(1);
            }
        }
    });
    CHECK(failures.load() == std::size_t{0});
    CHECK(triangles.load() == std::size_t{2007595});
    CHECK(outOfTable.load() == std::size_t{0});
    world->evictAll();

    // A road node of colorado.nav in the festival's town: asphalt at the
    // node's height.
    const auto road = world->groundBelow(glm::vec3(-2063.6F, 100.0F, -45.0F), 200.0F);
    CHECK(road.has_value());
    if (road) {
        CHECK_THAT(road->point.y, WithinAbs(-10.58, 0.01));
        CHECK(surfaces.nameOf(road->surface) == std::string("Asphalt"));
        CHECK(road->normal.y > 0.99F);
    }
    const auto resting = world->sphereContacts(glm::vec3(-2063.6F, -10.58F + 0.3F, -45.0F), 0.5F);
    CHECK(!resting.empty());
    if (!resting.empty()) {
        CHECK_THAT(resting[0].depth, WithinAbs(0.2, 0.02));
        CHECK(resting[0].normal.y > 0.99F);
    }
    // A dirt road node north-east of the town.
    const auto dirt = world->groundBelow(glm::vec3(825.0F, 200.0F, -2752.5F), 100.0F);
    CHECK((dirt.has_value() && std::abs(dirt->point.y - 169.6F) < 0.01F && surfaces.nameOf(dirt->surface) == "Dirt"));
}
