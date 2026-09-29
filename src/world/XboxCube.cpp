#include "XboxCube.h"

#include "Archive.h"
#include "TrackTextures.h"

#include "base/Error.h"

#include <lzx/zip.h>
#include <xds/xds.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace fh::world {

namespace {

/// Tiled surfaces are allocated in whole 32x32-block tiles, each surface
/// starting on a 4 KiB page (as xds lays out 2D textures).
constexpr std::uint32_t kTileBlocks = 32;
constexpr std::uint64_t kPageBytes = 4096;
/// Levels of 16 texels or less on their shorter side share the packed tail.
constexpr int kPackedTailLog2 = 4;
constexpr int kMaxCubeSize = 4096;

constexpr std::size_t kXprPrefixBytes = 12;
constexpr std::size_t kXprRecordBytes = 16;
constexpr std::size_t kD3dHeaderBytes = 52;
/// The fetch constant is the last 24 bytes of the Direct3D texture header.
constexpr std::size_t kFetchConstantOffset = 28;
constexpr std::uint32_t kCubeResourceType = 0x5458434D; // "TXCM"

/// Offsets in a CAFF container (see readCaffSurface).
constexpr std::size_t kCaffHeaderSizeOffset = 0x48;
constexpr std::size_t kCaffDataSizeOffset = 0x60;
constexpr std::size_t kCaffGpuSizeOffset = 0x9C;
constexpr std::size_t kCaffFormatOffset = 0x18;
constexpr std::size_t kCaffKindOffset = 0x1C;
constexpr std::size_t kCaffSizeOffset = 0x24;
constexpr std::size_t kCaffLevelsOffset = 0x30;
constexpr std::uint32_t kCaffCubeKind = 2;

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

TextureSurface::Format surfaceFormat(std::uint32_t format)
{
    switch (static_cast<xds::Format>(format)) {
    case xds::Format::kDxt1:
        return TextureSurface::Format::Dxt1;
    case xds::Format::kDxt2_3:
        return TextureSurface::Format::Dxt3;
    case xds::Format::kDxt4_5:
        return TextureSurface::Format::Dxt5;
    case xds::Format::k8_8_8_8:
        return TextureSurface::Format::Rgba8;
    default:
        throw DataError("cube texture format {} ({}) is not supported", format, xds::formatName(format));
    }
}

/// Bytes one face of a level takes in memory: whole tiles, whole pages.
std::uint64_t faceSurfaceBytes(int size, std::uint32_t format)
{
    const std::uint32_t edge = xds::blockEdge(format);
    const std::uint64_t blocks = (static_cast<std::uint32_t>(size) + edge - 1) / edge;
    const std::uint64_t side = alignUp(blocks, kTileBlocks);
    return alignUp(side * side * xds::bytesPerBlock(format), kPageBytes);
}

/// A 2D texture that reads one face's surface: its top level `size`
/// texels at `offset`, with `levels` levels in a packed tail when `packed`.
xds::TextureInfo faceInfo(const XboxCubeLayout& layout, int size, int levels, std::uint64_t offset, bool packed)
{
    xds::TextureInfo info;
    info.format = layout.formatWord & 0x3F;
    info.endian = static_cast<xds::Endian>((layout.formatWord >> 6) & 3);
    info.dimension = xds::Dimension::TwoD;
    info.tiled = true;
    info.packedMips = packed;
    info.width = static_cast<std::uint32_t>(size);
    info.height = static_cast<std::uint32_t>(size);
    const std::uint32_t edge = xds::blockEdge(info.format);
    info.pitch = static_cast<std::uint32_t>(alignUp((info.width + edge - 1) / edge, kTileBlocks)) * edge;
    info.baseAddress = static_cast<std::uint32_t>(offset);
    info.mipAddress = static_cast<std::uint32_t>(offset);
    info.maxMipLevel = static_cast<std::uint32_t>(levels - 1);
    return info;
}

TextureSurface extract(const xds::Texture& texture, std::uint32_t level, TextureSurface::Format format)
{
    TextureSurface surface;
    surface.format = format;
    const xds::LevelLayout& layout = texture.levelLayout(level);
    surface.width = static_cast<int>(layout.width);
    surface.height = static_cast<int>(layout.height);
    surface.data
        = format == TextureSurface::Format::Rgba8 ? texture.decodeLevel(level).rgba : texture.extractLevel(level).data;
    return surface;
}

std::string cString(ByteView data, std::size_t offset)
{
    if (offset >= data.size()) {
        return {};
    }
    const auto* first = reinterpret_cast<const char*>(data.data() + offset);
    const std::size_t length = std::find(first, first + (data.size() - offset), '\0') - first;
    return {first, length};
}

} // namespace

