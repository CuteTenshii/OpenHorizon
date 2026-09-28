#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <optional>

namespace fh::world {

/// One mip level of a texture in the layout PC graphics APIs expect: blocks
/// in row-major order with little-endian fields, no tiling. Compressed
/// formats upload as GL's S3TC and RGTC formats.
struct TextureSurface {
    enum class Format : std::uint8_t {
        /// 8-byte blocks; colour with optional 1-bit alpha (BC1).
        Dxt1,
        /// 16-byte blocks; explicit 4-bit alpha (BC2).
        Dxt3,
        /// 16-byte blocks; interpolated alpha (BC3).
        Dxt5,
        /// 16-byte blocks; two interpolated channels, red first (BC5, the
        /// Xbox 360's DXN), used for normal maps.
        Bc5,
        /// 4 bytes per texel: R, G, B, A. Also what formats without a PC
        /// counterpart here (8_8_8_8, DXT3A, DXT5A) are decoded to.
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
    /// In the packed mip tail when the texture is 16 texels or less on its
    /// shorter side; at the origin otherwise. Tiled textures that small keep
    /// their top level inside a shared 32×32-block tile, 16 texels along the
    /// shorter axis (along X for square ones).
    Packed,
};

/// Untiles the top mip level of Xbox 360 GPU texture data with the xds
/// library. `formatWord` is the second dword of the GPU fetch constant:
/// format in bits 0-5, byte order in bits 6-7. DXT1, DXT2/3, DXT4/5 and DXN
/// keep their blocks; 8_8_8_8, DXT3A and DXT5A are decoded to Rgba8. The
/// track containers carry no swizzle, so textures are read with the plain
/// one and 8_8_8_8 texels as A, R, G, B. Throws DataError for other formats
/// or truncated data.
TextureSurface untileXboxSurface(
    ByteView tiled, std::uint32_t formatWord, int width, int height, MipLayout layout = MipLayout::Unpacked);

/// Decodes a surface to RGBA. Throws DataError if its data is too short.
RgbaImage decodeSurface(const TextureSurface& surface);

/// Decodes the top mip level of an Xbox 360 texture file (.xds: a 52-byte
/// header whose last 24 bytes are the GPU fetch constant, then the tiled
/// texture memory) with the swizzle of its fetch constant applied. Throws
/// DataError on failure.
RgbaImage decodeXboxTexture(ByteView data);

/// Header of a track texture's `_0x<ID>.bix` file ("BIX1", big-endian: u32
/// width, height, mip count, fetch-constant format word, total size,
/// top-level size). The file holds the header and the smaller mip levels;
/// the top level is stored on its own in `_0x<ID>_B.bix` so it can be
/// streamed separately.
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

/// The top level of a track texture, from its `.bix` header file and its
/// `_B.bix` top-level file. Throws DataError on failure.
TextureSurface readBixSurface(ByteView header, ByteView topLevel);

/// The top level of a small track texture stored whole in a CAFF container
/// (`_0x<ID>.bin`): a `.data` section describing the texture (format word at
/// 0x18, u16 width and height at 0x24) and a `.gpu` section with the texels,
/// back to back after the header. Throws DataError on failure.
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

/// Encodes the red and green channels of 16 texels (row-major) as a BC5
/// block.
void encodeBc5(const Rgba* texels, std::uint8_t* block);

} // namespace dxt
} // namespace fh::world
