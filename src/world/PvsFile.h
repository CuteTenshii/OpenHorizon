#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <vector>

namespace fh::world {

/// The tables of a track's PVS file (`Ribbon_00/<Track>_00.pvs`, magic
/// "FPVS", big-endian) that the world needs. At 0x20 the file has a u32 zone
/// count and one u32 per zone; after a gap of varying size, found by its
/// contents within 64 bytes, it holds:
///
///     u32 N, N texture records of 28 bytes:
///         u32 texture id, u32 record number, f32 1, f32 1, u32 0, u32 0,
///         u32 flags
///     u32 count, shader paths (u32 length, text)
///     u32 count, 18-byte draw records, one per placed model: u16 render
///         object, then eight u16 not needed here
///     u32 count, one record per render object:
///         u32 n, n texture record numbers, u32 m, m shader numbers,
///         15 floats (orientation and bounding box)
///
/// Render object i is the file `<track>.<i>.rmb.bin`. A draw record is one
/// placement of a render object; the zone files say where (see
/// TrackPlacements).
struct PvsTables {
    /// Texture id of each texture record.
    std::vector<std::uint32_t> textureIds;
    /// Texture ids used by each render object.
    std::vector<std::vector<std::uint32_t>> objectTextures;
    /// Render object of each draw record.
    std::vector<std::uint16_t> drawObjects;
};

/// Parses a PVS file. Throws DataError if it does not have the expected
/// layout.
PvsTables readPvs(ByteView pvs);

} // namespace fh::world
