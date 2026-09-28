#include "FizTile.h"

#include "base/Error.h"

#include <cmath>
#include <limits>

namespace fh::collision {

namespace {

constexpr std::uint32_t kMagic = 0x66697A20; // "fiz "
constexpr std::uint32_t kOwbbMagic = 0x4F574242; // "OWBB"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kFileHeaderBytes = 0x20;
constexpr std::size_t kSectionHeaderBytes = 0x90;
constexpr std::size_t kVertexBytes = 16;
constexpr std::size_t kTriangleBytes = 32;
constexpr std::size_t kRouteListOffset = 0x3C;
constexpr std::size_t kRouteCountOffset = 0x78;
/// The route mask has 15 usable bits; bit 15 marks ordinary geometry.
constexpr std::uint32_t kMaxRoutes = 15;
constexpr std::size_t kOwbbFrameBytes = 16;
/// A 12 x 12 grid in every Colorado tile; anything far larger is corrupt.
constexpr std::uint32_t kMaxCellsPerAxis = 1024;

std::int32_t signExtend(std::uint32_t value, int bits)
{
    const std::uint32_t sign = 1U << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

bool finite(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

std::optional<OwbbBlock> parseOwbb(ByteView section, const std::string& source)
{
    if (section.empty()) {
        return std::nullopt;
    }
    BeReader r(section, source + " (OWBB block)");
    if (r.u32() != kOwbbMagic) {
        r.fail("no OWBB magic");
    }
    r.skip(4);
    OwbbBlock block;
    block.hash = r.u32();
    const std::uint32_t payloadSize = r.u32();
    if (payloadSize != section.size() - 2 * kOwbbFrameBytes) {
        r.fail(std::format("payload size {} does not fit the section's {} bytes", payloadSize, section.size()));
    }
    const ByteView payload = r.bytes(payloadSize);
    BeReader p(payload, r.source());
    block.id = p.u32();
    block.cell = p.u32();
    block.payload.assign(payload.begin(), payload.end());
    return block;
}

} // namespace

glm::vec3 unpackFizNormal(std::uint32_t packed)
{
    return {static_cast<float>(signExtend(packed & 0x7FFU, 11)) / 1023.0F,
        static_cast<float>(signExtend((packed >> 11) & 0x7FFU, 11)) / 1023.0F,
        static_cast<float>(signExtend(packed >> 22, 10)) / 511.0F};
}

bool FizTile::present(const FizTriangle& triangle, int activeRoute) const
{
    if (!triangle.belongsToRoutes() || activeRoute == kAllRoutes) {
        return true;
    }
    if (activeRoute < 0) {
        return false;
    }
    for (std::size_t k = 0; k < routes.size(); ++k) {
        if ((triangle.routeMask >> k & 1U) != 0 && routes[k] == activeRoute) {
            return true;
        }
    }
    return false;
}

FizTile parseFizTile(ByteView data, const std::string& source)
{
    BeReader header(data, source);
    if (header.u32() != kMagic) {
        header.fail("not a .fiz file (no \"fiz \" magic)");
    }
    FizTile tile;
    tile.version = header.u32();
    if (tile.version != kVersion) {
        header.fail(std::format("unsupported version {}", tile.version));
    }
    const std::uint32_t sizeA = header.u32();
    const std::uint32_t sizeB = header.u32();
    tile.headerId = header.u32();
    if (sizeA < kSectionHeaderBytes || sizeA > data.size() - kFileHeaderBytes
        || sizeB > data.size() - kFileHeaderBytes - sizeA) {
        header.fail(std::format("section sizes 0x{:x} and 0x{:x} do not fit 0x{:x} bytes", sizeA, sizeB,
            data.size()));
    }

    const ByteView a = data.subspan(kFileHeaderBytes, sizeA);
    BeReader r(a, source);
    const std::uint32_t vertexCount = r.u32();
    const std::uint32_t triangleCount = r.u32();
    const std::uint32_t vertexOffset = r.u32();
    const std::uint32_t triangleOffset = r.u32();
    tile.cellsX = r.u32();
    tile.cellsZ = r.u32();
    tile.boundsMin.x = r.f32();
    tile.boundsMin.y = r.f32();
    tile.boundsMax.x = r.f32();
    tile.boundsMax.y = r.f32();
    tile.cellSize.x = r.f32();
    tile.cellSize.y = r.f32();
    r.skip(8);
    const std::uint32_t gridOffset = r.u32();

    // Triangles address vertices, and cells triangles, with 16-bit indices.
    if (vertexCount > std::numeric_limits<std::uint16_t>::max() + 1U
        || triangleCount > std::numeric_limits<std::uint16_t>::max() + 1U) {
        r.fail(std::format("implausible counts: {} vertices, {} triangles", vertexCount, triangleCount));
    }
    if (tile.cellsX == 0 || tile.cellsZ == 0 || tile.cellsX > kMaxCellsPerAxis || tile.cellsZ > kMaxCellsPerAxis) {
        r.fail(std::format("implausible grid of {} x {} cells", tile.cellsX, tile.cellsZ));
    }
    if (!(tile.cellSize.x > 0.0F) || !(tile.cellSize.y > 0.0F) || !(tile.boundsMax.x >= tile.boundsMin.x)
        || !(tile.boundsMax.y >= tile.boundsMin.y)) {
        r.fail("bad bounds or grid cell size");
    }
    const std::size_t cellCount = static_cast<std::size_t>(tile.cellsX) * tile.cellsZ;
    const auto fits = [&](std::size_t offset, std::size_t bytes) {
        return offset >= kSectionHeaderBytes && offset <= a.size() && bytes <= a.size() - offset;
    };
    if (!fits(vertexOffset, static_cast<std::size_t>(vertexCount) * kVertexBytes)
        || !fits(triangleOffset, static_cast<std::size_t>(triangleCount) * kTriangleBytes)
        || !fits(gridOffset, cellCount * 4)) {
        r.fail(std::format("vertex (0x{:x}), triangle (0x{:x}) or grid (0x{:x}) table out of range", vertexOffset,
            triangleOffset, gridOffset));
    }

    r.seek(kRouteCountOffset);
    const std::uint32_t routeCount = r.u32();
    if (routeCount > kMaxRoutes) {
        r.fail(std::format("{} route ids, at most {} fit", routeCount, kMaxRoutes));
    }
    r.seek(kRouteListOffset);
    tile.routes.reserve(routeCount);
    for (std::uint32_t i = 0; i < routeCount; ++i) {
        tile.routes.push_back(r.u16());
    }

    r.seek(vertexOffset);
    tile.positions.reserve(vertexCount);
    tile.vertexNormals.reserve(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i) {
        glm::vec3 p;
        p.x = r.f32();
        p.y = r.f32();
        p.z = r.f32();
        if (!finite(p)) {
            r.fail(std::format("vertex {} is not finite", i));
        }
        tile.positions.push_back(p);
        tile.vertexNormals.push_back(unpackFizNormal(r.u32()));
    }

    const std::uint32_t routeMaskLimit = 1U << routeCount;
    r.seek(triangleOffset);
    tile.triangles.reserve(triangleCount);
    for (std::uint32_t i = 0; i < triangleCount; ++i) {
        FizTriangle t;
        t.flags = r.u8();
        t.surface = r.u8();
        t.routeMask = r.u16();
        if (t.routeMask != FizTriangle::kAlwaysPresent && (t.routeMask == 0 || t.routeMask >= routeMaskLimit)) {
            r.fail(std::format("triangle {} has route mask 0x{:x} with {} route ids", i, t.routeMask, routeCount));
        }
        for (std::uint16_t& vertex : t.vertices) {
            const std::uint32_t offset = r.u32();
            if (offset < vertexOffset || (offset - vertexOffset) % kVertexBytes != 0
                || (offset - vertexOffset) / kVertexBytes >= vertexCount) {
                r.fail(std::format("triangle {} points at 0x{:x}, not at a vertex", i, offset));
            }
            vertex = static_cast<std::uint16_t>((offset - vertexOffset) / kVertexBytes);
        }
        t.normal.x = r.f32();
        t.normal.y = r.f32();
        t.normal.z = r.f32();
        if (!finite(t.normal)) {
            r.fail(std::format("triangle {} has a non-finite normal", i));
        }
        r.skip(1);
        for (std::uint8_t& edge : t.edgeCosines) {
            edge = r.u8();
        }
        tile.triangles.push_back(t);
    }

    tile.cellStart.reserve(cellCount + 1);
    for (std::size_t cell = 0; cell < cellCount; ++cell) {
        r.seek(gridOffset + cell * 4);
        const std::uint32_t listOffset = r.u32();
        tile.cellStart.push_back(static_cast<std::uint32_t>(tile.cellTriangles.size()));
        if (listOffset == 0) {
            continue;
        }
        r.seek(listOffset);
        const std::uint16_t count = r.u16();
        for (std::uint16_t k = 0; k < count; ++k) {
            const std::uint16_t triangle = r.u16();
            if (triangle >= triangleCount) {
                r.fail(std::format("cell {} lists triangle {} of {}", cell, triangle, triangleCount));
            }
            tile.cellTriangles.push_back(triangle);
        }
    }
    tile.cellStart.push_back(static_cast<std::uint32_t>(tile.cellTriangles.size()));

    tile.owbb = parseOwbb(data.subspan(kFileHeaderBytes + sizeA, sizeB), source);
    return tile;
}

} // namespace fh::collision
