// Loads a track's world as the streamer does and reports counts, tiles and
// textures near a point. --sizes measures the whole map, one tile at a time.

#include "base/Log.h"
#include "vfs/Vfs.h"
#include "world/ShadedTiles.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <lzx/zip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr float kTileSize = 500.0F;
constexpr std::size_t kSampleTextures = 32;

int usage()
{
    std::fprintf(stderr,
        "usage: openhorizon-world DISC TRACK [--at X,Z | --sizes [THREADS]]\n"
        "  DISC     the extracted disc (its root or its media folder)\n"
        "  TRACK    a folder of media/tracks, such as colorado\n"
        "  --at     world X and Z to build the 3x3 tiles around; the map centre\n"
        "           by default\n"
        "  --sizes  builds every tile at every level of detail and decodes every\n"
        "           texture the tiles use, on THREADS threads (4 by default), and\n"
        "           reports their sizes and the time taken\n");
    return 2;
}

std::optional<std::pair<float, float>> parsePoint(std::string_view text)
{
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string x(text.substr(0, comma));
    const std::string z(text.substr(comma + 1));
    char* end = nullptr;
    const float px = std::strtof(x.c_str(), &end);
    if (x.empty() || *end != '\0') {
        return std::nullopt;
    }
    const float pz = std::strtof(z.c_str(), &end);
    if (z.empty() || *end != '\0') {
        return std::nullopt;
    }
    return std::pair{px, pz};
}

double secondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char* formatName(openhorizon::world::TextureSurface::Format format)
{
    switch (format) {
    case openhorizon::world::TextureSurface::Format::Dxt1:
        return "DXT1";
    case openhorizon::world::TextureSurface::Format::Dxt3:
        return "DXT3";
    case openhorizon::world::TextureSurface::Format::Dxt5:
        return "DXT5";
    case openhorizon::world::TextureSurface::Format::Bc5:
        return "BC5";
    case openhorizon::world::TextureSurface::Format::Rgba8:
        return "RGBA8";
    }
    return "?";
}

const char* shadingName(openhorizon::world::TileMesh::Shading shading)
{
    switch (shading) {
    case openhorizon::world::TileMesh::Shading::Plain:
        return "plain";
    case openhorizon::world::TileMesh::Shading::Splat:
        return "splat";
    case openhorizon::world::TileMesh::Shading::VertexBlend:
        return "vertex-blend";
    case openhorizon::world::TileMesh::Shading::Water:
        return "water";
    }
    return "?";
}

