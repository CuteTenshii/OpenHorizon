// Loads a track's world the way the engine's streamer does and reports what
// it found: model and prop counts, the tile meshes around one point and a
// sample of their textures.

#include "base/Log.h"
#include "vfs/Vfs.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <lzx/zip.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr float kTileSize = 500.0F;
constexpr std::size_t kSampleTextures = 32;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1world DISC TRACK [--at X,Z]\n"
        "  DISC   the extracted disc (its root or its media folder)\n"
        "  TRACK  a folder of media/tracks, such as colorado\n"
        "  --at   world X and Z to build the 3x3 tiles around; the map centre\n"
        "         by default\n");
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

const char* formatName(fh::world::TextureSurface::Format format)
{
    switch (format) {
    case fh::world::TextureSurface::Format::Dxt1:
        return "DXT1";
    case fh::world::TextureSurface::Format::Dxt3:
        return "DXT3";
    case fh::world::TextureSurface::Format::Dxt5:
        return "DXT5";
    case fh::world::TextureSurface::Format::Bc5:
        return "BC5";
    case fh::world::TextureSurface::Format::Rgba8:
        return "RGBA8";
    }
    return "?";
}

const char* shadingName(fh::world::TileMesh::Shading shading)
{
    switch (shading) {
    case fh::world::TileMesh::Shading::Plain:
        return "plain";
    case fh::world::TileMesh::Shading::Splat:
        return "splat";
    case fh::world::TileMesh::Shading::VertexBlend:
        return "vertex-blend";
    case fh::world::TileMesh::Shading::Water:
        return "water";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 5) {
        return usage();
    }
    std::optional<std::pair<float, float>> at;
    if (argc == 5) {
        at = parsePoint(argv[4]);
        if (std::string_view(argv[3]) != "--at" || !at) {
            return usage();
        }
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string track = argv[2];

        const auto loadStart = std::chrono::steady_clock::now();
        std::optional<fh::world::TrackWorld> loaded = fh::world::TrackWorld::load(vfs, track);
        if (!loaded) {
            return 1;
        }
        const fh::world::TrackWorld& world = *loaded;
        const double loadSeconds = secondsSince(loadStart);
        for (const std::string& warning : world.warnings) {
            fh::logWarning("{}", warning);
        }

        const fh::world::WorldIndex& index = world.index;
        std::size_t backdrop = 0;
        for (const fh::world::WorldChunk& chunk : index.chunks()) {
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
            const fh::world::TrackPlacements& placements = *world.placements;
            std::printf("draw records: %zu, zones: %d (%d failed), procedural sets: %zu (%d failed), scatter "
                        "instances: %zu\n",
                placements.drawCount(), placements.zoneCount(), placements.failedZones(),
                placements.scatterSets().size(), placements.failedScatterSets(), placements.scatterInstanceCount());
        }
        if (world.zones) {
            std::printf("zone grid: %d zones in %d x %d cells\n", world.zones->zoneCount(), world.zones->columns(),
                world.zones->rows());
        }
        if (world.textures) {
            std::printf("textures: %zu render objects, %zu shaders, %zu bundled copies\n",
                world.textures->objectCount(), world.textures->shaderCount(), world.textures->bundledTextureCount());
        }
        std::printf("load time: %.2f s\n", loadSeconds);

        const fh::world::Footprint footprint = index.footprint();
        const auto [x, z] = at ? *at : std::pair{footprint.centre().x, footprint.centre().y};
        std::printf("footprint: X %.0f .. %.0f, Z %.0f .. %.0f\n", footprint.min.x, footprint.max.x, footprint.min.y,
            footprint.max.y);
        const int zone = world.zones ? world.zones->zoneAt(x, z) : -1;
        std::printf("tiles of %.0f m around X %.1f, Z %.1f (zone %d):\n", kTileSize, x, z, zone);

        const fh::world::WorldTileGrid grid(index, kTileSize);
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
                const fh::world::WorldTileGrid::Tile& t = grid.tiles()[*tile];
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
        const fh::world::TrackTextures* textures = world.textures ? &*world.textures : nullptr;
        const auto meshStart = std::chrono::steady_clock::now();
        std::vector<std::future<fh::world::TileMesh>> futures;
        futures.reserve(jobs.size());
        for (const Job& job : jobs) {
            futures.push_back(std::async(std::launch::async, [&world, &index, &job, textures] {
                return fh::world::buildTileMesh(*world.archive, index, job.chunks, textures);
            }));
        }
        std::vector<fh::world::TileMesh> meshes;
        meshes.reserve(futures.size());
        for (std::future<fh::world::TileMesh>& future : futures) {
            meshes.push_back(future.get());
        }
        const double meshSeconds = secondsSince(meshStart);

        std::size_t totalTriangles = 0;
        std::size_t totalBatches = 0;
        int totalFailed = 0;
        std::vector<std::uint32_t> textureIds;
        std::map<fh::world::TileMesh::Shading, std::size_t> batchesByShading;
        for (std::size_t j = 0; j < jobs.size(); ++j) {
            const fh::world::TileMesh& mesh = meshes[j];
            const fh::world::WorldTileGrid::Tile& t = grid.tiles()[jobs[j].tile];
            std::printf("  tile %d,%d at %.0f m: %zu chunks, %zu vertices, %zu triangles, %zu batches, %d failed\n",
                t.cellX, t.cellZ, jobs[j].distance, jobs[j].chunks.size(), mesh.vertexCount(), mesh.triangleCount(),
                mesh.batches.size(), mesh.failedChunks);
            for (const std::string& error : mesh.errors) {
                std::printf("    %s\n", error.c_str());
            }
            totalTriangles += mesh.triangleCount();
            totalBatches += mesh.batches.size();
            totalFailed += mesh.failedChunks;
            for (const fh::world::TileMesh::Batch& batch : mesh.batches) {
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
                const fh::world::TextureMipChain chain = textures->loadTexture(*world.archive, id);
                ++byFormat[formatName(chain.format)];
                std::printf("  %s: %s %dx%d, %zu levels, %zu bytes, from %s\n",
                    fh::world::TrackTextures::textureName(id).c_str(), formatName(chain.format), chain.width,
                    chain.height, chain.levels.size(), chain.byteSize(), chain.files.c_str());
            } catch (const fh::DataError& e) {
                ++failedTextures;
                std::printf("  %s: FAILED: %s\n", fh::world::TrackTextures::textureName(id).c_str(), e.what());
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
        fh::logError("{}", e.what());
        return 1;
    }
}
