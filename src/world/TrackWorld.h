#pragma once

#include "Archive.h"
#include "ScatterSet.h"
#include "TrackPlacements.h"
#include "TrackSettings.h"
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

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::world {

/// Game paths of a track's world files, for a track folder name such as "colorado".
struct TrackPaths {
    /// Mount point of the track's bin.zip.
    std::string archive;
    /// `Ribbon_00/<Track>_00.pvs`: texture tables and draw records.
    std::string pvs;
    /// `Ribbon_00/<Track>_00.hex`: the visibility zone grid.
    std::string zoneGrid;

    static TrackPaths of(std::string_view track);
};

/// Everything a renderer needs to draw a track's world.
struct TrackWorld {
    std::shared_ptr<const lzx::ZipArchive> archive;
    /// Missing without a zone grid file; the backdrop terrain is then drawn from everywhere.
    std::optional<ZoneGrid> zones;
    /// Missing when the PVS file cannot be read; props are then left out.
    std::optional<TrackPlacements> placements;
    /// Missing when the PVS file cannot be read; geometry is then untextured.
    std::optional<TrackTextures> textures;
    TrackShaders shaders;
    /// Missing when TrackSettings.xml cannot be read; the scene constants it gives are then left 0.
    std::optional<TrackSettings> settings;
    WorldIndex index;
    /// What was missing or could not be read, for the log.
    std::vector<std::string> warnings;

    /// `track` is a folder of media/tracks. Throws DataError without a bin.zip; nothing if `cancel` was set.
    static std::optional<TrackWorld> load(const Vfs& vfs, std::string_view track, const Progress& progress = {},
        const std::atomic<bool>* cancel = nullptr);
};

} // namespace openhorizon::world
