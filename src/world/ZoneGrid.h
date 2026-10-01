#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <vector>

namespace openhorizon::world {

/// Which visibility zone (see TrackPlacements) covers each point of a track,
/// read from `Ribbon_00/<Track>_00.hex` (big-endian):
///
///     "HEXY", u32 version (101), f32 cell radius (100),
///     f32 origin X, f32 origin Z, u32 zone count, u32 columns, u32 rows,
///     columns × rows u32 zone numbers, row by row, 0xFFFFFFFF where no zone
///     is; then 9 bytes per zone, not needed here
///
/// Flat-topped hexagons centred at X = originX + radius·(1 + 1.5c), Z = originZ + radius·√3·(r + ½), odd columns
/// half a row further along Z; zone n is `__R00Z<n>.pvsz`. Cell placement inferred from each zone's props.
class ZoneGrid {
public:
    /// An empty grid: every point is outside it.
    ZoneGrid() = default;

    /// Throws DataError if the data does not have the expected layout.
    static ZoneGrid read(ByteView data);

    /// The zone of the cell whose centre is nearest to X, Z; -1 outside the grid or in a cell without a zone.
    int zoneAt(float x, float z) const;
    int zoneCount() const { return m_zoneCount; }
    int columns() const { return m_columns; }
    int rows() const { return m_rows; }

private:
    int zoneOfCell(int column, int row) const;

    float m_radius = 0.0F;
    float m_originX = 0.0F;
    float m_originZ = 0.0F;
    int m_zoneCount = 0;
    int m_columns = 0;
    int m_rows = 0;
    /// Zone per cell, row by row; -1 for none.
    std::vector<std::int32_t> m_cells;
};

} // namespace openhorizon::world
