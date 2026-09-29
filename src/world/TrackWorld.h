#pragma once

#include "Archive.h"
#include "ScatterSet.h"
#include "TrackPlacements.h"
#include "TrackShaders.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "ZoneGrid.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace fh {
class Vfs;
} // namespace fh

namespace fh::world {

/// Game paths of a track's world files, for a track folder name such as
/// "colorado".
struct TrackPaths {
    /// Mount point of the track's bin.zip.
    std::string archive;
    /// `Ribbon_00/<Track>_00.pvs`: texture tables and draw records.
    std::string pvs;
    /// `Ribbon_00/<Track>_00.hex`: the visibility zone grid.
    std::string zoneGrid;

    static TrackPaths of(std::string_view track);
};

/// Everything a renderer needs to draw a track's world: its archive, the
/// index of its render models with every prop placed, the texture tables
/// and the zone grid.
struct TrackWorld {
    std::shared_ptr<const lzx::ZipArchive> archive;
    /// Missing when the track has no zone grid file; the backdrop terrain is
    /// then drawn from everywhere.
    std::optional<ZoneGrid> zones;
    /// Missing when the PVS file cannot be read; props are then left out.
    std::optional<TrackPlacements> placements;
    /// Missing when the PVS file cannot be read; geometry is then untextured.
    std::optional<TrackTextures> textures;
    /// The track's effects, which the renderer draws the models with.
    TrackShaders shaders;
    WorldIndex index;
    /// What was missing or could not be read, for the log.
    std::vector<std::string> warnings;

    /// Loads the world of track `track` (a folder of media/tracks). Throws
    /// DataError if the track has no bin.zip. Returns nothing if `cancel`
    /// was set while it ran.
    static std::optional<TrackWorld> load(const Vfs& vfs, std::string_view track, const Progress& progress = {},
        const std::atomic<bool>* cancel = nullptr);
};

} // namespace fh::world
