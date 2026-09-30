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

namespace fh::world {

/// The track's compiled effects (`shaders/track/*.fxobj` in bin.zip), which
/// the render models name as `shaders\track\<name>.fx`, parsed once.
///
/// Every track effect has a "Default" technique whose single pass draws the
/// surface, and one vertex declaration: a big-endian stream of POSITION0
/// (float3), NORMAL0 and TANGENT0 (DEC3N), TEXCOORD0-2 (two unsigned 16-bit
/// fractions) and COLOR0 (D3DCOLOR), in the order the model files store
/// them (verified: the declaration's stride equals the vertex stride of every
/// model part drawn with it).
class TrackShaders {
public:
    /// The render states the Default pass sets. A pass holds, at the offset
    /// in its fourth word, a list whose seventh word counts (state, value)
    /// word pairs; the states are Xbox 360 D3DRS values (byte offsets into
    /// the device's state block). Read from the data of all 173 Colorado
    /// effects; which state each number is follows the Xbox 360 SDK's
    /// numbering (40 ZENABLE, 48 ZWRITEENABLE, 56 CULLMODE, 60
    /// ALPHABLENDENABLE, 72 SRCBLEND, 76 DESTBLEND, 96 ALPHATESTENABLE, 204
    /// SLOPESCALEDEPTHBIAS, 208 DEPTHBIAS, 336 ALPHATOMASKENABLE, 340
    /// ALPHATOMASKOFFSETS), which the values fit: blending passes pair 6
    /// (source alpha) with 7 (inverse source alpha) and do not write depth,
    /// decals carry a slope scale of 1 and a bias of 5e-5, the two-sided
    /// foliage effects ("_2sd") and flags cull nothing. State 308, set by
    /// every blending pass, is not identified.
    struct RenderState {
        bool depthWrite = true;
        /// Xbox 360 D3DCULL: 0 none, 2 (every other effect) the back faces.
        std::uint32_t cull = 2;
        /// Whether the pass sets the cull mode; when not, the device keeps
        /// whatever the caller set.
        bool cullGiven = false;
        bool blend = false;
        /// Xbox 360 D3DBLEND: 0 zero, 1 one, 6 source alpha, 7 inverse
        /// source alpha.
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
        /// The layout buildShadedMesh() converts them to (see
        /// renderDeclaration()).
        xenos::VertexDeclaration renderDeclaration;
        RenderState state;
        /// The first pass states of the DepthOnly and ShadowDepthOnly
        /// techniques, which 170 of Colorado's 173 effects have.
        std::optional<RenderState> depthOnlyState;
        std::optional<RenderState> shadowDepthOnlyState;
        /// The first pass states of the RealtimeCubemap technique, which
        /// the environment cube's faces draw with (172 of Colorado's 173
        /// effects have it; light_pollution does not).
        std::optional<RenderState> realtimeCubemapState;
    };

    /// Reads every `.fxobj` of `archive` on all hardware threads. Effects that
    /// cannot be parsed, or lack a Default technique or a vertex declaration,
    /// are left out and counted in failed().
    static TrackShaders load(const lzx::ZipArchive& archive);

    /// The effect a render model names by `path`, or nothing.
    std::optional<std::uint32_t> find(std::string_view path) const;
    const Entry& at(std::uint32_t index) const { return m_entries[index]; }
    std::size_t size() const { return m_entries.size(); }
    int failed() const { return m_failed; }

    /// The lookup key of a shader path: its file name, lower case, without
    /// extensions.
    static std::string key(std::string_view path);

    /// The declaration the renderer binds converted vertices with: the same
    /// elements in the same order, with positions, normals and tangents as
    /// three little-endian floats, texture coordinates as two little-endian
    /// floats and colours as their four bytes unchanged. Nothing if an
    /// element has a type the conversion does not know.
    static std::optional<xenos::VertexDeclaration> renderDeclaration(const xenos::VertexDeclaration& file);

    /// The render states of the first pass of technique `technique` of an
    /// effect file. Throws DataError if the file does not have the layout
    /// RenderState describes.
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

} // namespace fh::world
