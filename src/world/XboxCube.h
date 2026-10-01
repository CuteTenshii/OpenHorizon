#pragma once

#include "XboxTexture.h"

#include "base/Bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

/// Where an Xbox 360 cube texture's faces lie in its texture memory, from its GPU fetch constant.
struct XboxCubeLayout {
    /// Second dword of the fetch constant: format in bits 0-5, byte order in bits 6-7.
    std::uint32_t formatWord = 0;
    /// Edge of the top level in texels, a power of two.
    int size = 0;
    int levels = 1;
    /// Byte offsets of the top level and of the first smaller level.
    std::size_t baseOffset = 0;
    std::size_t mipOffset = 0;
    /// Levels of 16 texels and less share one packed surface per face (the fetch constant's packed-mips bit).
    bool packedMips = true;
};

/// A cube texture as PC APIs expect it (see TextureSurface). Faces in Direct3D order +X, -X, +Y, -Y, +Z, -Z, as
/// OpenGL's, texels as Direct3D samples them; xe_cube_direction maps the game's cube coordinates to match.
struct CubeTexture {
    static constexpr int kFaces = 6;

    /// The resource's name in its container, empty when it has none.
    std::string name;
    int size = 0;
    TextureSurface::Format format = TextureSurface::Format::Rgba8;
    /// faces[face][level]; every face has the same number of levels.
    std::array<std::vector<TextureSurface>, kFaces> faces;

    int levelCount() const { return static_cast<int>(faces[0].size()); }
    std::size_t byteSize() const;
};

/// Untiles the six faces from Xbox 360 texture memory. DXT blocks are kept, 8_8_8_8 decoded to Rgba8; throws
/// DataError for other formats, non-power-of-two sizes or short data. Layout inferred from xds's 2D tiling, verified
/// by size on every cube of the disc: per level six faces of whole 32x32-block tiles, each rounded to a 4 KiB page,
/// top level at `baseOffset`, the rest from `mipOffset` down to a packed tail at 16 texels or less.
CubeTexture untileXboxCube(ByteView memory, const XboxCubeLayout& layout);

/// Reads the first cube texture of an XPR2 file (`.xpr`; loader 0x82CEA640). Throws DataError if none.
/// Big-endian, verified on every `.xpr` of the disc: "XPR2", u32 header size, u32 data size, u32 resource count,
/// then per resource u32 type ("TXCM" for a cube), u32 offset of its 52-byte Direct3D texture header, u32 size and
/// u32 offset of its name, offsets from byte 12. Texture memory starts at 12 + header size; the header's last six
/// dwords are the fetch constant, its addresses offsets into that memory.
CubeTexture readXprCube(ByteView file);

/// Whether a track texture's CAFF container (`_0x<ID>.bin`) holds a cube: the description's word at 0x1C is 2.
/// Inferred: true exactly for the textures bound to Cube_MapSampler.
bool isCaffCube(ByteView data);

/// The description's byte at 0x30 counts the levels. Only single-level cubes occur and are read, since where smaller
/// levels would lie is unknown; throws DataError otherwise.
CubeTexture readCaffCube(ByteView data);

/// Loads `_0x<ID>.bin` from `archive`. Throws DataError if it is missing or no cube.
CubeTexture loadTrackCube(const lzx::ZipArchive& archive, std::uint32_t id);

} // namespace openhorizon::world
