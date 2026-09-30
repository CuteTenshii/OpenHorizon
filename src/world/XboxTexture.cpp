#include "XboxTexture.h"

#include <xds/xds.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace openhorizon::world {

namespace {

constexpr std::size_t kBixHeaderSize = 0x1C;
constexpr int kMaxDimension = 8192;
/// Tiled surfaces are whole 32x32-block tiles wide.
constexpr std::uint32_t kTileBlocks = 32;

using dxt::Rgba;

Rgba from565(std::uint16_t v)
{
    Rgba c;
    c.r = static_cast<std::uint8_t>(((v >> 11) & 31) * 255 / 31);
    c.g = static_cast<std::uint8_t>(((v >> 5) & 63) * 255 / 63);
    c.b = static_cast<std::uint8_t>((v & 31) * 255 / 31);
    return c;
}

std::uint16_t to565(float r, float g, float b)
{
    const auto quantize = [](float v, int levels) {
        return static_cast<std::uint16_t>(
            std::clamp(std::lround(v / 255.0F * static_cast<float>(levels)), 0L, static_cast<long>(levels)));
    };
    return static_cast<std::uint16_t>((quantize(r, 31) << 11) | (quantize(g, 63) << 5) | quantize(b, 31));
}

Rgba mix(const Rgba& a, const Rgba& b, int wa, int wb)
{
    const int total = wa + wb;
    Rgba c;
    c.r = static_cast<std::uint8_t>((a.r * wa + b.r * wb) / total);
    c.g = static_cast<std::uint8_t>((a.g * wa + b.g * wb) / total);
    c.b = static_cast<std::uint8_t>((a.b * wa + b.b * wb) / total);
    return c;
}

void putLe16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

/// The four colours a DXT colour block can select. DXT1 blocks with
/// c0 <= c1 use three colours and transparent black; DXT3 and DXT5 always
/// use four.
std::array<Rgba, 4> colourPalette(std::uint16_t c0, std::uint16_t c1, bool dxt1)
{
    std::array<Rgba, 4> palette{from565(c0), from565(c1), Rgba{}, Rgba{}};
    if (!dxt1 || c0 > c1) {
        palette[2] = mix(palette[0], palette[1], 2, 1);
        palette[3] = mix(palette[0], palette[1], 1, 2);
    } else {
        palette[2] = mix(palette[0], palette[1], 1, 1);
        palette[3] = Rgba{0, 0, 0, 0};
    }
    return palette;
}

/// The eight values an interpolated block (DXT5 alpha, each half of BC5)
/// selects from, rounded as decoders round them.
std::array<int, 8> channelPalette(int v0, int v1)
{
    std::array<int, 8> palette{v0, v1, 0, 0, 0, 0, 0, 0};
    if (v0 > v1) {
        for (int i = 1; i <= 6; ++i) {
            palette[static_cast<std::size_t>(i) + 1] = ((7 - i) * v0 + i * v1 + 3) / 7;
        }
    } else {
        for (int i = 1; i <= 4; ++i) {
            palette[static_cast<std::size_t>(i) + 1] = ((5 - i) * v0 + i * v1 + 2) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

int colourDistance(const Rgba& a, const Rgba& b)
{
    const int dr = a.r - b.r;
    const int dg = a.g - b.g;
    const int db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

/// Two endpoint colours spanning the block's texels along their principal
/// axis, which suits the gradients DXT blocks store far better than the
/// bounding box corners do.
std::pair<std::array<float, 3>, std::array<float, 3>> colourEndpoints(const Rgba* texels, const bool* use)
{
    std::array<float, 3> mean{};
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if (use[i]) {
            mean[0] += texels[i].r;
            mean[1] += texels[i].g;
            mean[2] += texels[i].b;
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    for (float& m : mean) {
        m /= static_cast<float>(count);
    }
    std::array<std::array<float, 3>, 3> covariance{};
    for (int i = 0; i < 16; ++i) {
        if (!use[i]) {
            continue;
        }
        const std::array<float, 3> d{texels[i].r - mean[0], texels[i].g - mean[1], texels[i].b - mean[2]};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                covariance[r][c] += d[r] * d[c];
            }
        }
    }
    // Power iteration toward the principal axis. It starts from the row of
    // the channel that varies most: a fixed start such as (1, 1, 1) can be
    // orthogonal to the true axis (red rising while green falls) and would
    // collapse every texel onto the mean.
    std::size_t widest = 0;
    for (std::size_t c = 1; c < 3; ++c) {
        if (covariance[c][c] > covariance[widest][widest]) {
            widest = c;
        }
    }
    std::array<float, 3> axis = covariance[widest];
    const float startLength = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (startLength < 1e-6F) {
        return {mean, mean};
    }
    for (float& a : axis) {
        a /= startLength;
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<float, 3> next{};
        for (std::size_t r = 0; r < 3; ++r) {
            next[r] = covariance[r][0] * axis[0] + covariance[r][1] * axis[1] + covariance[r][2] * axis[2];
        }
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6F) {
            break;
        }
        axis = {next[0] / length, next[1] / length, next[2] / length};
    }
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (int i = 0; i < 16; ++i) {
        if (!use[i]) {
            continue;
        }
        const float t
            = (texels[i].r - mean[0]) * axis[0] + (texels[i].g - mean[1]) * axis[1] + (texels[i].b - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    for (std::size_t c = 0; c < 3; ++c) {
        a[c] = std::clamp(mean[c] + axis[c] * hi, 0.0F, 255.0F);
        b[c] = std::clamp(mean[c] + axis[c] * lo, 0.0F, 255.0F);
    }
    return {a, b};
}

/// Writes the 8-byte colour half of a block. In DXT1, texels with alpha below
/// 128 select the three-colour mode's transparent black.
void encodeColour(const Rgba* texels, bool dxt1, std::uint8_t* block)
{
    std::array<bool, 16> transparent{};
    std::array<bool, 16> opaque{};
    bool anyTransparent = false;
    for (std::size_t i = 0; i < 16; ++i) {
        transparent[i] = dxt1 && texels[i].a < 128;
        opaque[i] = !transparent[i];
        anyTransparent = anyTransparent || transparent[i];
    }
    const auto [a, b] = colourEndpoints(texels, opaque.data());
    std::uint16_t c0 = to565(a[0], a[1], a[2]);
    std::uint16_t c1 = to565(b[0], b[1], b[2]);
    if (anyTransparent) {
        if (c0 > c1) {
            std::swap(c0, c1);
        }
    } else if (c0 < c1) {
        std::swap(c0, c1);
    }
    const auto palette = colourPalette(c0, c1, dxt1);
    // With equal endpoints a DXT1 block is in three-colour mode; only the
    // first three entries are colours then.
    const int colours = (dxt1 && c0 <= c1) ? 3 : 4;
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint32_t index = 3;
        if (!transparent[i]) {
            int best = std::numeric_limits<int>::max();
            for (int p = 0; p < colours; ++p) {
                const int d = colourDistance(texels[i], palette[static_cast<std::size_t>(p)]);
                if (d < best) {
                    best = d;
                    index = static_cast<std::uint32_t>(p);
                }
            }
        }
        bits |= index << (2 * i);
    }
    putLe16(block, c0);
    putLe16(block + 2, c1);
    for (int i = 0; i < 4; ++i) {
        block[4 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

/// Writes an 8-byte interpolated block of one channel.
void encodeChannel(const std::array<std::uint8_t, 16>& values, std::uint8_t* block)
{
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    block[0] = *hi;
    block[1] = *lo;
    const auto palette = channelPalette(*hi, *lo);
    std::uint64_t bits = 0;
    if (*hi != *lo) {
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint64_t index = 0;
            int best = std::numeric_limits<int>::max();
            for (std::size_t p = 0; p < 8; ++p) {
                const int d = std::abs(values[i] - palette[p]);
                if (d < best) {
                    best = d;
                    index = p;
                }
            }
            bits |= index << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) {
        block[2 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

bool isDxt1(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt1)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt1_As16_16_16_16);
}

bool isDxt3(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt2_3)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt2_3_As16_16_16_16);
}

bool isDxt5(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt4_5)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt4_5_As16_16_16_16);
}

bool isDxn(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxn);
}

bool is8888(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::k8_8_8_8)
        || format == static_cast<std::uint32_t>(xds::Format::k8_8_8_8_As16_16_16_16);
}

RgbaImage imageOf(xds::Image&& decoded)
{
    return {static_cast<int>(decoded.width), static_cast<int>(decoded.height), std::move(decoded.rgba)};
}

/// The top level of `texture`: block formats as they are, anything else
/// (8_8_8_8, the single-channel formats) decoded to RGBA with the texture's
/// swizzle.
TextureSurface topLevelSurface(const xds::Texture& texture)
{
    const std::uint32_t format = texture.info().format;
    TextureSurface surface;
    surface.width = static_cast<int>(texture.info().width);
    surface.height = static_cast<int>(texture.info().height);
    if (isDxt1(format) || isDxt3(format) || isDxt5(format) || isDxn(format)) {
        surface.format = isDxt1(format) ? TextureSurface::Format::Dxt1
            : isDxt3(format)            ? TextureSurface::Format::Dxt3
            : isDxt5(format)            ? TextureSurface::Format::Dxt5
                                        : TextureSurface::Format::Bc5;
        surface.data = texture.extractLevel(0).data;
        return surface;
    }
    surface.format = TextureSurface::Format::Rgba8;
    surface.data = texture.decodeLevel(0).rgba;
    return surface;
}

} // namespace

int TextureSurface::blockBytes() const
{
    switch (format) {
    case Format::Dxt1:
        return 8;
    case Format::Dxt3:
    case Format::Dxt5:
    case Format::Bc5:
        return 16;
    case Format::Rgba8:
        break;
    }
    return 4;
}

int TextureSurface::blocksWide() const
{
    return isCompressed() ? (width + 3) / 4 : width;
}

int TextureSurface::blocksHigh() const
{
    return isCompressed() ? (height + 3) / 4 : height;
}

std::size_t TextureSurface::byteSize() const
{
    return static_cast<std::size_t>(std::max(blocksWide(), 0)) * static_cast<std::size_t>(std::max(blocksHigh(), 0))
        * static_cast<std::size_t>(blockBytes());
}

namespace dxt {

void encodeDxt1(const Rgba* texels, std::uint8_t* block)
{
    encodeColour(texels, true, block);
}

void encodeDxt5(const Rgba* texels, std::uint8_t* block)
{
    std::array<std::uint8_t, 16> alpha{};
    for (std::size_t i = 0; i < 16; ++i) {
        alpha[i] = texels[i].a;
    }
    encodeChannel(alpha, block);
    encodeColour(texels, false, block + 8);
}

void encodeBc5(const Rgba* texels, std::uint8_t* block)
{
    std::array<std::uint8_t, 16> red{};
    std::array<std::uint8_t, 16> green{};
    for (std::size_t i = 0; i < 16; ++i) {
        red[i] = texels[i].r;
        green[i] = texels[i].g;
    }
    encodeChannel(red, block);
    encodeChannel(green, block + 8);
}

} // namespace dxt

TextureSurface untileXboxSurface(ByteView tiled, std::uint32_t formatWord, int width, int height, MipLayout layout)
{
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        throw DataError("invalid texture size {}x{}", width, height);
    }
    xds::TextureInfo info;
    info.format = formatWord & 0x3F;
    if (!xds::isFormatSupported(info.format)) {
        throw DataError("unsupported texture format {} ({})", info.format, xds::formatName(info.format));
    }
    info.endian = static_cast<xds::Endian>((formatWord >> 6) & 3);
    info.tiled = true;
    info.packedMips = layout == MipLayout::Packed;
    info.width = static_cast<std::uint32_t>(width);
    info.height = static_cast<std::uint32_t>(height);
    const std::uint32_t edge = xds::blockEdge(info.format);
    const std::uint32_t blocksWide = (info.width + edge - 1) / edge;
    info.pitch = (blocksWide + kTileBlocks - 1) / kTileBlocks * kTileBlocks * edge;
    if (is8888(info.format)) {
        // The containers carry no swizzle; their 8_8_8_8 texels are A, R, G,
        // B in memory once swapped.
        info.swizzle = {xds::Swizzle::Y, xds::Swizzle::Z, xds::Swizzle::W, xds::Swizzle::X};
    }
    try {
        return topLevelSurface(xds::Texture::fromData(info, tiled));
    } catch (const xds::Error& e) {
        throw DataError("{}", e.what());
    }
}

