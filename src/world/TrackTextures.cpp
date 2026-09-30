#include "TrackTextures.h"

#include "Archive.h"
#include "PvsFile.h"

#include <lzx/zip.h>

#include <array>
#include <cstring>
#include <list>
#include <mutex>

namespace openhorizon::world {

namespace {

constexpr std::uint32_t kLayoutMarker = 0x290;
constexpr std::size_t kMarkerSearch = 256;
constexpr int kPositionBytes = 12;
constexpr int kInputBytes = 4;
constexpr std::uint32_t kFirstOutputRegister = 0x30;
constexpr std::uint32_t kUsageTexcoord = 5;
constexpr std::uint32_t kUsageColour = 10;
constexpr std::uint32_t kConstantTableHeaderBytes = 28;
constexpr std::uint32_t kPixelShader30 = 0xFFFF0300;
constexpr std::size_t kConstantRecordBytes = 20;
constexpr std::uint16_t kSamplerRegisterSet = 3;
constexpr std::uint32_t kMaxConstants = 1024;
constexpr std::uint16_t kMaxSamplerRegister = 16;
constexpr std::size_t kBundleHeaderBytes = 28;
constexpr std::uint32_t kBundleSeparator = 0xFFFFFFFF;
constexpr std::uint32_t kMaxBundleDimension = 4096;
/// Bundle packs kept decompressed; loading a view's missing textures hits
/// the same few packs many times.
constexpr std::size_t kCachedPacks = 8;

using dxt::Rgba;
// Texels are copied to and from RGBA8 bytes as they are.
static_assert(sizeof(Rgba) == 4);

std::vector<Rgba> decodeTexels(const TextureSurface& surface)
{
    const RgbaImage image = decodeSurface(surface);
    std::vector<Rgba> texels(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
    std::memcpy(texels.data(), image.rgba.data(), std::min(image.rgba.size(), texels.size() * sizeof(Rgba)));
    return texels;
}

/// Halves an image with a box filter. Colours are weighted by alpha so that
/// transparent texels (black in DXT1) do not darken the edges of cut-outs.
std::vector<Rgba> downsample(const std::vector<Rgba>& texels, int width, int height, int newWidth, int newHeight)
{
    std::vector<Rgba> out(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight));
    for (int y = 0; y < newHeight; ++y) {
        for (int x = 0; x < newWidth; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;
            int a = 0;
            int samples = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, width - 1);
                    const int sy = std::min(y * 2 + dy, height - 1);
                    const Rgba& t = texels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width)
                        + static_cast<std::size_t>(sx)];
                    r += t.r * t.a;
                    g += t.g * t.a;
                    b += t.b * t.a;
                    a += t.a;
                    ++samples;
                }
            }
            Rgba& o
                = out[static_cast<std::size_t>(y) * static_cast<std::size_t>(newWidth) + static_cast<std::size_t>(x)];
            if (a > 0) {
                o.r = static_cast<std::uint8_t>(r / a);
                o.g = static_cast<std::uint8_t>(g / a);
                o.b = static_cast<std::uint8_t>(b / a);
            }
            o.a = static_cast<std::uint8_t>(a / samples);
        }
    }
    return out;
}

Bytes encodeLevel(const std::vector<Rgba>& texels, int width, int height, TextureSurface::Format format)
{
    if (format == TextureSurface::Format::Rgba8) {
        Bytes out(texels.size() * sizeof(Rgba));
        std::memcpy(out.data(), texels.data(), out.size());
        return out;
    }
    const TextureSurface shape{format, width, height, {}};
    const auto blocksWide = static_cast<std::size_t>(shape.blocksWide());
    const auto blocksHigh = static_cast<std::size_t>(shape.blocksHigh());
    const auto blockBytes = static_cast<std::size_t>(shape.blockBytes());
    Bytes out(blocksWide * blocksHigh * blockBytes);
    std::array<Rgba, 16> block{};
    for (std::size_t by = 0; by < blocksHigh; ++by) {
        for (std::size_t bx = 0; bx < blocksWide; ++bx) {
            for (std::size_t i = 0; i < 16; ++i) {
                // Levels smaller than a block repeat their edge texels.
                const std::size_t x = std::min(bx * 4 + i % 4, static_cast<std::size_t>(width) - 1);
                const std::size_t y = std::min(by * 4 + i / 4, static_cast<std::size_t>(height) - 1);
                block[i] = texels[y * static_cast<std::size_t>(width) + x];
            }
            std::uint8_t* target = out.data() + (by * blocksWide + bx) * blockBytes;
            switch (format) {
            case TextureSurface::Format::Dxt1:
                dxt::encodeDxt1(block.data(), target);
                break;
            case TextureSurface::Format::Bc5:
                dxt::encodeBc5(block.data(), target);
                break;
            case TextureSurface::Format::Dxt3:
            case TextureSurface::Format::Dxt5:
            case TextureSurface::Format::Rgba8:
                dxt::encodeDxt5(block.data(), target);
                break;
            }
        }
    }
    return out;
}

