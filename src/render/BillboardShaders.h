#pragma once

#include <string>

/// GLSL ports of PROC_MODEL_BILLBOARD: vertex shaders BILLBOARD (0x82189510),
/// _4SIDED (0x8218D608), _NONFACING (0x82189D90), _NONFACING_2SIDED
/// (0x8218B558); pixel shaders BILLBOARD (0x82166B98), _NONFACING (0x82166D70).
/// Types 0/1 use BILLBOARD, 4/5 NONFACING (0x82ED0648). The vertex shaders
/// match the translated microcode to 2e-3. FadeValues (c157) is (-2fS, -fS, 0,
/// -w) in the main view (0x824477A4), w = 50 (0x833A57BC).
namespace openhorizon::render::billboardglsl {

/// Uniform, storage and texture bindings of the programs.
inline constexpr unsigned kFrameBinding = 4;
inline constexpr unsigned kCardBinding = 9;
inline constexpr unsigned kDiffuseUnit = 0;
inline constexpr unsigned kShadowMaskUnit = 13;

/// Floats of one card in the storage buffer (eight vec4, see
/// BillboardRenderer).
inline constexpr unsigned kCardVec4s = 8;

struct Variant {
    /// Types 4 and 5: cards fixed to the copy's axes, lit per pixel.
    bool nonFacing = false;
    /// Type 1: four pictures by the viewing direction.
    bool fourSided = false;
    /// Type 5: a second picture seen from behind.
    bool twoSided = false;
};

std::string vertexSource(const Variant& variant);
std::string fragmentSource(const Variant& variant);

} // namespace openhorizon::render::billboardglsl