RgbaImage decodeSurface(const TextureSurface& surface)
{
    if (surface.width <= 0 || surface.height <= 0 || surface.data.size() < surface.byteSize()) {
        throw DataError("the {}x{} texture level is truncated", surface.width, surface.height);
    }
    if (!surface.isCompressed()) {
        const auto end = surface.data.begin() + static_cast<std::ptrdiff_t>(surface.byteSize());
        return {surface.width, surface.height, Bytes(surface.data.begin(), end)};
    }
    // The blocks as a linear texture in PC byte order, for xds to decode.
    xds::TextureInfo info;
    switch (surface.format) {
    case TextureSurface::Format::Dxt1:
        info.format = static_cast<std::uint32_t>(xds::Format::kDxt1);
        break;
    case TextureSurface::Format::Dxt3:
        info.format = static_cast<std::uint32_t>(xds::Format::kDxt2_3);
        break;
    case TextureSurface::Format::Dxt5:
        info.format = static_cast<std::uint32_t>(xds::Format::kDxt4_5);
        break;
    case TextureSurface::Format::Bc5:
        info.format = static_cast<std::uint32_t>(xds::Format::kDxn);
        break;
    case TextureSurface::Format::Rgba8:
        throw DataError("RGBA8 is not a block format");
    }
    info.endian = xds::Endian::None;
    info.tiled = false;
    info.width = static_cast<std::uint32_t>(surface.width);
    info.height = static_cast<std::uint32_t>(surface.height);
    info.pitch = static_cast<std::uint32_t>(surface.blocksWide()) * 4;
    try {
        return imageOf(xds::Texture::fromData(info, surface.data).decodeLevel(0));
    } catch (const xds::Error& e) {
        throw DataError("{}", e.what());
    }
}

