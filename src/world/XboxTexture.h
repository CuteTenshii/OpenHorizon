#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <optional>

namespace openhorizon::world {

/// One mip level as PC APIs expect it: row-major blocks, little-endian fields, no tiling (GL S3TC / RGTC).
struct TextureSurface {
    enum class Format : std::uint8_t {
        /// 8-byte blocks; colour with optional 1-bit alpha (BC1).
        Dxt1,
        /// 16-byte blocks; explicit 4-bit alpha (BC2).
        Dxt3,
        /// 16-byte blocks; interpolated alpha (BC3).
        Dxt5,
        /// 16-byte blocks; two interpolated channels, red first (BC5, the Xbox 360's DXN), for normal maps.
        Bc5,
        /// 4 bytes per texel: R, G, B, A. Also the target of 8_8_8_8, DXT3A and DXT5A.
        Rgba8,
    };

    Format format = Format::Rgba8;
    int width = 0;
    int height = 0;
    Bytes data;

    bool isCompressed() const { return format != Format::Rgba8; }
    /// Bytes per 4x4 block for compressed formats, per texel for Rgba8.
    int blockBytes() const;
    int blocksWide() const;
    int blocksHigh() const;
    /// The bytes a level of this size and format takes.
    std::size_t byteSize() const;
};

/// 8-bit RGBA pixels, row-major, no padding between rows.
struct RgbaImage {
    int width = 0;
    int height = 0;
    Bytes rgba;
};

/// Where a texture's top level sits in its tiled data.
enum class MipLayout : std::uint8_t {
    /// At the origin.
    Unpacked,
    /// In the packed mip tail when 16 texels or less on its shorter side, 16 texels along that axis of a shared
    /// 32×32-block tile (along X when square); at the origin otherwise.
    Packed,
};

/// Untiles the top mip level with xds. `formatWord` is the fetch constant's second dword (format bits 0-5, byte
/// order 6-7). DXT/DXN blocks are kept; 8_8_8_8 (read as A, R, G, B: the track containers carry no swizzle), DXT3A
/// and DXT5A are decoded to Rgba8. Throws DataError for other formats or truncated data.
TextureSurface untileXboxSurface(
    ByteView tiled, std::uint32_t formatWord, int width, int height, MipLayout layout = MipLayout::Unpacked);

/// Decodes a surface to RGBA. Throws DataError if its data is too short.
RgbaImage decodeSurface(const TextureSurface& surface);

/// Decodes the top level of an .xds file (52-byte header ending in the 24-byte fetch constant, then tiled memory)
/// with its fetch constant's swizzle. Throws DataError on failure.
RgbaImage decodeXboxTexture(ByteView data);

/// Header of `_0x<ID>.bix` ("BIX1", big-endian u32 width, height, mip count, format word, total size, top-level
/// size), followed by the smaller levels; the top level is in `_0x<ID>_B.bix`.
struct BixHeader {
    int width = 0;
    int height = 0;
    int mipCount = 0;
    /// Second dword of the GPU fetch constant (format and byte order).
    std::uint32_t formatWord = 0;
    std::uint32_t totalSize = 0;
    std::uint32_t topLevelSize = 0;
};

/// Nothing if the data does not start with a BIX1 header.
std::optional<BixHeader> readBixHeader(ByteView data);

/// The top level of a track texture from its `.bix` and `_B.bix` files. Throws DataError on failure.
TextureSurface readBixSurface(ByteView header, ByteView topLevel);

/// A small track texture stored whole in a CAFF container (`_0x<ID>.bin`): a `.data` section (format word at 0x18,
/// u16 width and height at 0x24), then the `.gpu` texels after it. Throws DataError on failure.
TextureSurface readCaffSurface(ByteView data);

namespace dxt {

/// One texel, as encoded to a block.
struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

/// Encodes 16 texels (row-major) as a DXT1 block. Texels with alpha below 128
/// become transparent, using DXT1's three-colour mode.
void encodeDxt1(const Rgba* texels, std::uint8_t* block);

/// Encodes 16 texels (row-major) as a DXT5 block.
void encodeDxt5(const Rgba* texels, std::uint8_t* block);

/// Encodes the red and green channels of 16 texels (row-major) as a BC5 block.
void encodeBc5(const Rgba* texels, std::uint8_t* block);

} // namespace dxt
} // namespace openhorizon::world