std::size_t CubeTexture::byteSize() const
{
    std::size_t total = 0;
    for (const std::vector<TextureSurface>& face : faces) {
        for (const TextureSurface& level : face) {
            total += level.data.size();
        }
    }
    return total;
}

CubeTexture untileXboxCube(ByteView memory, const XboxCubeLayout& layout)
{
    if (layout.size <= 0 || layout.size > kMaxCubeSize || !std::has_single_bit(static_cast<unsigned>(layout.size))) {
        throw DataError("cube texture size {} is not a power of two", layout.size);
    }
    const int log2Size = std::countr_zero(static_cast<unsigned>(layout.size));
    if (layout.levels < 1 || layout.levels > log2Size + 1) {
        throw DataError("a {}-texel cube cannot have {} levels", layout.size, layout.levels);
    }
    const std::uint32_t format = layout.formatWord & 0x3F;
    CubeTexture cube;
    cube.size = layout.size;
    cube.format = surfaceFormat(format);

    const int packedStart = !layout.packedMips ? layout.levels : std::max(0, log2Size - kPackedTailLog2);
    try {
        std::uint64_t cursor = layout.mipOffset;
        for (int level = 0; level < std::min(packedStart, layout.levels); ++level) {
            const int size = layout.size >> level;
            const std::uint64_t stride = faceSurfaceBytes(size, format);
            const std::uint64_t start = level == 0 ? layout.baseOffset : cursor;
            for (int face = 0; face < CubeTexture::kFaces; ++face) {
                const xds::Texture texture
                    = xds::Texture::fromData(faceInfo(layout, size, 1, start + stride * face, false), memory);
                cube.faces[static_cast<std::size_t>(face)].push_back(extract(texture, 0, cube.format));
            }
            if (level > 0) {
                cursor += stride * CubeTexture::kFaces;
            }
        }
        if (packedStart < layout.levels) {
            const int tailSize = layout.size >> packedStart;
            const int tailLevels = layout.levels - packedStart;
            const std::uint64_t stride = faceSurfaceBytes(tailSize, format);
            const std::uint64_t start = packedStart == 0 ? layout.baseOffset : cursor;
            for (int face = 0; face < CubeTexture::kFaces; ++face) {
                const xds::Texture texture = xds::Texture::fromData(
                    faceInfo(layout, tailSize, tailLevels, start + stride * face, true), memory);
                for (int level = 0; level < tailLevels; ++level) {
                    cube.faces[static_cast<std::size_t>(face)].push_back(
                        extract(texture, static_cast<std::uint32_t>(level), cube.format));
                }
            }
        }
    } catch (const xds::Error& e) {
        throw DataError("cube texture: {}", e.what());
    }
    return cube;
}

