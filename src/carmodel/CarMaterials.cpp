#include "CarMaterials.h"

#include "base/Bytes.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <span>

namespace fh::carmodel {

namespace {

/// Case-insensitive search, as the executable's 0x82CC0AE0 does for part
/// names.
bool containsNoCase(std::string_view text, std::string_view word)
{
    return toLower(text).find(toLower(word)) != std::string::npos;
}

bool contains(std::string_view text, std::string_view word)
{
    return text.find(word) != std::string_view::npos;
}

bool startsWithAny(std::string_view text, std::span<const std::string_view> prefixes)
{
    return std::any_of(prefixes.begin(), prefixes.end(), [&](std::string_view p) { return text.starts_with(p); });
}

bool containsAny(std::string_view text, std::span<const std::string_view> words)
{
    return std::any_of(words.begin(), words.end(), [&](std::string_view w) { return contains(text, w); });
}

/// Part names of the cockpit rule, in the order 0x82E62700 onwards tests
/// them.
constexpr std::array<std::string_view, 19> kCockpitParts{"interior", "seat", "steering_wheel", "speed", "fuel", "boost",
    "oiltemp", "oilpressure", "watertemp", "volt", "gaugea", "gaugeb", "gaugec", "gauged", "gaugee", "gaugef", "power",
    "doorCard", "tach"};
/// The rule's last name, tested after "tach" at 0x82E62A18 and on.
constexpr std::string_view kCockpitBoot = "boot";

/// Materials the brake rule keeps (0x82E62590 to 0x82E626C0).
constexpr std::array<std::string_view, 6> kBrakeKeep{
    "ghost", "geoShadowBrake", "shadowMap", "SMask", "dropShadowBrake", "brake_badge"};

/// Slots whose parts come in upgrade variants, longest first so that
/// "exhaustL" wins over "exhaust".
constexpr std::array<std::string_view, 11> kVariantSlots{"undercarriage", "exhaustL", "exhaustR", "bumperF", "bumperR",
    "exhaust", "skirtL", "skirtR", "wheel", "hood", "wing"};

bool isCockpitPart(std::string_view partName)
{
    return std::any_of(kCockpitParts.begin(), kCockpitParts.end(), [&](std::string_view p) {
        return containsNoCase(partName, p);
    }) || containsNoCase(partName, kCockpitBoot);
}

/// The brake rule: a rotor or caliper part draws with the technique of that
/// name unless its material is one of the kept ones.
std::optional<std::string> brakeTechnique(std::string_view material, std::string_view partName)
{
    for (const std::string_view kind : {std::string_view("rotor"), std::string_view("caliper")}) {
        if (!containsNoCase(partName, kind)) {
            continue;
        }
        const bool kept = std::any_of(kBrakeKeep.begin(), kBrakeKeep.end(),
            [&](std::string_view word) { return containsNoCase(material, word); });
        if (!kept) {
            return std::string(kind);
        }
    }
    return std::nullopt;
}

std::string_view searchFolder(std::string_view searchPath, const TextureFolders& folders)
{
    if (searchPath == "%CARFOLDER%") {
        return folders.car;
    }
    if (searchPath == "%WHEELFOLDER%") {
        return folders.wheel;
    }
    return folders.shared;
}

std::string trim(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t");
    return std::string(text.substr(first, last - first + 1));
}

} // namespace

bool isTyreMaterial(std::string_view material)
{
    constexpr std::array<std::string_view, 6> kPrefixes{
        "tire", "dropShadowTire", "sidewall", "tread", "scaling_text", "wheel_black"};
    constexpr std::array<std::string_view, 5> kWords{"ghostTire", "geoShadowTire", "tire_", "sidewall", "tread"};
    return startsWithAny(material, kPrefixes) || containsAny(material, kWords);
}

bool isRimMaterial(std::string_view material)
{
    constexpr std::array<std::string_view, 13> kPrefixes{"rim", "ghostRim", "inner_rim", "outer_rim", "chrome_rim",
        "chrome_blur_rim", "chrome_blur_lip", "wheel_emblem", "wheel_black", "blur_rim", "blur_lip", "dropShadowRim",
        "geoShadowRim"};
    return startsWithAny(material, kPrefixes) || contains(material, "rim_");
}

std::string techniqueName(std::string_view material, std::string_view partName, bool cockpitFlag)
{
    if (std::optional<std::string> brake = brakeTechnique(material, partName)) {
        return *brake;
    }
    if (cockpitFlag && isCockpitPart(partName) && !contains(material, "shadowMap")) {
        return "cockpit_" + std::string(material);
    }
    if (!cockpitFlag && (isTyreMaterial(material) || isRimMaterial(material))) {
        return std::string(material) + "_V2";
    }
    if (partName == "glassF" || partName == "lexanF") {
        if (material == "window") {
            return "frontwindow";
        }
        if (material == "window_2") {
            return "frontwindow_2";
        }
    }
    return std::string(material);
}

std::string missingTechnique(std::string_view material, bool cockpitFlag)
{
    if (isTyreMaterial(material)) {
        return cockpitFlag ? "missingTire" : "missingTire_V2";
    }
    return "missing";
}

bool isMotionBlurMaterial(std::string_view material)
{
    return contains(toLower(material), "blur");
}

bool loadFlagsMatch(std::string_view flags, const std::vector<std::string>& set)
{
    std::size_t start = 0;
    while (start <= flags.size()) {
        const std::size_t end = std::min(flags.find('&', start), flags.size());
        std::string term = trim(flags.substr(start, end - start));
        if (!term.empty()) {
            const bool negated = term.front() == '!';
            if (negated) {
                term = trim(std::string_view(term).substr(1));
            }
            const bool isSet = std::any_of(
                set.begin(), set.end(), [&](const std::string& f) { return toLower(f) == toLower(term); });
            if (isSet == negated) {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}

CarTextureTable CarTextureTable::load(const Vfs& vfs)
{
    CarTextureTable table;
    const std::optional<std::string> text = vfs.readText("media/cars/shared/textures.xml");
    if (!text) {
        return table;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return table;
    }
    for (const pugi::xml_node sampler : doc.child("TextureDefinitions").children()) {
        std::vector<Entry>& entries = table.m_samplers[sampler.name()];
        for (const pugi::xml_node texture : sampler.children("Texture")) {
            entries.push_back({texture.attribute("value").as_string(), texture.attribute("searchPath").as_string(),
                texture.attribute("loadFlags").as_string()});
        }
    }
    return table;
}

std::optional<std::string> CarTextureTable::resolve(
    std::string_view sampler, const std::vector<std::string>& flags, const TextureFolders& folders) const
{
    const auto found = m_samplers.find(sampler);
    if (found == m_samplers.end()) {
        return std::nullopt;
    }
    for (const Entry& entry : found->second) {
        if (loadFlagsMatch(entry.loadFlags, flags)) {
            return std::string(searchFolder(entry.searchPath, folders)) + toLower(entry.file);
        }
    }
    return std::nullopt;
}

std::optional<ShaderSettings> ShaderSettings::load(const Vfs& vfs, const std::string& path)
{
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    ShaderSettings settings;
    for (const pugi::xml_node technique : doc.child("ShaderSettings").children()) {
        if (technique.type() != pugi::node_element) {
            continue;
        }
        Parameters& parameters = settings.m_techniques[technique.name()];
        for (const pugi::xml_node parameter : technique.children()) {
            const std::string_view name = parameter.name();
            if (parameter.type() != pugi::node_element || name.starts_with("D3DRS_")) {
                continue;
            }
            if (const pugi::xml_attribute value = parameter.attribute("value")) {
                parameters[std::string(name)] = glm::vec4(value.as_float(), 0.0F, 0.0F, 0.0F);
            } else if (parameter.attribute("r")) {
                parameters[std::string(name)]
                    = glm::vec4(parameter.attribute("r").as_float(), parameter.attribute("g").as_float(),
                        parameter.attribute("b").as_float(), parameter.attribute("a").as_float());
            }
        }
    }
    return settings;
}

void ShaderSettings::overlay(const ShaderSettings& other)
{
    for (const auto& [technique, parameters] : other.m_techniques) {
        Parameters& mine = m_techniques[technique];
        for (const auto& [name, value] : parameters) {
            mine[name] = value;
        }
    }
}

const ShaderSettings::Parameters* ShaderSettings::technique(std::string_view technique) const
{
    const auto found = m_techniques.find(technique);
    return found == m_techniques.end() ? nullptr : &found->second;
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
