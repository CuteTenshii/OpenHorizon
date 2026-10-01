#pragma once

#include "base/Bytes.h"

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::collision {

/// A track's collision grid: `Ribbon_NN/<Track>_track_NN.col`, the index of
/// the `<id>.fiz` tiles in the track's bin.zip, which ships them as separate entries.
///
/// Layout (big-endian unless noted):
///
///     0x00  u32 columns (along X), u32 rows (along Z)            verified
///     0x08  f32 minX, minZ, maxX, maxZ: extent of the tiles'
///           cells, grid origin at (minX, minZ)                   verified
///     0x18  f32 cell width and depth, stored little-endian (100) inferred
///     0x20  f32 1 / cell width, 1 / cell depth                    verified
///     0x28  u32 end of the pack                                   inferred
///     0x2C  u32 4                                                 unknown
///     0x30  u32 0xABCD1234                                        verified
///     0x34  f32 cell width, cell depth (100 m)                    verified
///     0x3C  f32 margin by which a tile's bounds exceed its cell
///           on every side (8 m)                                   verified
///     0x44  f32 the tiles' grid cell size, little-endian (10 m)   inferred
///     0x4C  u32 tile count                                        verified
///     0x50  u32 0x54                                              unknown
///     0x54  u32 pack offset of the first tile (0x13000)           verified
///     0x58  per tile, 24 bytes:                                   verified
///             u32 tile id (the entry "<id>.fiz"; ids run 0, 1, ...)
///             u32 cell: row · columns + column
///             u32 pack offset of the tile's section A
///             u32 size of section A, as in the tile's header
///             u32 0
///             u32 pack offset after section A (0 for the last tile)
///
/// A tile's bounds are its cell plus the margin on every side. Each cell has
/// at most one tile; cells without one have no collision.
class CollisionIndex {
public:
    struct Tile {
        std::uint32_t id = 0;
        std::uint32_t cell = 0;
        std::uint32_t packOffset = 0;
        std::uint32_t sectionSize = 0;
    };

    /// Decodes a `.col` file; `source` names it in errors. Throws
    /// DataError for a bad magic, sizes that do not fit, a cell outside the
    /// grid or two tiles in one cell.
    static CollisionIndex parse(ByteView data, const std::string& source);

    /// Game path of a track's index: "media/tracks/<track>/Ribbon_<NN>/
    /// <track>_track_<NN>.col".
    static std::string pathOf(std::string_view track, int ribbon = 0);

    std::uint32_t columns() const { return m_columns; }
    std::uint32_t rows() const { return m_rows; }
    /// World XZ of the grid's corner (x = world X, y = world Z).
    glm::vec2 origin() const { return m_origin; }
    /// Largest XZ the tiles' cells reach, as stored.
    glm::vec2 extent() const { return m_extent; }
    glm::vec2 cellSize() const { return m_cellSize; }
    float margin() const { return m_margin; }
    const std::vector<Tile>& tiles() const { return m_tiles; }

    /// The cell holding world point (x, z), or nothing outside the grid.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> cellAt(float x, float z) const;
    /// The index into tiles() of the tile of a cell, if it has one.
    std::optional<std::size_t> tileAtCell(std::uint32_t column, std::uint32_t row) const;
    /// The index into tiles() of the tile whose cell holds (x, z).
    std::optional<std::size_t> tileAt(float x, float z) const;
    /// Indices into tiles() of the tiles whose bounds (cell plus margin)
    /// overlap the XZ rectangle [lo, hi]; the ones covering a point when
    /// lo == hi. In increasing tile order.
    std::vector<std::size_t> tilesOverlapping(glm::vec2 lo, glm::vec2 hi) const;

    /// A tile's bounds (cell plus margin), and its cell alone.
    std::pair<glm::vec2, glm::vec2> tileBounds(const Tile& tile) const;
    std::pair<glm::vec2, glm::vec2> cellBounds(std::uint32_t column, std::uint32_t row) const;

private:
    std::uint32_t m_columns = 0;
    std::uint32_t m_rows = 0;
    glm::vec2 m_origin{0.0F};
    glm::vec2 m_extent{0.0F};
    glm::vec2 m_cellSize{100.0F};
    float m_margin = 0.0F;
    std::vector<Tile> m_tiles;
    /// Index into m_tiles per cell, row-major; -1 for empty cells.
    std::vector<std::int32_t> m_tileOfCell;
};

} // namespace openhorizon::collision
