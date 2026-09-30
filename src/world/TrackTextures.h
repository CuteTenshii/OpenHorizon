#pragma once

#include "XboxTexture.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

/// A texture ready for upload: every mip level from the full size down to
/// 1x1, all in one format. Level 0 keeps the game's own compressed blocks;
/// smaller levels are filtered from it and compressed again.
struct TextureMipChain {
    /// Where a texture was read from.
    enum class Origin : std::uint8_t {
        /// `_0x<ID>.bix` and `_0x<ID>_B.bix`.
        Bix,
        /// `_0x<ID>.bin`.
        Caff,
        /// A small copy in a `.bundle` pack.
        Bundle,
    };

    TextureSurface::Format format = TextureSurface::Format::Rgba8;
    int width = 0;
    int height = 0;
    std::vector<Bytes> levels;
    Origin origin = Origin::Bix;
    /// The archive files the texture was read from, for display.
    std::string files;

    int levelWidth(int level) const { return std::max(1, width >> level); }
    int levelHeight(int level) const { return std::max(1, height >> level); }
    /// The level as a surface, e.g. to decode it where the GPU cannot.
    TextureSurface level(int level) const;
    std::size_t byteSize() const;
};

/// Builds the mip chain of `top`. DXT3 textures are converted to DXT5, the
/// only other alpha format the chain uses. Throws DataError if `top` is
/// truncated.
TextureMipChain buildMipChain(const TextureSurface& top);

/// What the renderer needs to know about one of the track's shaders.
struct ShaderLayout {
    /// Byte offset of the first texture coordinate pair in a vertex, or -1
    /// when the shader reads none.
    int texcoord0Offset = -1;
    /// Byte offset of the second texture coordinate pair, or -1.
    int texcoord1Offset = -1;
    /// Byte offset of the third texture coordinate pair, or -1.
    int texcoord2Offset = -1;
    /// Byte offset of the vertex colour, or -1.
    int colourOffset = -1;
    /// Vertex size implied by the shader's inputs.
    int vertexBytes = 0;
    /// Name of the pixel shader's sampler at each register ("Blend_ASampler",
    /// "Splat_Sampler"), empty where there is none. A material's texture
    /// slot k feeds register k.
    std::vector<std::string> samplers;

    /// The register of sampler `name`, or -1.
    int samplerRegister(std::string_view name) const;
};

/// Reads the vertex inputs of a compiled track shader (`.fxobj`). Nothing if
/// the data has no vertex input table.
///
/// After the first vertex shader's version string ("vs_3_0", compiler
/// version) the file holds a table: u32 0x290, u32 whose low 16 bits count
/// the entries, then one u32 per input and output. Inputs carry the D3D
/// declaration usage in bits 12-15, the usage index in bits 16-19 and the
/// register in bits 0-7; outputs use registers from 0x30 up. Every input is
/// four bytes in the vertex, after the 12-byte position, in table order.
/// That layout is inferred from matching the tables against vertex strides.
///
/// The sampler registers come from the first pixel shader's constant table,
/// which has the layout of Direct3D's (big-endian, without its "CTAB" tag):
/// a 28-byte header (u32 28, creator offset, version 0xFFFF0300, constant
/// count, record offset, flags, target offset pointing at "ps_3_0"), then
/// 20-byte records (name offset, u16 register set, u16 register, u16
/// count, u16, type offset, default offset), offsets from the header;
/// register set 3 holds samplers.
std::optional<ShaderLayout> readShaderLayout(ByteView fxobj);

