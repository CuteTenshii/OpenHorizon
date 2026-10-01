#pragma once

#include <glm/vec4.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::carmodel {

/// Which carbin a mesh comes from.
enum class ModelSource : std::uint8_t { Exterior, Cockpit, Wheel, Brake };

/// The technique of `media/shaders/cars/shaders_v16.fxobj` that draws a
/// mesh, built from its material name the way the executable's name builder
/// does (0x82E62418, called per mesh from 0x82E63204):
///
/// - parts whose names contain rotor or caliper draw with the techniques
///   `rotor` and `caliper`, whatever their material (unless it is a shadow,
///   ghost or brake badge material);
/// - with the builder's flag set, parts whose names contain interior, seat,
///   steering_wheel, a gauge name (speed, tach, fuel, boost, oiltemp,
///   oilpressure, watertemp, volt, gaugea to gaugef, power), doorCard or boot
///   get `cockpit_` before the material; with it clear, tyre and rim
///   materials (the tests at 0x82E54180 and 0x82E542C0) get `_V2` after it;
/// - the windscreen parts `glassF` and `lexanF` turn `window` into
///   `frontwindow` and `window_2` into `frontwindow_2`.
///
/// Verified: those rules and the strings they test. The flag's source is not
/// traced; it is taken as set, the only setting under which every material
/// of the disc names a technique. Ghost, engine bay and SuperLOD variants are
/// not applied.
std::string techniqueName(std::string_view material, std::string_view partName, bool cockpitFlag = true);

/// The technique the executable falls back to when an effect lacks the
/// named one (0x82E75BB8 and 0x82E763C0): a tyre technique for tyre
/// materials (`missingTire` with the flag above set, `missingTire_V2`
/// without), `missing` otherwise.
std::string missingTechnique(std::string_view material, bool cockpitFlag = true);

/// True for tyre materials (0x82E54180).
bool isTyreMaterial(std::string_view material);
/// True for rim materials (0x82E542C0).
bool isRimMaterial(std::string_view material);

/// Render states the car draw sets around one mesh, over those of its draw
/// list and its technique's passes.
struct MaterialDrawState {
    /// Back faces are culled (the list's D3DCULL_CW); false draws both sides.
    bool cull = true;
    /// Colour 0's alpha masks the pixels it covers (RB_COLORCONTROL's
    /// alpha-to-mask bit).
    bool alphaToMask = false;
    /// False forces blending off, whatever the draw list uses.
    bool blend = true;
};
/// The render states the car draw (0x82E63638 to 0x82E63728) sets for a
/// material. Verified: names containing `defrost_lines` (0x82E56238, search
/// at 0x82CC0AE0) draw with culling off (0x8247C4D0), alpha to mask on
/// (0x8247D9A0) and blending off (0x8247C398), restored after; other
/// materials keep the defaults. Inferred: that the name searched is the
/// material's.
/// Inferred: that the name searched is the material's.
MaterialDrawState materialDrawState(std::string_view material);

/// True for materials the shadow map technique choosers (0x82E62130 and
/// 0x82E62338) give no technique, so that they cast no shadow: names
/// containing `defrost_lines` (0x82E56238) or `liverybadge` (0x82E62190 and
/// 0x82E62390, the same search). Not applied: a third early out on a mesh
/// flag byte (+152) whose meaning is not traced.
bool castsNoShadow(std::string_view material);

/// True for meshes that stand in for a spinning wheel at speed (blurred
/// rims and tyre lips); a renderer shows them instead of the sharp ones
/// above some wheel speed.
bool isMotionBlurMaterial(std::string_view material);

/// Folders a car's textures are looked up in, as `media/cars/shared/
/// textures.xml` names them.
struct TextureFolders {
    /// %CARFOLDER%: `media/cars/<car>/`.
    std::string car;
    /// %SHAREDCARFOLDER%: `media/cars/shared/`.
    std::string shared = "media/cars/shared/";
    /// %WHEELFOLDER%: `media/wheels/<wheel>/`.
    std::string wheel;
};

/// The sampler-to-file table of `media/cars/shared/textures.xml`: for each
/// sampler name of the car effects, texture files with a search folder and
/// the load flags that select them (LOD0, LOD15, SuperLOD, Cockpit,
/// InGameTextures, CarCommon, combined with `&` and `!`).
class CarTextureTable {
public:
    struct Entry {
        std::string file;
        /// "%CARFOLDER%", "%SHAREDCARFOLDER%" or "%WHEELFOLDER%".
        std::string searchPath;
        std::string loadFlags;
    };

    /// Reads the table; empty when the file is missing.
    static CarTextureTable load(const Vfs& vfs);

    /// The game path of the texture for `sampler` in a model loaded with
    /// `flags` set (every other flag clear), or nothing when no entry of the
    /// sampler matches or the table does not list it.
    std::optional<std::string> resolve(
        std::string_view sampler, const std::vector<std::string>& flags, const TextureFolders& folders) const;

    const std::map<std::string, std::vector<Entry>, std::less<>>& samplers() const { return m_samplers; }

private:
    std::map<std::string, std::vector<Entry>, std::less<>> m_samplers;
};

/// Whether `flags` (a textures.xml loadFlags expression: names joined by
/// `&`, each optionally negated with `!`) holds for the set flags `set`.
bool loadFlagsMatch(std::string_view flags, const std::vector<std::string>& set);

/// Material parameters from the game's shader settings files: per technique,
/// parameter values by name. `<Name value="v"/>` is (v, 0, 0, 0), `<Name r=
/// g= b= a=/>` a colour. Render state entries (`D3DRS_...`) and sampler
/// entries (no value) are left out.
class ShaderSettings {
public:
    using Parameters = std::map<std::string, glm::vec4, std::less<>>;

    /// Reads one settings file; nothing when it is missing or malformed.
    static std::optional<ShaderSettings> load(const Vfs& vfs, const std::string& path);

    /// Adds `other`'s values over these, technique by technique.
    void overlay(const ShaderSettings& other);

    /// The parameters of `technique`, or nullptr.
    const Parameters* technique(std::string_view technique) const;
    bool empty() const { return m_techniques.empty(); }

private:
    std::map<std::string, Parameters, std::less<>> m_techniques;
};

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

} // namespace openhorizon::carmodel
