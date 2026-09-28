#pragma once

#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fh::carmodel {

/// How a batch should be lit and blended, from its material name.
enum class Shading : std::uint8_t {
    /// Opaque, lit with the diffuse texture (trim, frame, plastics).
    Plain,
    /// Car paint: the diffuse texture tinted with the body colour.
    Paint,
    /// Translucent glass, drawn after the opaque batches.
    Glass,
    /// Lamps and their reflectors, which glow when switched on.
    Lights,
    /// Chrome and bare metal: mostly reflection.
    Metal,
    /// Tyres, rubber seals.
    Rubber,
    /// Cockpit trim.
    Interior,
    /// Alpha-tested overlays: badges, number plates, grilles.
    Decal,
};

/// Folder a texture is looked up in.
enum class TextureFolder : std::uint8_t {
    /// `media/cars/<car>/`.
    Car,
    /// `media/cars/shared/`.
    Shared,
    /// `media/wheels/<wheel>/`.
    Wheel,
};

/// A texture file a material may use, in order of preference.
struct TextureCandidate {
    TextureFolder folder = TextureFolder::Car;
    std::string file;
};

/// Where a material's mesh comes from, which decides the default texture of
/// materials that do not name one.
enum class ModelSource : std::uint8_t { Exterior, Cockpit, Wheel, Brake };

/// Shading of a material. `translucent` is the model's own verdict (its draw
/// lists), which wins over the name: opaque meshes named after glass, such as
/// the blackened `black_glass` border, stay opaque.
///
/// The game binds every in-race material to a technique of
/// `media/shaders/cars/shaders_v16.fxobj` (the separate `<name>.fxobj`
/// shaders are listed in `customcarshaders.xml` as showroom-only). That
/// effect's technique table is not decoded, so the classification is by
/// name; the word lists come from the material names used by the 176 cars.
Shading classifyMaterial(std::string_view material, ModelSource source, bool translucent);

/// Diffuse texture files for a material, best first; empty for materials
/// drawn without one (chrome, rubber, calipers, which the game tints).
///
/// Sampler bindings are those of `media/cars/shared/textures.xml` for the
/// in-race (non-"SuperLOD") entries, plus the executable's fixed `nodamage`
/// for baseSampler. The choice of sampler per material is by name, checked
/// by laying each material's texture coordinates over the candidate
/// textures: exterior materials and brakes use the car's
/// `nodamage_lod0.xds` atlas (which holds the brake disc and caliper
/// badge), lamps and lamp lenses (`detail_glass_*`) `lights_lod0.xds`,
/// cockpit trim `interior_lod0.xds`, wheel rims the wheel's
/// `wheel_lod0.xds`, blurred rims `wheel0.xds`, tyres the shared
/// `tirea0.xds`, and shared-texture materials (undercarriage, bumper frame,
/// grilles, number plates, carbon fibre) their files in `media/cars/shared`.
/// Window glass and `black` have none: their coordinates cross the atlas
/// without following its images.
std::vector<TextureCandidate> diffuseTextures(std::string_view material, ModelSource source);

/// A colour to multiply the diffuse texture by (or to use alone when
/// `textured` is false), RGBA 0 to 1: dark tinted translucent glass, near
/// black for rubber and black trim, light grey metal, white otherwise; paint
/// is white for the renderer to tint with the body colour. Chosen by name;
/// the game's own material constants are not decoded.
glm::vec4 baseColour(std::string_view material, Shading shading, bool textured);

/// True for meshes that stand in for a spinning wheel at speed (blurred
/// rims and tyre lips); a renderer shows them instead of the sharp ones
/// above some wheel speed.
bool isMotionBlurMaterial(std::string_view material);

/// Upgrade variant of a part, from its name: exterior parts that the game
/// swaps for upgrades are named `<slot><letter>`, where `a` is the stock part
/// (the upgrade tables' sequence 0) and later letters are aftermarket kits,
/// or `<slot>race` for the race kit (`bumperFrace`, `wingrace`).
struct PartVariant {
    /// The slot, e.g. "bumperF", "skirtL", "wing"; empty for parts without
    /// variants.
    std::string slot;
    /// "a", "b", ... or "race"; empty for parts without variants.
    std::string variant;

    /// True for parts a stock car shows: no variant or variant "a". Race kit
    /// parts other than slots (`cagerace`, `lexan*` windows) are not stock
    /// either.
    bool stock = true;
};

PartVariant partVariant(std::string_view partName);

} // namespace fh::carmodel