/// Which of the track's textures each render model uses, and how to read
/// them.
///
/// The track's PVS file (see PvsTables) lists the textures of each render
/// object; render object i is the file `<track>.<i>.rmb.bin`. A material's
/// texture slots index the object's texture list, and slot 0 holds the
/// diffuse texture in every shader the track uses (its sampler register 0).
///
/// A texture id names either `_0x<ID>.bix` (header and small mips) with
/// `_0x<ID>_B.bix` (top level), or `_0x<ID>.bin` (a CAFF container).
///
/// The `_0x1000xxxx.bundle` packs hold small copies (4x4 to 16x16 texels) of
/// nearly every texture, keyed by texture record number. For about a third
/// of the textures the models use, those copies are all there is; they are
/// mostly flat colours. A pack is u32 count, then per texture: u32 record
/// number, u32 width, u32 height, u32 mip count, u32 format word (as in the
/// GPU fetch constant), u32 0xFFFFFFFF, u32 size, and `size` bytes of tiled
/// data with packed mips. The same texture can appear in several packs.
class TrackTextures {
public:
    /// One texture in a bundle pack.
    struct BundleEntry {
        std::uint32_t record = 0;
        int width = 0;
        int height = 0;
        std::uint32_t formatWord = 0;
        /// Where the tiled data starts in the pack, and its size.
        std::size_t offset = 0;
        std::size_t size = 0;
    };

    TrackTextures();
    ~TrackTextures();
    TrackTextures(TrackTextures&& other) noexcept;
    TrackTextures& operator=(TrackTextures&& other) noexcept;
    TrackTextures(const TrackTextures&) = delete;
    TrackTextures& operator=(const TrackTextures&) = delete;

    /// Reads the PVS file, every shader in `archive` and the index of every
    /// bundle pack, on all hardware threads. Throws DataError if the PVS
    /// file does not have the expected layout.
    static TrackTextures load(ByteView pvs, const lzx::ZipArchive& archive);

    /// Texture ids used by render object `object`, or nullptr if the PVS file
    /// has no such object.
    const std::vector<std::uint32_t>* objectTextures(std::uint32_t object) const;
    std::size_t objectCount() const { return m_objects.size(); }

    /// Layout of the shader at `path` as written in a render model, such as
    /// "shaders\track\h_diff_1.fx"; nullptr if the archive lacks it.
    const ShaderLayout* shader(std::string_view path) const;
    std::size_t shaderCount() const { return m_shaders.size(); }

    /// The file name stem of texture `id`, e.g. "_0x00002B40".
    static std::string textureName(std::uint32_t id);

    /// The number of the render object in an archive entry name such as
    /// "coloradoout.06454.rmb.bin", or nothing for other names.
    static std::optional<std::uint32_t> objectNumber(std::string_view entryName);

    /// Lists the textures in a bundle pack. Throws DataError if it is
    /// malformed.
    static std::vector<BundleEntry> readBundle(ByteView pack);

    /// Texture records with a copy in a bundle pack.
    std::size_t bundledTextureCount() const { return m_bundled.size(); }

    /// Loads and decodes texture `id` from its full-size files, or from a
    /// bundle pack when the archive has none. `archive` must be the one
    /// load() indexed. Safe to call from several threads. Throws DataError
    /// if the texture is missing or cannot be decoded.
    TextureMipChain loadTexture(const lzx::ZipArchive& archive, std::uint32_t id) const;

private:
    struct BundleLocation {
        /// Index into lzx::ZipArchive::entries().
        std::size_t archiveEntry = 0;
        BundleEntry entry;
    };
    class PackCache;

    /// The lookup key of a shader path: its file name, lower case, without
    /// extensions.
    static std::string shaderKey(std::string_view path);
    TextureMipChain loadBundled(const lzx::ZipArchive& archive, std::uint32_t id) const;

    std::vector<std::vector<std::uint32_t>> m_objects;
    /// Texture id to texture record number.
    std::unordered_map<std::uint32_t, std::uint32_t> m_records;
    std::unordered_map<std::string, ShaderLayout> m_shaders;
    /// Texture record number to its copy in a bundle pack.
    std::unordered_map<std::uint32_t, BundleLocation> m_bundled;
    std::unique_ptr<PackCache> m_packs;
};

} // namespace openhorizon::world
