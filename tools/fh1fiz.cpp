// Reads a track's collision tiles (.fiz, indexed by the ribbon's .col file):
// statistics over every tile, the surfaces at one point, an OBJ export of the
// triangles around a point, and a comparison of collision heights with the
// render geometry at road nodes, gameplay objects and off-road points.

#include "base/Log.h"
#include "collision/TrackCollision.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <lzx/zip.h>
#include <pugixml.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

using fh::collision::FizTile;
using fh::collision::FizTriangle;
using fh::collision::QueryFilter;
using fh::collision::RayHit;
using fh::collision::TrackCollision;

/// Metres within which collision and render heights count as matching.
constexpr float kHeightTolerance = 0.2F;
constexpr float kRenderTileSize = 500.0F;
/// Off-road samples lie at least this far from every road node.
constexpr float kOffRoadClearance = 30.0F;
constexpr std::size_t kRoadSamples = 30;
constexpr std::size_t kObjectSamples = 15;
constexpr std::size_t kOffRoadSamples = 20;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1fiz DISC TRACK stats\n"
        "       fh1fiz DISC TRACK at X,Z\n"
        "       fh1fiz DISC TRACK --obj OUT.obj X,Z RADIUS\n"
        "       fh1fiz DISC TRACK validate\n"
        "  DISC      the extracted disc (its root or its media folder)\n"
        "  TRACK     a folder of media/tracks, such as colorado\n"
        "  stats     decode every tile; counts, surfaces and failures\n"
        "  at        the tiles covering world point X,Z and the surfaces there\n"
        "  --obj     write the triangles within RADIUS metres of X,Z (every\n"
        "            route's barriers and camera-only geometry included), one\n"
        "            material per surface category, with OUT.mtl beside it\n"
        "  validate  compare collision heights and surfaces with the render\n"
        "            geometry at road nodes, gameplay objects and off-road points\n");
    return 2;
}

