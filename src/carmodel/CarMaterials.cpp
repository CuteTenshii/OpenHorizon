#include "CarMaterials.h"

#include "base/Bytes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>

namespace fh::carmodel {

namespace {

bool contains(std::string_view text, std::string_view word)
{
    return text.find(word) != std::string_view::npos;
}

bool containsAny(std::string_view text, std::span<const std::string_view> words)
{
    for (const std::string_view word : words) {
        if (contains(text, word)) {
            return true;
        }
    }
    return false;
}

// Material names of lamps: head, tail, indicator, fog, reverse, brake and
// side lights and daytime running lights, their reflectors and the
// `lights_*` techniques.
constexpr std::array<std::string_view, 19> kLightWords{"light", "indicator", "fog", "reverse", "reflector", "drl",
    "xenon", "hidhead", "oldhead", "fullbeam", "beam", "orange", "brk", "brin", "linred", "rinred", "redsec",
    "tailside", "taillight"};
constexpr std::array<std::string_view, 7> kMetalWords{
    "chrome", "metal", "rim", "hub", "rotor", "caliper", "aluminum"};
constexpr std::array<std::string_view, 4> kRubberWords{"tire", "tyre", "rubber", "weatherstrip"};
constexpr std::array<std::string_view, 3> kPaintWords{"body", "bottom", "paint"};
constexpr std::array<std::string_view, 9> kDecalWords{
    "badge", "emblem", "numplate", "numberplate", "grille", "stripe", "decal", "sticker", "logo"};
constexpr std::array<std::string_view, 11> kInteriorWords{"leather", "cloth", "gauge", "suede", "headliner",
    "wood", "stitch", "interior", "mottled", "digital", "needle"};
constexpr std::array<std::string_view, 3> kGlassWords{"glass", "window", "lexan"};

/// Slots whose parts come in upgrade variants, longest first so that
/// "exhaustL" wins over "exhaust".
constexpr std::array<std::string_view, 11> kVariantSlots{"undercarriage", "exhaustL", "exhaustR", "bumperF",
    "bumperR", "exhaust", "skirtL", "skirtR", "wheel", "hood", "wing"};

/// Cockpit materials drawn with the interior atlas; the others in the
/// cockpit file (paint, trim and lamps of the bonnet, headlights and
/// mirrors it shares with the outside) use the exterior's textures.
bool isInteriorMaterial(std::string_view name)
{
    constexpr std::array<std::string_view, 5> kExact{"plastic", "metal", "metal2", "solid", "rubber"};
    return containsAny(name, kInteriorWords) || name.starts_with("bump_") || name.starts_with("mirror")
        || std::find(kExact.begin(), kExact.end(), name) != kExact.end();
}

std::vector<TextureCandidate> carTextures(std::string_view stem)
{
    const std::string name(stem);
    return {{TextureFolder::Car, name + "_lod0.xds"}, {TextureFolder::Car, name + ".xds"}};
}

std::vector<TextureCandidate> sharedTexture(std::string_view file)
{
    return {{TextureFolder::Shared, std::string(file)}};
}

} // namespace

Shading classifyMaterial(std::string_view material, ModelSource source, bool translucent)
{
    const std::string name = toLower(material);
    if (translucent) {
        return Shading::Glass;
    }
    if (source == ModelSource::Wheel && contains(name, "tire")) {
        return Shading::Rubber;
    }
    if (containsAny(name, kDecalWords)) {
        return Shading::Decal;
    }
    if (containsAny(name, kLightWords)) {
        return Shading::Lights;
    }
    if (containsAny(name, kRubberWords)) {
        return Shading::Rubber;
    }
    if (containsAny(name, kMetalWords)) {
        return Shading::Metal;
    }
    if (isInteriorMaterial(name)) {
        return Shading::Interior;
    }
    if (containsAny(name, kPaintWords)) {
        return Shading::Paint;
    }
    return Shading::Plain;
}

std::vector<TextureCandidate> diffuseTextures(std::string_view material, ModelSource source)
{
    const std::string name = toLower(material);
    if (source == ModelSource::Brake) {
        return carTextures("nodamage");
    }
    if (source == ModelSource::Wheel) {
        if (contains(name, "tire")) {
            return sharedTexture("tirea0.xds");
        }
        if (isMotionBlurMaterial(name)) {
            return {{TextureFolder::Wheel, "wheel0_lod0.xds"}, {TextureFolder::Wheel, "wheel0.xds"}};
        }
        return {{TextureFolder::Wheel, "wheel_lod0.xds"}, {TextureFolder::Wheel, "wheel.xds"}};
    }
    if (contains(name, "undercarriage")) {
        return sharedTexture("undercarriage.xds");
    }
    if (contains(name, "bumper_frame")) {
        return sharedTexture("bumper_frame.xds");
    }
    if (contains(name, "grille1")) {
        return sharedTexture("grille1_alpha.xds");
    }
    if (contains(name, "grille2")) {
        return sharedTexture("grille2_alpha.xds");
    }
    if (contains(name, "numplate") || contains(name, "numberplate")) {
        return sharedTexture("traffic_numplates_dxt1.xds");
    }
    if (contains(name, "carbon_fiber")) {
        return sharedTexture("carbonfiber.xds");
    }
    if (name.starts_with("detail_glass") || name.starts_with("lights_gls")) {
        return carTextures("lights");
    }
    // Window glass and the black paint spread their coordinates over the
    // whole atlas, crossing unrelated images: they are shaded without it.
    if (containsAny(name, kGlassWords) || name == "black") {
        return {};
    }
    if (containsAny(name, kLightWords)) {
        return carTextures("lights");
    }
    if (contains(name, "gauge") || contains(name, "digital")) {
        return {{TextureFolder::Car, "interior_emissive_lod0.xds"}, {TextureFolder::Car, "interior_lod0.xds"}};
    }
    if (isInteriorMaterial(name)) {
        std::vector<TextureCandidate> candidates = carTextures("interior");
        const std::vector<TextureCandidate> exterior = carTextures("nodamage");
        candidates.insert(candidates.end(), exterior.begin(), exterior.end());
        return candidates;
    }
    return carTextures("nodamage");
}

glm::vec4 baseColour(std::string_view material, Shading shading, bool textured)
{
    const std::string name = toLower(material);
    switch (shading) {
    case Shading::Glass:
        return textured ? glm::vec4(1.0F, 1.0F, 1.0F, 0.5F) : glm::vec4(0.08F, 0.09F, 0.1F, 0.35F);
    case Shading::Rubber:
        return textured ? glm::vec4(1.0F) : glm::vec4(0.05F, 0.05F, 0.05F, 1.0F);
    case Shading::Metal:
        return textured ? glm::vec4(1.0F) : glm::vec4(0.7F, 0.7F, 0.72F, 1.0F);
    case Shading::Paint:
    case Shading::Lights:
    case Shading::Interior:
    case Shading::Decal:
    case Shading::Plain:
        break;
    }
    if (contains(name, "black")) {
        return glm::vec4(0.03F, 0.03F, 0.03F, 1.0F);
    }
    return textured ? glm::vec4(1.0F) : glm::vec4(0.5F, 0.5F, 0.5F, 1.0F);
}

bool isMotionBlurMaterial(std::string_view material)
{
    const std::string name = toLower(material);
    return contains(name, "blur");
}

PartVariant partVariant(std::string_view partName)
{
    PartVariant result;
    const std::string lower = toLower(partName);
    for (const std::string_view slot : kVariantSlots) {
        const std::string slotLower = toLower(slot);
        if (!lower.starts_with(slotLower)) {
            continue;
        }
        std::string rest = lower.substr(slotLower.size());
        const std::size_t underscore = rest.find('_');
        if (underscore != std::string::npos) {
            rest.resize(underscore);
        }
        const bool letter = rest.size() == 1 && std::isalpha(static_cast<unsigned char>(rest[0])) != 0;
        if (rest == "race" || letter) {
            result.slot = std::string(slot);
            result.variant = rest;
            break;
        }
    }
    result.stock = result.variant.empty() || result.variant == "a";
    if (contains(lower, "race") || lower.starts_with("lexan")) {
        result.stock = false;
    }
    return result;
}

} // namespace fh::carmodel