/// The NUL-terminated string at `offset`, or an empty one outside `data`.
std::string_view stringAt(ByteView data, std::size_t offset)
{
    if (offset >= data.size()) {
        return {};
    }
    const auto* first = reinterpret_cast<const char*>(data.data() + offset);
    const auto* end = reinterpret_cast<const char*>(data.data() + data.size());
    return std::string_view(first, static_cast<std::size_t>(std::find(first, end, '\0') - first));
}

/// Where `pattern` first occurs in `data` at or after `from`, or npos.
std::size_t find(ByteView data, std::string_view pattern, std::size_t from = 0)
{
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    return text.find(pattern, from);
}

/// Sampler names by register, from the first pixel shader 3.0 constant
/// table (see readShaderLayout()); empty if there is none.
std::vector<std::string> readPixelSamplers(ByteView fxobj)
{
    for (std::size_t header = 0; header + kConstantTableHeaderBytes <= fxobj.size(); header += 4) {
        const std::uint8_t* p = fxobj.data() + header;
        if (loadBe32(p) != kConstantTableHeaderBytes || loadBe32(p + 8) != kPixelShader30) {
            continue;
        }
        const std::uint32_t count = loadBe32(p + 12);
        const std::uint32_t records = loadBe32(p + 16);
        const std::uint32_t targetOffset = loadBe32(p + 24);
        if (count > kMaxConstants || stringAt(fxobj, header + targetOffset) != "ps_3_0"
            || header + records + static_cast<std::size_t>(count) * kConstantRecordBytes > fxobj.size()) {
            continue;
        }
        std::vector<std::string> samplers;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint8_t* record
                = fxobj.data() + header + records + static_cast<std::size_t>(i) * kConstantRecordBytes;
            const std::uint16_t set = loadBe16(record + 4);
            const std::uint16_t reg = loadBe16(record + 6);
            if (set != kSamplerRegisterSet || reg >= kMaxSamplerRegister) {
                continue;
            }
            if (samplers.size() <= reg) {
                samplers.resize(static_cast<std::size_t>(reg) + 1);
            }
            samplers[reg] = std::string(stringAt(fxobj, header + loadBe32(record)));
        }
        return samplers;
    }
    return {};
}

} // namespace

TextureSurface TextureMipChain::level(int level) const
{
    TextureSurface surface;
    surface.format = format;
    surface.width = levelWidth(level);
    surface.height = levelHeight(level);
    if (level >= 0 && static_cast<std::size_t>(level) < levels.size()) {
        surface.data = levels[static_cast<std::size_t>(level)];
    }
    return surface;
}

std::size_t TextureMipChain::byteSize() const
{
    std::size_t total = 0;
    for (const Bytes& level : levels) {
        total += level.size();
    }
    return total;
}

TextureMipChain buildMipChain(const TextureSurface& top)
{
    TextureMipChain chain;
    chain.width = top.width;
    chain.height = top.height;
    chain.format = top.format == TextureSurface::Format::Dxt3 ? TextureSurface::Format::Dxt5 : top.format;
    std::vector<Rgba> texels = decodeTexels(top);
    chain.levels.push_back(
        top.format == chain.format ? top.data : encodeLevel(texels, top.width, top.height, chain.format));
    int width = top.width;
    int height = top.height;
    while (width > 1 || height > 1) {
        const int newWidth = std::max(1, width / 2);
        const int newHeight = std::max(1, height / 2);
        texels = downsample(texels, width, height, newWidth, newHeight);
        width = newWidth;
        height = newHeight;
        chain.levels.push_back(encodeLevel(texels, width, height, chain.format));
    }
    return chain;
}