RgbaImage decodeXboxTexture(ByteView data)
{
    try {
        return imageOf(xds::Texture::parse(data).decodeLevel(0));
    } catch (const xds::Error& e) {
        throw DataError("{}", e.what());
    }
}

std::optional<BixHeader> readBixHeader(ByteView data)
{
    if (data.size() < kBixHeaderSize || std::memcmp(data.data(), "BIX1", 4) != 0) {
        return std::nullopt;
    }
    const std::uint8_t* p = data.data();
    BixHeader header;
    header.width = static_cast<int>(loadBe32(p + 4));
    header.height = static_cast<int>(loadBe32(p + 8));
    header.mipCount = static_cast<int>(loadBe32(p + 12));
    header.formatWord = loadBe32(p + 16);
    header.totalSize = loadBe32(p + 20);
    header.topLevelSize = loadBe32(p + 24);
    return header;
}

TextureSurface readBixSurface(ByteView header, ByteView topLevel)
{
    const std::optional<BixHeader> bix = readBixHeader(header);
    if (!bix) {
        throw DataError("not a BIX1 texture header");
    }
    if (topLevel.size() < bix->topLevelSize) {
        throw DataError("texture top level is truncated");
    }
    return untileXboxSurface(topLevel, bix->formatWord, bix->width, bix->height, MipLayout::Packed);
}

