#include "CollisionIndex.h"

#include "base/Error.h"

#include <algorithm>
#include <cmath>

namespace openhorizon::collision {

namespace {

constexpr std::uint32_t kMagic = 0xABCD1234;
constexpr std::size_t kMagicOffset = 0x30;
constexpr std::size_t kCountOffset = 0x4C;
constexpr std::size_t kRecordsOffset = 0x58;
constexpr std::size_t kRecordBytes = 24;
/// Colorado's grid is 133 x 100; this only rejects garbage.
constexpr std::uint32_t kMaxCellsPerAxis = 4096;

} // namespace

CollisionIndex CollisionIndex::parse(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    CollisionIndex index;
    index.m_columns = r.u32();
    index.m_rows = r.u32();
    index.m_origin.x = r.f32();
    index.m_origin.y = r.f32();
    index.m_extent.x = r.f32();
    index.m_extent.y = r.f32();
    if (index.m_columns == 0 || index.m_rows == 0 || index.m_columns > kMaxCellsPerAxis
        || index.m_rows > kMaxCellsPerAxis) {
        r.fail(std::format("implausible grid of {} x {} cells", index.m_columns, index.m_rows));
    }
    r.seek(kMagicOffset);
    if (r.u32() != kMagic) {
        r.fail("not a collision index (no 0xABCD1234 marker)");
    }
    index.m_cellSize.x = r.f32();
    index.m_cellSize.y = r.f32();
    index.m_margin = r.f32();
    if (!(index.m_cellSize.x > 0.0F) || !(index.m_cellSize.y > 0.0F) || !std::isfinite(index.m_origin.x)
        || !std::isfinite(index.m_origin.y) || !(index.m_margin >= 0.0F)) {
        r.fail("bad grid origin, cell size or margin");
    }

    r.seek(kCountOffset);
    const std::uint32_t count = r.u32();
    const std::size_t cellCount = static_cast<std::size_t>(index.m_columns) * index.m_rows;
    if (count > cellCount) {
        r.fail(std::format("{} tiles for {} cells", count, cellCount));
    }
    r.seek(kRecordsOffset);
    r.need(static_cast<std::size_t>(count) * kRecordBytes);
    index.m_tileOfCell.assign(cellCount, -1);
    index.m_tiles.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Tile tile;
        tile.id = r.u32();
        tile.cell = r.u32();
        tile.packOffset = r.u32();
        tile.sectionSize = r.u32();
        r.skip(8);
        if (tile.cell >= cellCount) {
            r.fail(std::format("tile {} lies in cell {}, outside the grid", tile.id, tile.cell));
        }
        if (index.m_tileOfCell[tile.cell] >= 0) {
            r.fail(std::format("tiles {} and {} share cell {}", index.m_tiles[index.m_tileOfCell[tile.cell]].id,
                tile.id, tile.cell));
        }
        index.m_tileOfCell[tile.cell] = static_cast<std::int32_t>(index.m_tiles.size());
        index.m_tiles.push_back(tile);
    }
    return index;
}

std::string CollisionIndex::pathOf(std::string_view track, int ribbon)
{
    return std::format("media/tracks/{0}/Ribbon_{1:02}/{0}_track_{1:02}.col", track, ribbon);
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> CollisionIndex::cellAt(float x, float z) const
{
    const float column = std::floor((x - m_origin.x) / m_cellSize.x);
    const float row = std::floor((z - m_origin.y) / m_cellSize.y);
    if (!(column >= 0.0F) || !(row >= 0.0F) || column >= static_cast<float>(m_columns)
        || row >= static_cast<float>(m_rows)) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::uint32_t>(column), static_cast<std::uint32_t>(row)};
}

std::optional<std::size_t> CollisionIndex::tileAtCell(std::uint32_t column, std::uint32_t row) const
{
    if (column >= m_columns || row >= m_rows) {
        return std::nullopt;
    }
    const std::int32_t tile = m_tileOfCell[static_cast<std::size_t>(row) * m_columns + column];
    if (tile < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(tile);
}

std::optional<std::size_t> CollisionIndex::tileAt(float x, float z) const
{
    const auto cell = cellAt(x, z);
    if (!cell) {
        return std::nullopt;
    }
    return tileAtCell(cell->first, cell->second);
}

std::vector<std::size_t> CollisionIndex::tilesOverlapping(glm::vec2 lo, glm::vec2 hi) const
{
    std::vector<std::size_t> result;
    // A tile reaches `margin` beyond its cell, so widen the search by that.
    const float x0 = std::floor((lo.x - m_margin - m_origin.x) / m_cellSize.x);
    const float z0 = std::floor((lo.y - m_margin - m_origin.y) / m_cellSize.y);
    const float x1 = std::floor((hi.x + m_margin - m_origin.x) / m_cellSize.x);
    const float z1 = std::floor((hi.y + m_margin - m_origin.y) / m_cellSize.y);
    if (!(x1 >= 0.0F) || !(z1 >= 0.0F) || !(x0 < static_cast<float>(m_columns))
        || !(z0 < static_cast<float>(m_rows))) {
        return result;
    }
    const auto first = [](float v) { return static_cast<std::uint32_t>(std::max(v, 0.0F)); };
    const auto last = [](float v, std::uint32_t count) {
        return static_cast<std::uint32_t>(std::min(v, static_cast<float>(count - 1)));
    };
    for (std::uint32_t row = first(z0); row <= last(z1, m_rows); ++row) {
        for (std::uint32_t column = first(x0); column <= last(x1, m_columns); ++column) {
            const std::optional<std::size_t> tile = tileAtCell(column, row);
            if (!tile) {
                continue;
            }
            const auto [bmin, bmax] = tileBounds(m_tiles[*tile]);
            if (bmin.x <= hi.x && bmax.x >= lo.x && bmin.y <= hi.y && bmax.y >= lo.y) {
                result.push_back(*tile);
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::pair<glm::vec2, glm::vec2> CollisionIndex::cellBounds(std::uint32_t column, std::uint32_t row) const
{
    const glm::vec2 min = m_origin + glm::vec2(static_cast<float>(column), static_cast<float>(row)) * m_cellSize;
    return {min, min + m_cellSize};
}

std::pair<glm::vec2, glm::vec2> CollisionIndex::tileBounds(const Tile& tile) const
{
    const auto [min, max] = cellBounds(tile.cell % m_columns, tile.cell / m_columns);
    return {min - glm::vec2(m_margin), max + glm::vec2(m_margin)};
}

} // namespace openhorizon::collision