std::optional<ShaderLayout> readShaderLayout(ByteView fxobj)
{
    static constexpr std::string_view kVersion("vs_3_0", 7);
    static constexpr std::string_view kMarker("\x00\x00\x02\x90", 4);
    const std::size_t version = find(fxobj, kVersion);
    if (version == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t table = find(fxobj, kMarker, version);
    if (table == std::string_view::npos || table - version > kMarkerSearch || table + 8 > fxobj.size()
        || loadBe32(fxobj.data() + table) != kLayoutMarker) {
        return std::nullopt;
    }
    const std::uint32_t count = loadBe32(fxobj.data() + table + 4) & 0xFFFF;
    ShaderLayout layout;
    int inputs = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t at = table + 8 + static_cast<std::size_t>(i) * 4;
        if (at + 4 > fxobj.size()) {
            return std::nullopt;
        }
        const std::uint32_t entry = loadBe32(fxobj.data() + at);
        if ((entry & 0xFF) >= kFirstOutputRegister) {
            break;
        }
        const std::uint32_t usage = (entry >> 12) & 0xF;
        const std::uint32_t usageIndex = (entry >> 16) & 0xF;
        const int offset = kPositionBytes + inputs * kInputBytes;
        if (usage == kUsageTexcoord && usageIndex == 0 && layout.texcoord0Offset < 0) {
            layout.texcoord0Offset = offset;
        } else if (usage == kUsageTexcoord && usageIndex == 1 && layout.texcoord1Offset < 0) {
            layout.texcoord1Offset = offset;
        } else if (usage == kUsageTexcoord && usageIndex == 2 && layout.texcoord2Offset < 0) {
            layout.texcoord2Offset = offset;
        } else if (usage == kUsageColour && usageIndex == 0 && layout.colourOffset < 0) {
            layout.colourOffset = offset;
        }
        ++inputs;
    }
    layout.vertexBytes = kPositionBytes + inputs * kInputBytes;
    layout.samplers = readPixelSamplers(fxobj);
    return layout;
}

int ShaderLayout::samplerRegister(std::string_view name) const
{
    for (std::size_t r = 0; r < samplers.size(); ++r) {
        if (samplers[r] == name) {
            return static_cast<int>(r);
        }
    }
    return -1;
}

/// The most recently read bundle packs, shared by the loading threads.
class TrackTextures::PackCache {
public:
    std::shared_ptr<const Bytes> get(const lzx::ZipArchive& archive, std::size_t entry)
    {
        {
            const std::lock_guard lock(m_mutex);
            for (auto it = m_packs.begin(); it != m_packs.end(); ++it) {
                if (it->first == entry) {
                    m_packs.splice(m_packs.begin(), m_packs, it);
                    return m_packs.front().second;
                }
            }
        }
        // Decompressed outside the lock so other threads are not held up; two
        // threads may occasionally read the same pack.
        auto data = std::make_shared<const Bytes>(readEntry(archive, archive.entries()[entry]));
        const std::lock_guard lock(m_mutex);
        m_packs.emplace_front(entry, data);
        if (m_packs.size() > kCachedPacks) {
            m_packs.pop_back();
        }
        return data;
    }

private:
    std::mutex m_mutex;
    std::list<std::pair<std::size_t, std::shared_ptr<const Bytes>>> m_packs;
};

TrackTextures::TrackTextures()
    : m_packs(std::make_unique<PackCache>())
{
}

TrackTextures::~TrackTextures() = default;
TrackTextures::TrackTextures(TrackTextures&& other) noexcept = default;
TrackTextures& TrackTextures::operator=(TrackTextures&& other) noexcept = default;

std::vector<TrackTextures::BundleEntry> TrackTextures::readBundle(ByteView pack)
{
    BeReader r(pack, "bundle pack");
    const std::uint32_t count = r.u32();
    if (count > pack.size() / kBundleHeaderBytes) {
        throw DataError("not a bundle pack");
    }
    std::vector<BundleEntry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BundleEntry entry;
        entry.record = r.u32();
        const std::uint32_t width = r.u32();
        const std::uint32_t height = r.u32();
        r.u32(); // mip count; the packed levels are rebuilt from the top one
        entry.formatWord = r.u32();
        const std::uint32_t separator = r.u32();
        entry.size = r.u32();
        entry.offset = r.pos();
        r.skip(entry.size);
        if (separator != kBundleSeparator || width == 0 || height == 0 || width > kMaxBundleDimension
            || height > kMaxBundleDimension) {
            throw DataError("bundle entry {} is malformed", i);
        }
        entry.width = static_cast<int>(width);
        entry.height = static_cast<int>(height);
        entries.push_back(entry);
    }
    return entries;
}