TextureSurface readCaffSurface(ByteView data)
{
    constexpr std::size_t kHeaderSizeOffset = 0x48;
    constexpr std::size_t kDataSizeOffset = 0x60;
    constexpr std::size_t kGpuSizeOffset = 0x9C;
    constexpr std::size_t kFormatOffset = 0x18;
    constexpr std::size_t kSizeOffset = 0x24;
    if (data.size() < kGpuSizeOffset + 4 || std::memcmp(data.data(), "CAFF", 4) != 0) {
        throw DataError("not a CAFF container");
    }
    const std::uint8_t* p = data.data();
    const std::size_t headerSize = loadBe32(p + kHeaderSizeOffset);
    const std::size_t dataSize = loadBe32(p + kDataSizeOffset);
    const std::size_t gpuSize = loadBe32(p + kGpuSizeOffset);
    if (dataSize < kSizeOffset + 4 || gpuSize == 0 || headerSize + dataSize + gpuSize > data.size()) {
        throw DataError("CAFF sections do not fit the file");
    }
    const std::uint8_t* description = p + headerSize;
    if (std::memcmp(description, "texture", 8) != 0) {
        throw DataError("CAFF container does not hold a texture");
    }
    const std::uint32_t formatWord = loadBe32(description + kFormatOffset);
    const int width = loadBe16(description + kSizeOffset);
    const int height = loadBe16(description + kSizeOffset + 2);
    return untileXboxSurface(
        data.subspan(headerSize + dataSize, gpuSize), formatWord, width, height, MipLayout::Packed);
}

} // namespace openhorizon::world