std::optional<float> parseFloat(std::string_view text)
{
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (copy.empty() || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<glm::vec2> parsePoint(std::string_view text)
{
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<float> x = parseFloat(text.substr(0, comma));
    const std::optional<float> z = parseFloat(text.substr(comma + 1));
    if (!x || !z) {
        return std::nullopt;
    }
    return glm::vec2(*x, *z);
}

double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::string surfaceLabel(const TrackCollision& collision, std::uint8_t surface)
{
    const std::string_view category = collision.surfaces().categoryOf(surface);
    return std::format("{} ({}, {})", collision.surfaces().nameOf(surface), surface,
        category.empty() ? "no category" : category);
}

std::string flagText(std::uint8_t flags)
{
    namespace F = fh::collision::TriangleFlags;
    std::string text = std::format("0x{:02x}", flags);
    if ((flags & F::kGround) != 0) {
        text += " ground";
    }
    if ((flags & F::kCamera) != 0) {
        text += " camera";
    }
    if ((flags & F::kSolid) != 0) {
        text += " solid";
    }
    return text;
}

int stats(const fh::Vfs& vfs, std::string_view track, const TrackCollision& collision)
{
    const auto start = std::chrono::steady_clock::now();
    const auto& tiles = collision.index().tiles();

    std::size_t archiveEntries = 0;
    std::unordered_set<std::string> distinctNames;
    if (const auto archive = vfs.archiveAt(std::format("media/tracks/{}/bin", track))) {
        for (const lzx::ZipEntry& entry : archive->entries()) {
            if (fh::endsWithNoCase(entry.name, ".fiz")) {
                ++archiveEntries;
                distinctNames.insert(lzx::ZipArchive::normalizeName(entry.name));
            }
        }
    }

    struct Totals {
        std::size_t vertices = 0;
        std::size_t triangles = 0;
        std::size_t routeTriangles = 0;
        std::size_t owbbBlocks = 0;
        std::size_t maxTriangles = 0;
        std::map<std::uint8_t, std::size_t> surfaces;
        std::map<std::uint8_t, std::size_t> flags;
        std::set<std::uint16_t> routes;
        std::vector<std::string> failures;
    };
    Totals totals;
    std::mutex mutex;
    fh::world::parallelFor(tiles.size(), [&](std::size_t i) {
        std::shared_ptr<const FizTile> tile;
        try {
            tile = collision.tile(i);
        } catch (const std::exception& e) {
            const std::lock_guard lock(mutex);
            totals.failures.push_back(std::format("tile {}: {}", tiles[i].id, e.what()));
            return;
        }
        std::map<std::uint8_t, std::size_t> surfaces;
        std::map<std::uint8_t, std::size_t> flags;
        std::size_t routeTriangles = 0;
        for (const FizTriangle& triangle : tile->triangles) {
            ++surfaces[triangle.surface];
            ++flags[triangle.flags];
            routeTriangles += triangle.belongsToRoutes() ? 1 : 0;
        }
        const std::lock_guard lock(mutex);
        totals.vertices += tile->positions.size();
        totals.triangles += tile->triangles.size();
        totals.maxTriangles = std::max(totals.maxTriangles, tile->triangles.size());
        totals.routeTriangles += routeTriangles;
        totals.owbbBlocks += tile->owbb ? 1 : 0;
        totals.routes.insert(tile->routes.begin(), tile->routes.end());
        for (const auto& [surface, count] : surfaces) {
            totals.surfaces[surface] += count;
        }
        for (const auto& [flag, count] : flags) {
            totals.flags[flag] += count;
        }
    });

    std::printf("index: %u x %u cells of %.0f m from (%.2f, %.2f), margin %.0f m\n", collision.index().columns(),
        collision.index().rows(), collision.index().cellSize().x, collision.index().origin().x,
        collision.index().origin().y, collision.index().margin());
    std::printf("archive: %zu .fiz entries, %zu distinct names\n", archiveEntries, distinctNames.size());
    std::printf("tiles: %zu, parsed %zu, failed %zu (%.2f s)\n", tiles.size(), tiles.size() - totals.failures.size(),
        totals.failures.size(), secondsSince(start));
    std::printf("vertices: %zu, triangles: %zu (at most %zu in one tile), %zu in route barriers of %zu routes\n",
        totals.vertices, totals.triangles, totals.maxTriangles, totals.routeTriangles, totals.routes.size());
    std::printf("OWBB blocks: %zu\n", totals.owbbBlocks);
    std::printf("surfaces:\n");
    for (const auto& [surface, count] : totals.surfaces) {
        std::printf("  %8zu  %5.2f%%  %s\n", count, 100.0 * static_cast<double>(count) / totals.triangles,
            surfaceLabel(collision, surface).c_str());
    }
    std::printf("flags:\n");
    for (const auto& [flag, count] : totals.flags) {
        std::printf("  %8zu  %s\n", count, flagText(flag).c_str());
    }
    for (const std::string& failure : totals.failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    return totals.failures.empty() ? 0 : 1;
}

int at(const TrackCollision& collision, glm::vec2 point)
{
    const auto& index = collision.index();
    const auto cell = index.cellAt(point.x, point.y);
    if (cell) {
        std::printf("cell: column %u, row %u\n", cell->first, cell->second);
    } else {
        std::printf("cell: outside the grid\n");
    }
    const std::vector<std::size_t> covering = index.tilesOverlapping(point, point);
    if (covering.empty()) {
        std::printf("no tile covers %.2f, %.2f: no collision there\n", point.x, point.y);
        return 1;
    }
    const std::optional<std::size_t> owner = index.tileAt(point.x, point.y);
    for (const std::size_t i : covering) {
        const auto& entry = index.tiles()[i];
        const auto [min, max] = index.tileBounds(entry);
        std::printf("tile %u.fiz: X %.2f..%.2f, Z %.2f..%.2f%s", entry.id, min.x, max.x, min.y, max.y,
            owner == i ? " (its cell)" : "");
        if (const auto tile = collision.tryTile(i)) {
            std::printf(", %zu triangles", tile->triangles.size());
            if (!tile->routes.empty()) {
                std::printf(", barriers of routes");
                for (const std::uint16_t route : tile->routes) {
                    std::printf(" %u", route);
                }
            }
            std::printf("\n");
        } else {
            std::printf(", unreadable: %s\n", collision.tileError(i).value_or("?").c_str());
        }
    }

    QueryFilter everything;
    everything.requiredFlags = 0;
    everything.activeRoute = fh::collision::kAllRoutes;
    const std::vector<RayHit> hits = collision.verticalHits(point.x, point.y, everything);
    const QueryFilter solid;
    std::printf("surfaces along the vertical line, highest first:\n");
    for (const RayHit& hit : hits) {
        const auto tile = collision.tile(hit.tile);
        const FizTriangle& triangle = tile->triangles[hit.triangle];
        std::string routes;
        for (std::size_t k = 0; k < tile->routes.size(); ++k) {
            if ((triangle.routeMask >> k & 1U) != 0 && triangle.belongsToRoutes()) {
                routes += std::format(" {}", tile->routes[k]);
            }
        }
        std::printf("  y %9.3f  %s  %s  normal (%.3f, %.3f, %.3f)%s%s%s\n", hit.point.y,
            surfaceLabel(collision, hit.surface).c_str(), flagText(hit.flags).c_str(), triangle.normal.x,
            triangle.normal.y, triangle.normal.z, hit.frontFace ? "" : " from below",
            routes.empty() ? "" : "  route barrier:", routes.c_str());
    }
    const std::optional<RayHit> ground = collision.groundBelow(glm::vec3(point.x, 20000.0F, point.y), 40000.0F, solid);
    if (ground) {
        std::printf("ground: %.3f, %s\n", ground->point.y, surfaceLabel(collision, ground->surface).c_str());
    } else {
        std::printf("ground: none\n");
    }
    return 0;
}

/// RGB of each surface category in the OBJ export.
std::array<float, 3> categoryColour(std::string_view category)
{
    static const std::map<std::string_view, std::array<float, 3>> colours{
        {"Car", {0.9F, 0.2F, 0.6F}},
        {"Cone", {1.0F, 0.5F, 0.0F}},
        {"Tire", {0.1F, 0.1F, 0.1F}},
        {"HardWorld", {0.5F, 0.5F, 0.55F}},
        {"SoftWorld", {0.45F, 0.6F, 0.25F}},
        {"RumbleStrip", {0.9F, 0.1F, 0.1F}},
        {"TireWall", {0.2F, 0.2F, 0.2F}},
        {"GuardRail", {0.95F, 0.85F, 0.2F}},
        {"GenCollObj", {0.3F, 0.5F, 0.9F}},
        {"Pylon", {0.9F, 0.9F, 0.9F}},
        {"SoccerBall", {1.0F, 1.0F, 1.0F}},
        {"NoCollision", {0.2F, 0.9F, 0.9F}},
        {"Smashable", {0.6F, 0.3F, 0.9F}},
    };
    const auto found = colours.find(category);
    return found != colours.end() ? found->second : std::array<float, 3>{1.0F, 0.0F, 1.0F};
}

int exportObj(const TrackCollision& collision, const std::filesystem::path& out, glm::vec2 centre, float radius)
{
    struct Face {
        std::array<glm::vec3, 3> corners;
        std::string material;
        std::string group;
    };
    std::vector<Face> faces;
    QueryFilter everything;
    everything.requiredFlags = 0;
    everything.activeRoute = fh::collision::kAllRoutes;
    collision.forEachTriangle(centre - glm::vec2(radius), centre + glm::vec2(radius), everything,
        [&](std::size_t, const FizTile& tile, std::uint16_t index) {
            const FizTriangle& triangle = tile.triangles[index];
            Face face;
            for (int k = 0; k < 3; ++k) {
                face.corners[k] = tile.positions[triangle.vertices[k]];
            }
            const std::string_view category = collision.surfaces().categoryOf(triangle.surface);
            face.material = category.empty() ? "Unknown" : std::string(category);
            if (triangle.belongsToRoutes()) {
                face.group = "route_barriers";
            } else if ((triangle.flags & fh::collision::TriangleFlags::kSolid) == 0) {
                face.group = "camera_only";
            } else {
                face.group = "world";
            }
            faces.push_back(std::move(face));
        });
    std::sort(faces.begin(), faces.end(),
        [](const Face& l, const Face& r) { return std::tie(l.group, l.material) < std::tie(r.group, r.material); });

    std::filesystem::path mtlPath = out;
    mtlPath.replace_extension(".mtl");
    std::ofstream mtl(mtlPath);
    std::set<std::string> materials;
    for (const Face& face : faces) {
        materials.insert(face.material);
    }
    for (const std::string& material : materials) {
        const std::array<float, 3> rgb = categoryColour(material);
        mtl << std::format("newmtl {}\nKd {} {} {}\n\n", material, rgb[0], rgb[1], rgb[2]);
    }

    std::ofstream obj(out);
    obj << std::format("# collision triangles within {} m of {}, {}\nmtllib {}\n", radius, centre.x, centre.y,
        mtlPath.filename().string());
    std::string group;
    std::string material;
    std::size_t vertex = 1;
    for (const Face& face : faces) {
        if (face.group != group) {
            group = face.group;
            obj << "g " << group << '\n';
            material.clear();
        }
        if (face.material != material) {
            material = face.material;
            obj << "usemtl " << material << '\n';
        }
        for (const glm::vec3& p : face.corners) {
            obj << std::format("v {} {} {}\n", p.x, p.y, p.z);
        }
        obj << std::format("f {} {} {}\n", vertex, vertex + 1, vertex + 2);
        vertex += 3;
    }
    if (!obj || !mtl) {
        std::fprintf(stderr, "fh1fiz: could not write %s\n", out.string().c_str());
        return 1;
    }
    std::printf("wrote %zu triangles to %s and %s\n", faces.size(), out.string().c_str(),
        mtlPath.string().c_str());
    return 0;
}

struct Sample {
    std::string kind;
    std::string label;
    glm::vec3 position{0.0F};
    /// Whether position.y is a known height (road node, object) to pick the
    /// surface nearest to; otherwise the highest collision surface is used.
    bool hasHeight = true;
    std::optional<float> collisionY;
    std::uint8_t surface = 0;
    std::vector<float> renderHeights;
};

Sample makeSample(std::string kind, std::string label, glm::vec3 position, bool hasHeight)
{
    Sample sample;
    sample.kind = std::move(kind);
    sample.label = std::move(label);
    sample.position = position;
    sample.hasHeight = hasHeight;
    return sample;
}

std::vector<glm::vec3> readNavNodes(const fh::Vfs& vfs, std::string_view track)
{
    const std::string path = std::format("media/tracks/{0}/{0}.nav", track);
    const std::optional<fh::Bytes> data = vfs.read(path);
    std::vector<glm::vec3> nodes;
    if (!data) {
        return nodes;
    }
    // Header of ten words, the second the node count; nodes of 32 bytes:
    // u32 id, f32 x, y, z, then four words of links.
    fh::BeReader r(*data, path);
    r.skip(4);
    const std::uint32_t count = r.u32();
    r.seek(40);
    for (std::uint32_t i = 0; i < count && r.remaining() >= 32; ++i) {
        r.skip(4);
        glm::vec3 p;
        p.x = r.f32();
        p.y = r.f32();
        p.z = r.f32();
        r.skip(16);
        nodes.push_back(p);
    }
    return nodes;
}

std::vector<std::pair<std::string, glm::vec3>> readGameObjects(const fh::Vfs& vfs, std::string_view track)
{
    std::vector<std::pair<std::string, glm::vec3>> objects;
    const std::optional<std::string> text = vfs.readText(std::format("media/tracks/{}/Ribbon_00/GameObjs.xml", track));
    if (!text) {
        return objects;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return objects;
    }
    for (const pugi::xml_node object : doc.child("GameObjs").children()) {
        const pugi::xml_node pos = object.child("Pos");
        if (!pos) {
            continue;
        }
        objects.emplace_back(object.attribute("GameplayID").value(),
            glm::vec3(pos.attribute("x").as_float(), pos.attribute("y").as_float(), pos.attribute("z").as_float()));
    }
    return objects;
}

/// Heights at which the vertical line through (x, z) crosses the mesh.
void renderHeightsAt(const fh::world::TileMesh& mesh, Sample& sample)
{
    const float x = sample.position.x;
    const float z = sample.position.z;
    constexpr int stride = fh::world::TileMesh::kFloatsPerVertex;
    for (const fh::world::TileMesh::Batch& batch : mesh.batches) {
        if (batch.backdrop) {
            continue;
        }
        for (std::uint32_t i = batch.firstIndex; i + 2 < batch.firstIndex + batch.indexCount; i += 3) {
            std::array<glm::vec3, 3> p;
            for (int k = 0; k < 3; ++k) {
                const float* v = &mesh.vertices[static_cast<std::size_t>(mesh.indices[i + k]) * stride];
                p[k] = glm::vec3(v[0], v[1], v[2]);
            }
            const float d = (p[1].z - p[2].z) * (p[0].x - p[2].x) + (p[2].x - p[1].x) * (p[0].z - p[2].z);
            if (std::abs(d) < 1e-9F) {
                continue;
            }
            const float l0 = ((p[1].z - p[2].z) * (x - p[2].x) + (p[2].x - p[1].x) * (z - p[2].z)) / d;
            const float l1 = ((p[2].z - p[0].z) * (x - p[2].x) + (p[0].x - p[2].x) * (z - p[2].z)) / d;
            const float l2 = 1.0F - l0 - l1;
            if (l0 < -1e-5F || l1 < -1e-5F || l2 < -1e-5F) {
                continue;
            }
            sample.renderHeights.push_back(l0 * p[0].y + l1 * p[1].y + l2 * p[2].y);
        }
    }
}

std::optional<float> nearest(const std::vector<float>& heights, float to)
{
    std::optional<float> best;
    for (const float h : heights) {
        if (!best || std::abs(h - to) < std::abs(*best - to)) {
            best = h;
        }
    }
    return best;
}

int validate(const fh::Vfs& vfs, std::string_view track, const TrackCollision& collision)
{
    const auto start = std::chrono::steady_clock::now();
    const std::vector<glm::vec3> nav = readNavNodes(vfs, track);
    const auto objects = readGameObjects(vfs, track);
    std::vector<Sample> samples;

    const auto covered = [&](float x, float z) { return collision.index().tileAt(x, z).has_value(); };
    std::vector<glm::vec3> coveredNav;
    for (const glm::vec3& node : nav) {
        if (covered(node.x, node.z)) {
            coveredNav.push_back(node);
        }
    }
    for (std::size_t k = 0; k < kRoadSamples && !coveredNav.empty(); ++k) {
        const glm::vec3& node = coveredNav[k * coveredNav.size() / kRoadSamples];
        samples.push_back(
            makeSample("road", std::format("nav node {}", k * coveredNav.size() / kRoadSamples), node, true));
    }
    std::vector<std::pair<std::string, glm::vec3>> coveredObjects;
    for (const auto& object : objects) {
        // Objects left at the origin are placeholders, not placed on the ground.
        if (covered(object.second.x, object.second.z) && object.second != glm::vec3(0.0F)) {
            coveredObjects.push_back(object);
        }
    }
    for (std::size_t k = 0; k < kObjectSamples && !coveredObjects.empty(); ++k) {
        const auto& object = coveredObjects[k * coveredObjects.size() / kObjectSamples];
        samples.push_back(makeSample("object", object.first, object.second, true));
    }
    std::mt19937 random(1234);
    const auto& tiles = collision.index().tiles();
    std::uniform_int_distribution<std::size_t> pickTile(0, tiles.empty() ? 0 : tiles.size() - 1);
    std::uniform_real_distribution<float> within(0.1F, 0.9F);
    // The tiles only hold the drivable area, so most points of a cell have
    // no collision; off-road samples are drawn from the ones that do.
    std::size_t randomPoints = 0;
    std::size_t randomPointsWithGround = 0;
    const std::size_t wanted = samples.size() + kOffRoadSamples;
    for (int attempt = 0; attempt < 20000 && !tiles.empty() && samples.size() < wanted; ++attempt) {
        const auto& entry = tiles[pickTile(random)];
        const auto [min, max] = collision.index().cellBounds(
            entry.cell % collision.index().columns(), entry.cell / collision.index().columns());
        const glm::vec2 p = min + (max - min) * glm::vec2(within(random), within(random));
        ++randomPoints;
        const std::optional<RayHit> ground = collision.groundBelow(glm::vec3(p.x, 20000.0F, p.y), 40000.0F);
        if (!ground || !ground->frontFace) {
            continue;
        }
        ++randomPointsWithGround;
        const bool nearRoad = std::any_of(nav.begin(), nav.end(),
            [&](const glm::vec3& n) { return glm::length(glm::vec2(n.x, n.z) - p) < kOffRoadClearance; });
        if (!nearRoad) {
            samples.push_back(
                makeSample("offroad", std::format("tile {}", entry.id), glm::vec3(p.x, 0.0F, p.y), false));
        }
    }
    std::printf("random points in tile cells: %zu, with collision below: %zu\n", randomPoints,
        randomPointsWithGround);

    const QueryFilter solid;
    for (Sample& sample : samples) {
        const std::vector<RayHit> hits = collision.verticalHits(sample.position.x, sample.position.z, solid);
        std::optional<RayHit> chosen;
        for (const RayHit& hit : hits) {
            if (!hit.frontFace) {
                continue;
            }
            if (!chosen
                || (sample.hasHeight
                    && std::abs(hit.point.y - sample.position.y) < std::abs(chosen->point.y - sample.position.y))) {
                chosen = hit;
            }
        }
        if (chosen) {
            sample.collisionY = chosen->point.y;
            sample.surface = chosen->surface;
        }
    }

    std::fprintf(stderr, "loading the render world...\n");
    const std::optional<fh::world::TrackWorld> world = fh::world::TrackWorld::load(vfs, track);
    if (!world) {
        std::fprintf(stderr, "fh1fiz: the render world could not be loaded\n");
        return 1;
    }
    const fh::world::WorldTileGrid grid(world->index, kRenderTileSize);
    for (std::size_t t = 0; t < grid.tiles().size(); ++t) {
        const auto& tile = grid.tiles()[t];
        std::vector<Sample*> inside;
        for (Sample& sample : samples) {
            if (sample.position.x >= tile.boundsMin.x && sample.position.x <= tile.boundsMax.x
                && sample.position.z >= tile.boundsMin.z && sample.position.z <= tile.boundsMax.z) {
                inside.push_back(&sample);
            }
        }
        if (inside.empty()) {
            continue;
        }
        // The finest level of the ground, roads and placed props; procedural
        // trees and bushes have no collision of their own in the tiles.
        std::vector<std::uint32_t> chunks;
        for (const std::uint32_t chunk : grid.chunksAt(tile, 0.0F)) {
            const fh::world::WorldChunk& c = world->index.chunks()[chunk];
            if (!c.backdrop && !c.scattered) {
                chunks.push_back(chunk);
            }
        }
        const fh::world::TileMesh mesh = fh::world::buildTileMesh(*world->archive, world->index, chunks);
        for (Sample* sample : inside) {
            renderHeightsAt(mesh, *sample);
        }
    }

    struct Summary {
        std::size_t count = 0;
        std::size_t compared = 0;
        std::size_t within = 0;
        double sumAbs = 0.0;
        float maxAbs = 0.0F;
        std::map<std::string, std::size_t> categories;
        std::map<std::string, std::size_t> names;
    };
    std::map<std::string, Summary> summaries;
    std::printf("%-8s %-34s %10s %10s %9s %9s %9s %8s  %s\n", "kind", "label", "x", "z", "ref y", "coll y",
        "render y", "diff", "surface");
    for (const Sample& sample : samples) {
        Summary& summary = summaries[sample.kind];
        ++summary.count;
        std::optional<float> render;
        if (sample.collisionY) {
            render = nearest(sample.renderHeights, *sample.collisionY);
            const std::string_view category = collision.surfaces().categoryOf(sample.surface);
            ++summary.categories[category.empty() ? "none" : std::string(category)];
            ++summary.names[collision.surfaces().nameOf(sample.surface)];
        }
        const std::optional<float> diff
            = sample.collisionY && render ? std::optional<float>(*sample.collisionY - *render) : std::nullopt;
        if (diff) {
            ++summary.compared;
            summary.within += std::abs(*diff) <= kHeightTolerance ? 1 : 0;
            summary.sumAbs += std::abs(*diff);
            summary.maxAbs = std::max(summary.maxAbs, std::abs(*diff));
        }
        const auto show = [](std::optional<float> v) { return v ? std::format("{:.3f}", *v) : std::string("-"); };
        std::printf("%-8s %-34.34s %10.2f %10.2f %9s %9s %9s %8s  %s\n", sample.kind.c_str(), sample.label.c_str(),
            sample.position.x, sample.position.z, sample.hasHeight ? show(sample.position.y).c_str() : "-",
            show(sample.collisionY).c_str(), show(render).c_str(), show(diff).c_str(),
            sample.collisionY ? surfaceLabel(collision, sample.surface).c_str() : "no collision");
    }
    std::printf("\n");
    for (const auto& [kind, summary] : summaries) {
        std::printf("%s: %zu points, %zu compared, %zu within %.1f m, mean |diff| %.3f m, max %.3f m\n", kind.c_str(),
            summary.count, summary.compared, summary.within, kHeightTolerance,
            summary.compared ? summary.sumAbs / static_cast<double>(summary.compared) : 0.0, summary.maxAbs);
        std::printf("  categories:");
        for (const auto& [name, count] : summary.categories) {
            std::printf(" %s %zu", name.c_str(), count);
        }
        std::printf("\n  surfaces:");
        for (const auto& [name, count] : summary.names) {
            std::printf(" %s %zu", name.c_str(), count);
        }
        std::printf("\n");
    }
    std::printf("(%.1f s)\n", secondsSince(start));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        return usage();
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string track = argv[2];
        const std::string command = argv[3];
        const auto start = std::chrono::steady_clock::now();
        const std::unique_ptr<TrackCollision> collision = TrackCollision::open(vfs, track);
        std::fprintf(stderr, "opened %zu collision tiles in %.2f s\n", collision->index().tiles().size(),
            secondsSince(start));
        if (command == "stats" && argc == 4) {
            return stats(vfs, track, *collision);
        }
        if (command == "at" && argc == 5) {
            const std::optional<glm::vec2> point = parsePoint(argv[4]);
            return point ? at(*collision, *point) : usage();
        }
        if (command == "--obj" && argc == 7) {
            const std::optional<glm::vec2> point = parsePoint(argv[5]);
            const std::optional<float> radius = parseFloat(argv[6]);
            if (!point || !radius || !(*radius > 0.0F)) {
                return usage();
            }
            return exportObj(*collision, argv[4], *point, *radius);
        }
        if (command == "validate" && argc == 4) {
            return validate(vfs, track, *collision);
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fh1fiz: %s\n", e.what());
        return 1;
    }
}
