#include "ZoneGrid.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace fh::world {

namespace {

constexpr std::uint32_t kNoZone = 0xFFFFFFFF;
constexpr std::uint32_t kMaxCells = 1'000'000;
constexpr float kSqrt3 = std::numbers::sqrt3_v<float>;

} // namespace

ZoneGrid ZoneGrid::read(ByteView data)
{
    BeReader r(data, "zone grid");
    if (data.size() < 4 || std::memcmp(data.data(), "HEXY", 4) != 0) {
        throw DataError("not a zone grid file");
    }
    r.skip(8);
    ZoneGrid grid;
    grid.m_radius = r.f32();
    grid.m_originX = r.f32();
    grid.m_originZ = r.f32();
    const std::uint32_t zones = r.u32();
    const std::uint32_t columns = r.u32();
    const std::uint32_t rows = r.u32();
    if (!(grid.m_radius > 0.0F) || !std::isfinite(grid.m_radius) || !std::isfinite(grid.m_originX)
        || !std::isfinite(grid.m_originZ) || columns == 0 || rows == 0 || columns > kMaxCells / rows
        || zones > kMaxCells) {
        throw DataError("the zone grid header is not valid");
    }
    const std::size_t cellCount = static_cast<std::size_t>(columns) * rows;
    const ByteView cells = r.bytes(cellCount * 4);
    grid.m_zoneCount = static_cast<int>(zones);
    grid.m_columns = static_cast<int>(columns);
    grid.m_rows = static_cast<int>(rows);
    grid.m_cells.reserve(cellCount);
    for (std::size_t i = 0; i < cellCount; ++i) {
        const std::uint32_t zone = loadBe32(cells.data() + i * 4);
        if (zone != kNoZone && zone >= zones) {
            throw DataError("zone grid cell {} names zone {} of {}", i, zone, zones);
        }
        grid.m_cells.push_back(zone == kNoZone ? -1 : static_cast<std::int32_t>(zone));
    }
    return grid;
}

int ZoneGrid::zoneOfCell(int column, int row) const
{
    return m_cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_columns)
        + static_cast<std::size_t>(column)];
}

int ZoneGrid::zoneAt(float x, float z) const
{
    if (m_cells.empty()) {
        return -1;
    }
    const float rowPitch = kSqrt3 * m_radius;
    const int nearColumn = static_cast<int>(std::floor((x - m_originX - m_radius) / (1.5F * m_radius)));
    float best = std::numeric_limits<float>::infinity();
    int zone = -1;
    // The nearest centre is always in one of the columns next to the one
    // the point falls in, and in the row nearest to it within that column.
    for (int column = nearColumn - 1; column <= nearColumn + 2; ++column) {
        if (column < 0 || column >= m_columns) {
            continue;
        }
        const float centreX = m_originX + m_radius * (1.0F + 1.5F * static_cast<float>(column));
        const float firstCentreZ = m_originZ + rowPitch * ((column & 1) != 0 ? 1.0F : 0.5F);
        const int nearRow = static_cast<int>(std::lround((z - firstCentreZ) / rowPitch));
        for (int row = nearRow - 1; row <= nearRow + 1; ++row) {
            if (row < 0 || row >= m_rows) {
                continue;
            }
            const float distance = std::hypot(x - centreX, z - (firstCentreZ + rowPitch * static_cast<float>(row)));
            if (distance < best) {
                best = distance;
                zone = zoneOfCell(column, row);
            }
        }
    }
    // Past the grid's edge the nearest cell is further than its own corners.
    return best <= m_radius ? zone : -1;
}

} // namespace fh::world
