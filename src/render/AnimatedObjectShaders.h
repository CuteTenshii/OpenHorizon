#pragma once

#include <string>

/// GLSL ported from the executable's PROC_ANIM_OBJ_* shaders (vertex
/// 0x82174C18-0x82181110, pixel 0x8215D568-0x82164190); matches
/// xenos::translate of the microcode on the GPU to about 3e-6.
///
/// Variants are built at 0x82EC5380 and picked from a table of 144 triples
/// indexed by `kind * 36 + 12 * geometry + m` (0x82445488).
///
/// Left out: specular (c1.x depends on an untraced object, 0x824458E4),
/// kinds 1 to 3 (selector not traced), the deferred headlight term (pixel
/// bool b100) and the depth-only variants.
namespace openhorizon::render::animatedglsl {

/// Uniform and storage bindings of the programs.
inline constexpr unsigned kFrameBinding = 4;
inline constexpr unsigned kDrawBinding = 7;
inline constexpr unsigned kBoneBinding = 8;
/// Texture units: the samplers' registers.
inline constexpr unsigned kDiffuseUnit = 0;
inline constexpr unsigned kNormalUnit = 1;
inline constexpr unsigned kEmissiveUnit = 3;
inline constexpr unsigned kShadowMaskUnit = 13;

struct Variant {
    /// 0 rigid, 1 four bones, 2 one bone a vertex.
    int geometry = 0;
    bool normalMap = false;
    bool emissive = false;
    /// Alpha to mask; on the single-sample target, keeps alpha >= 0.5.
    bool alphaToMask = false;
};

std::string vertexSource(const Variant& variant);
std::string fragmentSource(const Variant& variant);

} // namespace openhorizon::render::animatedglsl
