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

/// Where the faces of an Xbox 360 cube texture lie in its texture memory, as
/// its GPU fetch constant describes them.
struct XboxCubeLayout {
    /// Second dword of the fetch constant: format in bits 0-5, byte order in
    /// bits 6-7.
    std::uint32_t formatWord = 0;
    /// Edge of the top level in texels, a power of two.
    int size = 0;
    int levels = 1;
    /// Byte offsets of the top level and of the first smaller level.
    std::size_t baseOffset = 0;
    std::size_t mipOffset = 0;
    /// Whether the levels of 16 texels and less share one packed surface
    /// per face (the fetch constant's packed-mips bit).
    bool packedMips = true;
};

/// A cube texture in the layout PC graphics APIs expect: per face, every
/// level as row-major blocks with little-endian fields (see TextureSurface).
/// Faces are in the Direct3D order +X, -X, +Y, -Y, +Z, -Z, which is also
/// OpenGL's (GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), and each face's texels
/// are stored as Direct3D samples them, which is how GL samples a face
/// uploaded unchanged; the shader translator turns the game's cube
/// coordinates into GL directions on that basis (xe_cube_direction).
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

/// Untiles the six faces of a cube texture from `memory`, its Xbox 360
/// texture memory. DXT1, DXT2/3 and DXT4/5 keep their blocks; 8_8_8_8 is
/// decoded to Rgba8. Throws DataError for other formats, sizes that are not
/// powers of two, or data too short for the layout.
///
/// Layout, inferred from the Xenos tiling rules the 2D textures follow (see
/// xds) and verified by size against every cube on the disc (the six
/// `.xpr` cube maps and the three CAFF cube textures of Colorado's
/// reflective materials): each level holds the six faces one after another,
/// each face a 2D surface of whole 32x32-block tiles rounded up to a 4 KiB
/// page. The top level starts at `baseOffset`; the smaller levels follow
/// each other from `mipOffset`, down to the first level of 16 texels or
/// less, which starts a packed tail: one surface per face holding that
/// level and every smaller one at the offsets a 2D texture's packed tail
/// uses. A cube whose top level is 16 texels or less is all tail, at
/// `baseOffset`.
CubeTexture untileXboxCube(ByteView memory, const XboxCubeLayout& layout);

/// Reads the first cube texture of an XPR2 resource file (`.xpr`).
///
/// Layout, big-endian, verified on every `.xpr` of the disc: "XPR2", u32
/// header size, u32 data size, u32 resource count, then per resource u32
/// type ("TXCM" for a cube), u32 offset of its 52-byte Direct3D texture
/// header, u32 size and u32 offset of its name, the offsets counted from
/// byte 12. The texture memory follows the header, at 12 + header size, and
/// the header's last six dwords are the GPU fetch constant, whose base and
/// mip addresses are offsets into it. The executable's loader (0x82CEA640)
/// reads the same fields: the 12 bytes, the header, then relocates each
/// resource's offsets by the header's address.
///
/// Throws DataError if the file is not an XPR2 file or holds no cube.
CubeTexture readXprCube(ByteView file);

/// Whether a track texture's CAFF container (`_0x<ID>.bin`, see
/// readCaffSurface) holds a cube. Inferred from the data: the word at 0x1C
/// of its texture description is 2 in exactly the three Colorado textures
/// that reflective materials bind to their cube sampler (Cube_MapSampler),
/// each 256 texels square with six times a face's memory, and 0 in the
/// 2,962 others.
bool isCaffCube(ByteView data);

/// Reads a cube texture from a track texture's CAFF container. The byte at
/// 0x30 of the description counts the levels (1 in every cube, 1 to 9 in
/// the 2D textures, matching their sizes); only single-level cubes occur
/// and are read, as where a cube's smaller levels would lie in the
/// container is not known. Throws DataError otherwise.
CubeTexture readCaffCube(ByteView data);

/// Loads cube texture `id` of a track from its `_0x<ID>.bin` container in
/// `archive`. Throws DataError if the archive lacks it or it is no cube.
CubeTexture loadTrackCube(const lzx::ZipArchive& archive, std::uint32_t id);

} // namespace openhorizon::world