CubeTexture readXprCube(ByteView file)
{
    if (file.size() < kXprPrefixBytes + 4 || std::memcmp(file.data(), "XPR2", 4) != 0) {
        throw DataError("not an XPR2 resource file");
    }
    const std::uint8_t* p = file.data();
    const std::size_t headerSize = loadBe32(p + 4);
    const std::size_t dataSize = loadBe32(p + 8);
    const std::uint32_t count = loadBe32(p + kXprPrefixBytes);
    if (kXprPrefixBytes + headerSize + dataSize > file.size()
        || kXprPrefixBytes + 4 + std::size_t{count} * kXprRecordBytes > kXprPrefixBytes + headerSize) {
        throw DataError("XPR2 header does not fit the file");
    }
    const ByteView header = file.subspan(kXprPrefixBytes, headerSize);
    const ByteView memory = file.subspan(kXprPrefixBytes + headerSize, dataSize);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* record = header.data() + 4 + std::size_t{i} * kXprRecordBytes;
        if (loadBe32(record) != kCubeResourceType) {
            continue;
        }
        const std::size_t offset = loadBe32(record + 4);
        if (offset + kD3dHeaderBytes > header.size()) {
            throw DataError("XPR2 cube header lies outside the file");
        }
        std::array<std::uint32_t, 6> fetch{};
        for (std::size_t k = 0; k < fetch.size(); ++k) {
            fetch[k] = loadBe32(header.data() + offset + kFetchConstantOffset + 4 * k);
        }
        const xds::TextureInfo info = xds::decodeFetchConstant(fetch);
        if (info.dimension != xds::Dimension::Cube || info.width != info.height || !info.tiled) {
            throw DataError("XPR2 resource of type TXCM is not a tiled square cube");
        }
        XboxCubeLayout layout;
        layout.formatWord = fetch[1] & 0xFF;
        layout.size = static_cast<int>(info.width);
        layout.levels = static_cast<int>(info.levelCount());
        layout.baseOffset = info.baseAddress;
        layout.mipOffset = info.mipAddress;
        layout.packedMips = info.packedMips;
        CubeTexture cube = untileXboxCube(memory, layout);
        cube.name = cString(header, loadBe32(record + 12));
        return cube;
    }
    throw DataError("XPR2 file holds no cube texture");
}

bool isCaffCube(ByteView data)
{
    if (data.size() < kCaffGpuSizeOffset + 4 || std::memcmp(data.data(), "CAFF", 4) != 0) {
        return false;
    }
    const std::size_t description = loadBe32(data.data() + kCaffHeaderSizeOffset);
    return description + kCaffKindOffset + 4 <= data.size()
        && loadBe32(data.data() + description + kCaffKindOffset) == kCaffCubeKind;
}

CubeTexture readCaffCube(ByteView data)
{
    if (!isCaffCube(data)) {
        throw DataError("CAFF container does not hold a cube texture");
    }
    const std::uint8_t* p = data.data();
    const std::size_t headerSize = loadBe32(p + kCaffHeaderSizeOffset);
    const std::size_t dataSize = loadBe32(p + kCaffDataSizeOffset);
    const std::size_t gpuSize = loadBe32(p + kCaffGpuSizeOffset);
    if (dataSize < kCaffLevelsOffset + 4 || headerSize + dataSize + gpuSize > data.size()) {
        throw DataError("CAFF sections do not fit the file");
    }
    const std::uint8_t* description = p + headerSize;
    const int size = loadBe16(description + kCaffSizeOffset);
    const int height = loadBe16(description + kCaffSizeOffset + 2);
    const int levels = description[kCaffLevelsOffset];
    if (size != height) {
        throw DataError("CAFF cube texture is {}x{}, not square", size, height);
    }
    if (levels != 1) {
        throw DataError("CAFF cube texture has {} levels; only single-level cubes are known", levels);
    }
    XboxCubeLayout layout;
    layout.formatWord = loadBe32(description + kCaffFormatOffset) & 0xFF;
    layout.size = size;
    layout.levels = 1;
    return untileXboxCube(data.subspan(headerSize + dataSize, gpuSize), layout);
}

CubeTexture loadTrackCube(const lzx::ZipArchive& archive, std::uint32_t id)
{
    const std::string name = TrackTextures::textureName(id) + ".bin";
    const lzx::ZipEntry* entry = archive.find(name);
    if (entry == nullptr) {
        throw DataError("{} is missing", name);
    }
    CubeTexture cube = readCaffCube(readEntry(archive, *entry));
    cube.name = entry->name;
    return cube;
}

} // namespace fh::world
