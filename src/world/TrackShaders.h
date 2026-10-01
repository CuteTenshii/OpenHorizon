#pragma once

#include "base/Bytes.h"
#include "xenos/Effect.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

/// The track's compiled effects (`shaders/track/*.fxobj` in bin.zip), named by render models as
/// `shaders\track\<name>.fx`. Each has a single-pass "Default" technique and one big-endian vertex declaration
/// in model-file order. Verified: its stride matches every model part drawn with it.
class TrackShaders {
public:
    /// The Default pass's render states: at the offset in a pass's fourth word, a list whose seventh word counts
    /// (state, value) pairs of Xbox 360 D3DRS byte offsets (40 ZENABLE, 48 ZWRITEENABLE, 56 CULLMODE, 308 unknown,
    /// 60 ALPHABLENDENABLE, 72/76 SRC/DESTBLEND, 96 ALPHATESTENABLE, 204/208 depth bias, 336/340 ALPHATOMASK).
    struct RenderState {
        bool depthWrite = true;
        /// Xbox 360 D3DCULL: 0 none, 2 (every other effect) the back faces.
        std::uint32_t cull = 2;
        /// When false, the device keeps the caller's cull mode.
        bool cullGiven = false;
        bool blend = false;
        /// Xbox 360 D3DBLEND: 0 zero, 1 one, 6 source alpha, 7 inverse source alpha.
        std::uint32_t sourceBlend = 1;
        std::uint32_t destinationBlend = 0;
        float slopeScaleDepthBias = 0.0F;
        float depthBias = 0.0F;
        bool alphaToMask = false;
        /// Two bits per sample of dither offset, in quarters.
        std::uint32_t alphaToMaskOffsets = 0;
    };

    struct Entry {
        /// File name without extension, lower case, e.g. "h_diff_1".
        std::string name;
        xenos::Effect effect;
        /// Index into effect.techniques of "Default".
        std::size_t technique = 0;
        /// The layout of the model files' vertices.
        xenos::VertexDeclaration fileDeclaration;
        /// The layout buildShadedMesh() converts them to (see renderDeclaration()).
        xenos::VertexDeclaration renderDeclaration;
        RenderState state;
        /// First-pass states of the DepthOnly and ShadowDepthOnly techniques, when present.
        std::optional<RenderState> depthOnlyState;
        std::optional<RenderState> shadowDepthOnlyState;
        /// First-pass states of RealtimeCubemap, which the environment cube's faces use (light_pollution lacks it).
        std::optional<RenderState> realtimeCubemapState;
        /// The Default vertex shader reads material constants (vc360_N, or c360_N for bunting): the effects that
        /// animate in the vertex stage, possibly in model space (the flags' sway; see buildShadedMesh()).
        bool readsVertexMaterial = false;
    };

    /// Effects that fail to parse or lack a Default technique or vertex declaration are counted in failed().
    static TrackShaders load(const lzx::ZipArchive& archive);

    /// The effect a render model names by `path`, or nothing.
    std::optional<std::uint32_t> find(std::string_view path) const;
    const Entry& at(std::uint32_t index) const { return m_entries[index]; }
    std::size_t size() const { return m_entries.size(); }
    int failed() const { return m_failed; }

    /// The file name, lower case, without extensions.
    static std::string key(std::string_view path);

    /// The same elements with little-endian float3 positions, normals and tangents, float2 texture coordinates and
    /// colour bytes unchanged. Nothing if an element type is unknown to the conversion.
    static std::optional<xenos::VertexDeclaration> renderDeclaration(const xenos::VertexDeclaration& file);

    /// The first pass's states. Throws DataError if the file lacks the layout RenderState describes.
    static RenderState readRenderState(ByteView fxobj, std::string_view technique);

private:
    std::vector<Entry> m_entries;
    std::unordered_map<std::string, std::uint32_t> m_byKey;
    int m_failed = 0;
};

/// Xbox 360 D3DDECLTYPE values of the track vertex declarations and of the
/// converted layout (see VertexDeclElement::type for the bit fields).
namespace decltype360 {
/// FLOAT3, big-endian (32_32_32_FLOAT, 8-in-32 swap, selectors xyz1).
inline constexpr std::uint32_t kFloat3 = 0x2A23B9;
/// DEC3N (2_10_10_10, signed, 8-in-32 swap, selectors xyzw).
inline constexpr std::uint32_t kDec3N = 0x1A2187;
/// USHORT2N (16_16, 8-in-16 swap, selectors xy01).
inline constexpr std::uint32_t kUShort2N = 0x2C2059;
/// D3DCOLOR (8_8_8_8, 8-in-32 swap, selectors zyxw).
inline constexpr std::uint32_t kColor = 0x182886;
/// FLOAT3 without byte swap: little-endian floats.
inline constexpr std::uint32_t kFloat3Native = 0x2A2339;
/// FLOAT2 without byte swap (32_32_FLOAT, selectors xy01).
inline constexpr std::uint32_t kFloat2Native = 0x2C2025;
} // namespace decltype360

} // namespace openhorizon::world