TrackTextures TrackTextures::load(ByteView pvs, const lzx::ZipArchive& archive)
{
    PvsTables tables = readPvs(pvs);
    TrackTextures textures;
    textures.m_objects = std::move(tables.objectTextures);
    for (std::size_t record = 0; record < tables.textureIds.size(); ++record) {
        textures.m_records.insert_or_assign(tables.textureIds[record], static_cast<std::uint32_t>(record));
    }
    const auto& entries = archive.entries();

    const std::vector<std::size_t> shaders = distinctEntries(archive, ".fxobj");
    std::vector<std::optional<ShaderLayout>> layouts(shaders.size());
    parallelFor(shaders.size(), [&](std::size_t s) {
        try {
            layouts[s] = readShaderLayout(readEntry(archive, entries[shaders[s]]));
        } catch (const DataError&) {
            layouts[s].reset();
        }
    });
    for (std::size_t s = 0; s < shaders.size(); ++s) {
        if (layouts[s]) {
            textures.m_shaders.try_emplace(shaderKey(entries[shaders[s]].name), std::move(*layouts[s]));
        }
    }

    const std::vector<std::size_t> packs = distinctEntries(archive, ".bundle");
    std::vector<std::vector<BundleEntry>> packEntries(packs.size());
    parallelFor(packs.size(), [&](std::size_t p) {
        try {
            packEntries[p] = readBundle(readEntry(archive, entries[packs[p]]));
        } catch (const DataError&) {
            packEntries[p].clear();
        }
    });
    for (std::size_t p = 0; p < packs.size(); ++p) {
        for (const BundleEntry& entry : packEntries[p]) {
            textures.m_bundled.try_emplace(entry.record, BundleLocation{packs[p], entry});
        }
    }
    return textures;
}

const std::vector<std::uint32_t>* TrackTextures::objectTextures(std::uint32_t object) const
{
    return object < m_objects.size() ? &m_objects[object] : nullptr;
}

const ShaderLayout* TrackTextures::shader(std::string_view path) const
{
    const auto it = m_shaders.find(shaderKey(path));
    return it == m_shaders.end() ? nullptr : &it->second;
}

std::string TrackTextures::shaderKey(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    name = name.substr(0, name.find('.'));
    return toLower(name);
}

std::optional<std::uint32_t> TrackTextures::objectNumber(std::string_view entryName)
{
    return numberBefore(entryName, '.', ".rmb.bin");
}

std::string TrackTextures::textureName(std::uint32_t id)
{
    return std::format("_0x{:08X}", id);
}

TextureMipChain TrackTextures::loadTexture(const lzx::ZipArchive& archive, std::uint32_t id) const
{
    const std::string stem = textureName(id);
    TextureSurface surface;
    TextureMipChain::Origin origin = TextureMipChain::Origin::Bix;
    std::string files;
    if (const lzx::ZipEntry* header = archive.find(stem + ".bix")) {
        const lzx::ZipEntry* top = archive.find(stem + "_B.bix");
        if (top == nullptr) {
            throw DataError("{}_B.bix is missing", stem);
        }
        surface = readBixSurface(readEntry(archive, *header), readEntry(archive, *top));
        files = std::format("{}, {}", header->name, top->name);
    } else if (const lzx::ZipEntry* container = archive.find(stem + ".bin")) {
        surface = readCaffSurface(readEntry(archive, *container));
        origin = TextureMipChain::Origin::Caff;
        files = container->name;
    } else {
        return loadBundled(archive, id);
    }
    TextureMipChain chain = buildMipChain(surface);
    chain.origin = origin;
    chain.files = std::move(files);
    return chain;
}

TextureMipChain TrackTextures::loadBundled(const lzx::ZipArchive& archive, std::uint32_t id) const
{
    const std::string name = textureName(id);
    const auto record = m_records.find(id);
    const auto location = record == m_records.end() ? m_bundled.end() : m_bundled.find(record->second);
    if (location == m_bundled.end() || location->second.archiveEntry >= archive.entries().size()) {
        throw DataError("texture {} is not in the archive", name);
    }
    const std::shared_ptr<const Bytes> pack = m_packs->get(archive, location->second.archiveEntry);
    const BundleEntry& entry = location->second.entry;
    if (entry.offset + entry.size > pack->size()) {
        throw DataError("the bundle copy of texture {} lies outside its pack", name);
    }
    const TextureSurface surface = untileXboxSurface(ByteView(*pack).subspan(entry.offset, entry.size),
        entry.formatWord, entry.width, entry.height, MipLayout::Packed);
    TextureMipChain chain = buildMipChain(surface);
    chain.origin = TextureMipChain::Origin::Bundle;
    chain.files = std::format("{} (record {})", archive.entries()[location->second.archiveEntry].name, entry.record);
    return chain;
}

} // namespace openhorizon::world
