#pragma once

#include <string>

/// GLSL for the animated objects, written from the executable's
/// PROC_ANIM_OBJ_* shaders (debug paths ...\Shaders\PROC_ANIM_OBJ_*.updb;
/// vertex shaders at 0x82174C18-0x82181110, pixel shaders at
/// 0x8215D568-0x82164190). The code follows each variant's microcode step by
/// step and was checked on the GPU against the translation of the microcode
/// (xenos::translate) with random inputs, agreeing to about 3e-6.
///
/// The executable builds its variants at 0x82EC5380 and picks one per draw
/// from a table of 144 (vertex shader, pixel shader, declaration) triples
/// indexed by `kind * 36 + 12 * geometry + m` (0x82445488). Geometry is
/// rigid, four bones or one bone a vertex; m says which of emissive, normal
/// and specular maps the pixel shader reads. Every emissive vertex shader
/// has the microcode of its plain twin.
///
/// Ported here: the three geometries, with and without a normal map and an
/// emissive map, lit by the sun and ambient with the screen's shadow mask
/// and fogged. Left out:
///  - the specular variants: their power constant (c1.x) is the material's
///    power times 3 over a value the draw reads from an object whose origin
///    was not traced (vtable slot 16 of the draw's fourth argument, +600,
///    0x824458E4), so the specular term is not drawn;
///  - kinds 1 to 3 (DIFF_ONLY, the subsurface pixel shaders and
///    LIGHT_SHAFT), since which field of a draw selects the kind was not
///    traced;
///  - the deferred headlight term (pixel bool b100), whose buffer this
///    renderer has not (as for the track effects);
///  - the depth-only variants, as the passes that draw them were not traced.
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
    /// The opaque pass's alpha to mask, which on this renderer's single
    /// sample target keeps a pixel when its alpha reaches one half.
    bool alphaToMask = false;
};

std::string vertexSource(const Variant& variant);
std::string fragmentSource(const Variant& variant);

} // namespace openhorizon::render::animatedglsl
