#include "TrackWorld.h"

#include "vfs/Vfs.h"

#include <lzx/zip.h>

namespace fh::world {

TrackPaths TrackPaths::of(std::string_view track)
{
    const std::string folder = std::format("media/tracks/{}", track);
    return {folder + "/bin", std::format("{}/ribbon_00/{}_00.pvs", folder, track),
        std::format("{}/ribbon_00/{}_00.hex", folder, track)};
}

std::optional<TrackWorld> TrackWorld::load(
    const Vfs& vfs, std::string_view track, const Progress& progress, const std::atomic<bool>* cancel)
{
    const TrackPaths paths = TrackPaths::of(track);
    TrackWorld world;
    world.archive = vfs.archiveAt(paths.archive);
    if (!world.archive) {
        throw DataError("track {} has no archive at {}", track, paths.archive);
    }
    const lzx::ZipArchive& archive = *world.archive;

    if (const std::optional<Bytes> hex = vfs.read(paths.zoneGrid)) {
        try {
            world.zones = ZoneGrid::read(*hex);
        } catch (const DataError& e) {
            world.warnings.push_back(std::format("the backdrop terrain is drawn from everywhere: {}", e.what()));
        }
    } else {
        world.warnings.push_back(std::format("the backdrop terrain is drawn from everywhere: {} is missing",
            paths.zoneGrid));
    }

    if (const std::optional<Bytes> pvs = vfs.read(paths.pvs)) {
        try {
            world.placements = TrackPlacements::load(*pvs, archive, cancel);
            if (!world.placements) {
                return std::nullopt;
            }
            if (world.placements->failedZones() > 0 || world.placements->failedScatterSets() > 0) {
                world.warnings.push_back(std::format("{} zone files and {} procedural sets could not be read",
                    world.placements->failedZones(), world.placements->failedScatterSets()));
            }
            world.textures = TrackTextures::load(*pvs, archive);
        } catch (const DataError& e) {
            world.placements.reset();
            world.textures.reset();
            world.warnings.push_back(std::format("props are not placed and textures are unknown: {}", e.what()));
        }
    } else {
        world.warnings.push_back(
            std::format("props are not placed and textures are unknown: {} is missing", paths.pvs));
    }

    std::optional<WorldIndex> index = WorldIndex::build(archive, progress, cancel,
        world.placements ? &*world.placements : nullptr, world.zones ? &*world.zones : nullptr);
    if (!index) {
        return std::nullopt;
    }
    world.index = std::move(*index);
    return world;
}

} // namespace fh::world