/// Runs `work(i)` for every i below `count` on `threads` threads.
template <typename Work> void parallelEach(std::size_t count, unsigned threads, const Work& work)
{
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(threads, 1U); ++t) {
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < count; i = next++) {
                work(i);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

/// One level of detail's geometry over the whole map, as uploaded.
struct LevelCost {
    std::size_t chunks = 0;
    std::size_t triangles = 0;
    std::size_t bytes = 0;
    double buildSeconds = 0.0;
};

/// Builds every tile of free roam (route 0), one level of detail at a time,
/// then decodes every texture those tiles use, and prints the totals.
int reportSizes(const openhorizon::world::TrackWorld& world, unsigned threads)
{
    const openhorizon::world::WorldIndex& index = world.index;
    const openhorizon::world::WorldTileGrid grid(index, kTileSize, openhorizon::world::EventPropFilter::route(0));
    const openhorizon::world::TrackTextures* textures = world.textures ? &*world.textures : nullptr;
    // Levels -1 (no levels) to 3, placed geometry then procedural copies.
    constexpr std::size_t kLevels = 5;
    std::array<LevelCost, kLevels * 2> costs{};
    std::vector<std::size_t> tileBytes(grid.tiles().size(), 0);
    std::set<std::uint32_t> textureIds;
    std::mutex mutex;
    const auto start = std::chrono::steady_clock::now();
    parallelEach(grid.tiles().size(), threads, [&](std::size_t t) {
        const openhorizon::world::WorldTileGrid::Tile& tile = grid.tiles()[t];
        std::array<std::vector<std::uint32_t>, kLevels * 2> byLevel;
        for (const std::uint32_t c : tile.chunks) {
            const openhorizon::world::WorldChunk& chunk = index.chunks()[c];
            byLevel[(chunk.scattered ? kLevels : 0) + static_cast<std::size_t>(chunk.lod + 1)].push_back(c);
        }
        for (std::size_t level = 0; level < byLevel.size(); ++level) {
            if (byLevel[level].empty()) {
                continue;
            }
            const auto buildStart = std::chrono::steady_clock::now();
            std::size_t bytes = 0;
            std::size_t triangles = 0;
            std::vector<std::uint32_t> ids;
            if (level < kLevels) {
                const openhorizon::world::ShadedMesh mesh = openhorizon::world::buildShadedMesh(
                    *world.archive, index, byLevel[level], world.shaders, textures);
                bytes = mesh.vertices.size() + mesh.indices.size() * sizeof(std::uint32_t);
                triangles = mesh.triangleCount();
                ids = mesh.textureIds();
            } else {
                const openhorizon::world::TileMesh mesh
                    = openhorizon::world::buildTileMesh(*world.archive, index, byLevel[level], textures);
                bytes = (mesh.vertices.size() + mesh.indices.size()) * sizeof(std::uint32_t);
                triangles = mesh.triangleCount();
                ids = mesh.textures();
            }
            const double seconds = secondsSince(buildStart);
            const std::lock_guard lock(mutex);
            LevelCost& cost = costs[level];
            cost.chunks += byLevel[level].size();
            cost.triangles += triangles;
            cost.bytes += bytes;
            cost.buildSeconds += seconds;
            tileBytes[t] += bytes;
            for (const std::uint32_t id : ids) {
                if (id != openhorizon::world::TileMesh::kNoTexture) {
                    textureIds.insert(id);
                }
            }
        }
    });
    const double geometrySeconds = secondsSince(start);
    std::printf("geometry of %zu tiles, every level of detail, built on %u threads in %.1f s:\n", grid.tiles().size(),
        threads, geometrySeconds);
    std::printf("  %-22s %9s %12s %10s %10s\n", "level", "chunks", "triangles", "MiB", "build s");
    LevelCost total;
    constexpr double kMiB = 1024.0 * 1024.0;
    for (std::size_t level = 0; level < costs.size(); ++level) {
        const LevelCost& cost = costs[level];
        if (cost.chunks == 0) {
            continue;
        }
        const int lod = static_cast<int>(level % kLevels) - 1;
        const std::string name = std::string(level < kLevels ? "placed" : "procedural")
            + (lod < 0 ? std::string(" (no levels)") : " LOD" + std::to_string(lod));
        std::printf("  %-22s %9zu %12zu %10.1f %10.1f\n", name.c_str(), cost.chunks, cost.triangles,
            static_cast<double>(cost.bytes) / kMiB, cost.buildSeconds);
        total.chunks += cost.chunks;
        total.triangles += cost.triangles;
        total.bytes += cost.bytes;
        total.buildSeconds += cost.buildSeconds;
    }
    std::printf("  %-22s %9zu %12zu %10.1f %10.1f\n", "all", total.chunks, total.triangles,
        static_cast<double>(total.bytes) / kMiB, total.buildSeconds);
    std::sort(tileBytes.begin(), tileBytes.end());
    if (!tileBytes.empty()) {
        std::printf("  per tile, all levels: median %.1f MiB, largest %.1f MiB\n",
            static_cast<double>(tileBytes[tileBytes.size() / 2]) / kMiB, static_cast<double>(tileBytes.back()) / kMiB);
    }
    if (textures == nullptr) {
        return 0;
    }

    const std::vector<std::uint32_t> ids(textureIds.begin(), textureIds.end());
    std::size_t textureBytes = 0;
    std::size_t topLevelBytes = 0;
    int failed = 0;
    std::map<std::string, std::pair<int, std::size_t>> byFormat;
    const auto textureStart = std::chrono::steady_clock::now();
    parallelEach(ids.size(), threads, [&](std::size_t i) {
        try {
            const openhorizon::world::TextureMipChain chain = textures->loadTexture(*world.archive, ids[i]);
            const std::lock_guard lock(mutex);
            textureBytes += chain.byteSize();
            topLevelBytes += chain.levels.empty() ? 0 : chain.levels.front().size();
            auto& [count, bytes] = byFormat[formatName(chain.format)];
            ++count;
            bytes += chain.byteSize();
        } catch (const openhorizon::DataError&) {
            const std::lock_guard lock(mutex);
            ++failed;
        }
    });
    std::printf("textures: %zu used (%d missing from the disc), %.1f MiB with their mip chains (%.1f MiB top "
                "levels), decoded on %u threads in %.1f s\n",
        ids.size(), failed, static_cast<double>(textureBytes) / kMiB, static_cast<double>(topLevelBytes) / kMiB,
        threads, secondsSince(textureStart));
    for (const auto& [format, entry] : byFormat) {
        std::printf(
            "  %-6s %6d textures, %10.1f MiB\n", format.c_str(), entry.first, static_cast<double>(entry.second) / kMiB);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5) {
        return usage();
    }
    std::optional<std::pair<float, float>> at;
    std::optional<unsigned> sizesThreads;
    if (argc >= 4 && std::string_view(argv[3]) == "--sizes") {
        sizesThreads = argc == 5 ? static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)) : 4U;
        if (*sizesThreads == 0) {
            return usage();
        }
    } else if (argc == 5) {
        at = parsePoint(argv[4]);
        if (std::string_view(argv[3]) != "--at" || !at) {
            return usage();
        }
    } else if (argc != 3) {
        return usage();
    }
    try {
        openhorizon::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string track = argv[2];

        const auto loadStart = std::chrono::steady_clock::now();
        std::optional<openhorizon::world::TrackWorld> loaded = openhorizon::world::TrackWorld::load(vfs, track);
        if (!loaded) {
            return 1;
        }
        const openhorizon::world::TrackWorld& world = *loaded;
        const double loadSeconds = secondsSince(loadStart);
        for (const std::string& warning : world.warnings) {
            openhorizon::logWarning("{}", warning);
        }

        const openhorizon::world::WorldIndex& index = world.index;
        std::size_t backdrop = 0;
        for (const openhorizon::world::WorldChunk& chunk : index.chunks()) {
            backdrop += chunk.backdrop ? 1 : 0;
        }
        const std::size_t placed = index.placedCount();
        const std::size_t scattered = index.scatteredCount();
        std::printf("track %s: %zu archive entries\n", track.c_str(), world.archive->entries().size());
        std::printf("chunks: %zu (%zu world-space, %zu backdrop, %zu placed props: %zu from zone files, %zu "
                    "procedural)\n",
            index.chunks().size(), index.chunks().size() - placed, backdrop, placed, placed - scattered, scattered);
        std::printf("local models never placed: %d\n", index.localModelCount());
        if (world.placements) {
            const openhorizon::world::TrackPlacements& placements = *world.placements;
            std::printf("draw records: %zu, zones: %d (%d failed), procedural sets: %zu (%d failed), scatter "
                        "instances: %zu\n",
                placements.drawCount(), placements.zoneCount(), placements.failedZones(),
                placements.scatterSets().size(), placements.failedScatterSets(), placements.scatterInstanceCount());
        }
        if (world.zones) {
            std::printf("zone grid: %d zones in %d x %d cells\n", world.zones->zoneCount(), world.zones->columns(),
                world.zones->rows());
        }
        std::printf("track effects: %zu (%d unreadable)\n", world.shaders.size(), world.shaders.failed());
        if (world.textures) {
            std::printf("textures: %zu render objects, %zu shaders, %zu bundled copies\n",
                world.textures->objectCount(), world.textures->shaderCount(), world.textures->bundledTextureCount());
        }
        std::printf("load time: %.2f s\n", loadSeconds);
        if (sizesThreads) {
            return reportSizes(world, *sizesThreads);
        }

        const openhorizon::world::Footprint footprint = index.footprint();
        const auto [x, z] = at ? *at : std::pair{footprint.centre().x, footprint.centre().y};
        std::printf("footprint: X %.0f .. %.0f, Z %.0f .. %.0f\n", footprint.min.x, footprint.max.x, footprint.min.y,
            footprint.max.y);
        const int zone = world.zones ? world.zones->zoneAt(x, z) : -1;
        std::printf("tiles of %.0f m around X %.1f, Z %.1f (zone %d):\n", kTileSize, x, z, zone);

        const openhorizon::world::WorldTileGrid grid(index, kTileSize);
        const auto [cellX, cellZ] = grid.cellAt(x, z);
        struct Job {
            std::size_t tile = 0;
            float distance = 0.0F;
            std::vector<std::uint32_t> chunks;
        };
        std::vector<Job> jobs;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const std::optional<std::size_t> tile = grid.tileAt(cellX + dx, cellZ + dz);
                if (!tile) {
                    std::printf("  tile %d,%d: empty\n", cellX + dx, cellZ + dz);
                    continue;
                }
                const openhorizon::world::WorldTileGrid::Tile& t = grid.tiles()[*tile];
                const float distance = grid.distanceTo(t, x, z);
                std::vector<std::uint32_t> chunks;
                for (const std::uint32_t c : grid.chunksAt(t, distance)) {
                    if (index.chunks()[c].visibleFrom(zone)) {
                        chunks.push_back(c);
                    }
                }
                jobs.push_back({*tile, distance, std::move(chunks)});
            }
        }
        const openhorizon::world::TrackTextures* textures = world.textures ? &*world.textures : nullptr;
        const auto meshStart = std::chrono::steady_clock::now();
        std::vector<std::future<openhorizon::world::TileMesh>> futures;
        futures.reserve(jobs.size());
        for (const Job& job : jobs) {
            futures.push_back(std::async(std::launch::async, [&world, &index, &job, textures] {
                return openhorizon::world::buildTileMesh(*world.archive, index, job.chunks, textures);
            }));
        }
        std::vector<openhorizon::world::TileMesh> meshes;
        meshes.reserve(futures.size());
        for (std::future<openhorizon::world::TileMesh>& future : futures) {
            meshes.push_back(future.get());
        }
        const double meshSeconds = secondsSince(meshStart);

        std::size_t totalTriangles = 0;
        std::size_t totalBatches = 0;
        int totalFailed = 0;
        std::vector<std::uint32_t> textureIds;
        std::map<openhorizon::world::TileMesh::Shading, std::size_t> batchesByShading;
        for (std::size_t j = 0; j < jobs.size(); ++j) {
            const openhorizon::world::TileMesh& mesh = meshes[j];
            const openhorizon::world::WorldTileGrid::Tile& t = grid.tiles()[jobs[j].tile];
            std::printf("  tile %d,%d at %.0f m: %zu chunks, %zu vertices, %zu triangles, %zu batches, %d failed\n",
                t.cellX, t.cellZ, jobs[j].distance, jobs[j].chunks.size(), mesh.vertexCount(), mesh.triangleCount(),
                mesh.batches.size(), mesh.failedChunks);
            for (const std::string& error : mesh.errors) {
                std::printf("    %s\n", error.c_str());
            }
            totalTriangles += mesh.triangleCount();
            totalBatches += mesh.batches.size();
            totalFailed += mesh.failedChunks;
            for (const openhorizon::world::TileMesh::Batch& batch : mesh.batches) {
                ++batchesByShading[batch.shading];
            }
            for (const std::uint32_t id : mesh.textures()) {
                if (std::find(textureIds.begin(), textureIds.end(), id) == textureIds.end()) {
                    textureIds.push_back(id);
                }
            }
        }
        std::printf("tile meshes: %zu triangles, %zu batches (", totalTriangles, totalBatches);
        bool first = true;
        for (const auto& [shading, count] : batchesByShading) {
            std::printf("%s%zu %s", first ? "" : ", ", count, shadingName(shading));
            first = false;
        }
        std::printf("), %d failed chunks, %.2f s\n", totalFailed, meshSeconds);

        // The same tiles as the renderer draws them, with the track effects.
        std::size_t shadedTriangles = 0;
        std::size_t shadedBatches = 0;
        std::size_t shadedBytes = 0;
        int skippedMaterials = 0;
        std::map<std::string, std::size_t> batchesByEffect;
        for (const Job& job : jobs) {
            const openhorizon::world::ShadedMesh shaded
                = openhorizon::world::buildShadedMesh(*world.archive, index, job.chunks, world.shaders, textures);
            shadedTriangles += shaded.triangleCount();
            shadedBatches += shaded.batches.size();
            shadedBytes += shaded.vertices.size();
            skippedMaterials += shaded.skippedMaterials;
            for (const openhorizon::world::ShadedMesh::Batch& batch : shaded.batches) {
                ++batchesByEffect[world.shaders.at(batch.shader).name];
            }
        }
        std::printf("shaded meshes: %zu triangles, %zu batches of %zu effects, %zu vertex bytes, %d materials "
                    "skipped\n",
            shadedTriangles, shadedBatches, batchesByEffect.size(), shadedBytes, skippedMaterials);

        if (textures == nullptr) {
            return totalFailed == 0 ? 0 : 1;
        }
        const std::size_t sample = std::min(textureIds.size(), kSampleTextures);
        std::printf("decoding %zu of the %zu textures the tiles use:\n", sample, textureIds.size());
        const auto textureStart = std::chrono::steady_clock::now();
        int failedTextures = 0;
        std::map<std::string, int> byFormat;
        for (std::size_t i = 0; i < sample; ++i) {
            const std::uint32_t id = textureIds[i];
            try {
                const openhorizon::world::TextureMipChain chain = textures->loadTexture(*world.archive, id);
                ++byFormat[formatName(chain.format)];
                std::printf("  %s: %s %dx%d, %zu levels, %zu bytes, from %s\n",
                    openhorizon::world::TrackTextures::textureName(id).c_str(), formatName(chain.format), chain.width,
                    chain.height, chain.levels.size(), chain.byteSize(), chain.files.c_str());
            } catch (const openhorizon::DataError& e) {
                ++failedTextures;
                std::printf("  %s: FAILED: %s\n", openhorizon::world::TrackTextures::textureName(id).c_str(), e.what());
            }
        }
        std::printf("textures: %d failed of %zu (", failedTextures, sample);
        first = true;
        for (const auto& [format, count] : byFormat) {
            std::printf("%s%d %s", first ? "" : ", ", count, format.c_str());
            first = false;
        }
        std::printf("), %.2f s\n", secondsSince(textureStart));
        return totalFailed == 0 && failedTextures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        openhorizon::logError("{}", e.what());
        return 1;
    }
}
