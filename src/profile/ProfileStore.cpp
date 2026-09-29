#include "ProfileStore.h"

#include "base/Log.h"

#include <pugixml.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sstream>
#include <system_error>

namespace fh::profile {

namespace {

constexpr const char* kRoot = "ForzaProfile";
constexpr const char* kBags = "Profile";
constexpr const char* kGameState = "GameState";
constexpr const char* kState = "State";

/// Field names of the leaf states; the game's fields have offsets, not
/// names (see Profile.h).
constexpr const char* kActivityDiscovered = "ActivityDiscovered";
constexpr const char* kDiscovered = "Discovered";
constexpr const char* kPersonalBest = "PersonalBest";
constexpr const char* kCollected = "Collected";

void writeProperty(pugi::xml_node parent, const Property& property)
{
    pugi::xml_node node = parent.append_child("Property");
    node.append_attribute("name") = property.name.c_str();
    node.append_attribute("type") = std::string(propertyTypeName(property.type)).c_str();
    if (property.type == PropertyType::PropertyBag) {
        for (const Property& child : property.children) {
            writeProperty(node, child);
        }
    } else {
        node.append_attribute("value") = formatValue(property.value).c_str();
    }
}

void writeField(pugi::xml_node state, const char* name, const PropertyValue& value, PropertyType type)
{
    writeProperty(state, Property{name, type, value, {}});
}

pugi::xml_node writeState(pugi::xml_node parent, std::string_view name, std::string_view type, int version)
{
    pugi::xml_node node = parent.append_child(kState);
    node.append_attribute("name") = std::string(name).c_str();
    node.append_attribute("type") = std::string(type).c_str();
    node.append_attribute("version") = version;
    return node;
}

/// Reads the saved bag `saved` over `target`, which comes from the schema
/// when `target` is not new.
void readBag(const pugi::xml_node saved, std::vector<Property>& target, const std::string& path)
{
    for (const pugi::xml_node node : saved.children("Property")) {
        const std::string name = node.attribute("name").as_string();
        const std::string_view typeName = node.attribute("type").as_string();
        const std::optional<PropertyType> type = propertyTypeFromName(typeName);
        std::string fullPath = path;
        if (!fullPath.empty()) {
            fullPath += '/';
        }
        fullPath += name;
        if (name.empty() || !type) {
            logWarning("profile: {} has no name or an unknown type \"{}\"; ignored", fullPath, typeName);
            continue;
        }
        Property* existing = nullptr;
        for (Property& property : target) {
            if (property.name == name) {
                existing = &property;
                break;
            }
        }
        if (existing != nullptr && existing->type != *type) {
            logWarning("profile: {} is saved as {} but the schema has {}; the default is kept", fullPath, typeName,
                propertyTypeName(existing->type));
            continue;
        }
        if (*type == PropertyType::PropertyBag) {
            if (existing == nullptr) {
                // Not in the schema: kept as saved, so nothing is lost when
                // another schema had it.
                target.push_back(Property{name, *type, {}, {}});
                existing = &target.back();
            }
            readBag(node, existing->children, fullPath);
            continue;
        }
        std::optional<PropertyValue> value = parseValue(*type, node.attribute("value").as_string());
        if (!value) {
            logWarning("profile: {} has no valid {} value; {}", fullPath, typeName,
                existing != nullptr ? "the default is kept" : "ignored");
            continue;
        }
        if (existing == nullptr) {
            target.push_back(Property{name, *type, std::move(*value), {}});
        } else {
            existing->value = std::move(*value);
        }
    }
}

/// The value of field `name` of `state`, when it has the type given.
template <typename T> std::optional<T> readField(const pugi::xml_node state, const char* name, PropertyType type)
{
    const pugi::xml_node field = state.find_child_by_attribute("Property", "name", name);
    if (!field || propertyTypeFromName(field.attribute("type").as_string()) != type) {
        return std::nullopt;
    }
    const std::optional<PropertyValue> value = parseValue(type, field.attribute("value").as_string());
    if (!value) {
        return std::nullopt;
    }
    return std::get<T>(*value);
}

std::string hexWords(const std::vector<bool>& bits)
{
    std::string text;
    for (std::size_t word = 0; word * 32 < bits.size(); ++word) {
        std::uint32_t value = 0;
        for (std::size_t bit = 0; bit < 32 && word * 32 + bit < bits.size(); ++bit) {
            if (bits[word * 32 + bit]) {
                value |= 1U << bit;
            }
        }
        if (!text.empty()) {
            text += ' ';
        }
        text += std::format("{:08x}", value);
    }
    return text;
}

std::optional<std::vector<bool>> bitsFromHex(std::string_view text, std::size_t count)
{
    std::vector<bool> bits(count, false);
    std::size_t word = 0;
    while (!text.empty()) {
        const std::size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            break;
        }
        text.remove_prefix(start);
        const std::size_t end = std::min(text.find_first_of(" \t\r\n"), text.size());
        std::uint32_t value = 0;
        const char* first = text.data();
        const auto [last, error] = std::from_chars(first, first + end, value, 16);
        if (error != std::errc() || last != first + end) {
            return std::nullopt;
        }
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const std::size_t index = word * 32 + bit;
            if ((value >> bit & 1U) != 0 && index >= count) {
                return std::nullopt;
            }
            if (index < count) {
                bits[index] = (value >> bit & 1U) != 0;
            }
        }
        ++word;
        text.remove_prefix(end);
    }
    if (word != (count + 31) / 32) {
        return std::nullopt;
    }
    return bits;
}

void readState(const pugi::xml_node node, GameState& state)
{
    const std::string name = node.attribute("name").as_string();
    const std::string_view type = node.attribute("type").as_string();
    const int version = node.attribute("version").as_int(-1);
    const auto wrongVersion = [&](int expected) {
        if (version == expected) {
            return false;
        }
        logWarning("profile: state {} ({}) has version {}, not {}; dropped", name, type, version, expected);
        return true;
    };
    if (type == RoadDiscoveryState::kType) {
        if (wrongVersion(RoadDiscoveryState::kVersion)) {
            return;
        }
        const std::optional<std::vector<bool>> bits
            = bitsFromHex(node.text().as_string(), node.attribute("nodes").as_ullong());
        if (!bits) {
            logWarning("profile: {} does not hold its node count of bits; dropped", name);
            return;
        }
        state.roads = RoadDiscoveryState{*bits};
    } else if (type == SpeedCameraState::kType) {
        if (wrongVersion(SpeedCameraState::kVersion)) {
            return;
        }
        SpeedCameraState camera;
        camera.activityDiscovered = readField<bool>(node, kActivityDiscovered, PropertyType::Bool).value_or(false);
        camera.discovered = readField<bool>(node, kDiscovered, PropertyType::Bool).value_or(false);
        camera.personalBest = readField<float>(node, kPersonalBest, PropertyType::Float32).value_or(0.0F);
        state.speedCameras.insert_or_assign(name, camera);
    } else if (type == FlyerState::kType) {
        if (wrongVersion(FlyerState::kVersion)) {
            return;
        }
        FlyerState flyer;
        flyer.activityDiscovered = readField<bool>(node, kActivityDiscovered, PropertyType::Bool).value_or(false);
        flyer.collected = readField<bool>(node, kCollected, PropertyType::Bool).value_or(false);
        state.flyers.insert_or_assign(name, flyer);
    } else if (type == GasStationState::kType) {
        if (wrongVersion(GasStationState::kVersion)) {
            return;
        }
        GasStationState station;
        station.discovered = readField<bool>(node, kDiscovered, PropertyType::Bool).value_or(false);
        state.gasStations.insert_or_assign(name, station);
    } else {
        std::ostringstream text;
        node.print(text, "", pugi::format_raw);
        state.otherStates.push_back(text.str());
    }
}

bool writeFile(const std::filesystem::path& path, std::string_view content)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        logWarning("profile: cannot create {}: {}", path.string(), std::strerror(errno));
        return false;
    }
    bool ok = true;
    while (!content.empty()) {
        const ssize_t written = ::write(fd, content.data(), content.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    // Flushed before the rename, so the name never points at a file whose
    // data is not on disk yet.
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        logWarning("profile: cannot write {}: {}", path.string(), std::strerror(errno));
    }
    return ok;
}

} // namespace

std::filesystem::path ProfileStore::defaultDirectory()
{
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "fh1-native" / "profile";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "fh1-native" / "profile";
    }
    return std::filesystem::path("fh1-native-profile");
}

ProfileStore::ProfileStore(std::filesystem::path directory)
    : m_directory(std::move(directory))
{
}

ProfileStore::LoadResult ProfileStore::load(const ProfileSchema& schema)
{
    LoadResult result{LoadStatus::Missing, Profile(schema)};
    m_setAside = false;
    std::error_code error;
    if (!std::filesystem::exists(file(), error)) {
        return result;
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_file(file().c_str());
    const pugi::xml_node root = document.child(kRoot);
    if (!parsed || !root) {
        logWarning("profile: {} is not a profile ({}); starting a new one", file().string(), parsed.description());
        result.status = LoadStatus::Unreadable;
        m_setAside = true;
        return result;
    }
    if (const int version = root.attribute("version").as_int(-1); version != kFormatVersion) {
        logWarning("profile: {} has format version {}, this game reads {}; starting a new one", file().string(),
            version, kFormatVersion);
        result.status = LoadStatus::VersionMismatch;
        m_setAside = true;
        return result;
    }
    readBag(root.child(kBags), result.profile.bags(), "");
    if (const pugi::xml_node state = root.child(kGameState)) {
        if (const int version = state.attribute("version").as_int(-1); version != GameState::kVersion) {
            logWarning("profile: game state version {} is not {}; not read", version, GameState::kVersion);
        } else {
            for (const pugi::xml_node node : state.children(kState)) {
                readState(node, result.profile.state);
            }
        }
    }
    result.status = LoadStatus::Loaded;
    return result;
}

std::string ProfileStore::serialize(const Profile& profile)
{
    pugi::xml_document document;
    pugi::xml_node root = document.append_child(kRoot);
    root.append_attribute("version") = kFormatVersion;
    pugi::xml_node bags = root.append_child(kBags);
    for (const Property& bag : profile.bags()) {
        writeProperty(bags, bag);
    }
    pugi::xml_node states = root.append_child(kGameState);
    states.append_attribute("version") = GameState::kVersion;
    const GameState& state = profile.state;
    if (state.roads) {
        pugi::xml_node roads
            = writeState(states, RoadDiscoveryState::kName, RoadDiscoveryState::kType, RoadDiscoveryState::kVersion);
        roads.append_attribute("nodes") = static_cast<unsigned long long>(state.roads->visited.size());
        roads.text() = hexWords(state.roads->visited).c_str();
    }
    for (const auto& [name, camera] : state.speedCameras) {
        pugi::xml_node node = writeState(states, name, SpeedCameraState::kType, SpeedCameraState::kVersion);
        writeField(node, kActivityDiscovered, camera.activityDiscovered, PropertyType::Bool);
        writeField(node, kDiscovered, camera.discovered, PropertyType::Bool);
        writeField(node, kPersonalBest, camera.personalBest, PropertyType::Float32);
    }
    for (const auto& [name, flyer] : state.flyers) {
        pugi::xml_node node = writeState(states, name, FlyerState::kType, FlyerState::kVersion);
        writeField(node, kActivityDiscovered, flyer.activityDiscovered, PropertyType::Bool);
        writeField(node, kCollected, flyer.collected, PropertyType::Bool);
    }
    for (const auto& [name, station] : state.gasStations) {
        pugi::xml_node node = writeState(states, name, GasStationState::kType, GasStationState::kVersion);
        writeField(node, kDiscovered, station.discovered, PropertyType::Bool);
    }
    for (const std::string& other : state.otherStates) {
        pugi::xml_document kept;
        if (kept.load_string(other.c_str()) && kept.first_child()) {
            states.append_copy(kept.first_child());
        }
    }
    std::ostringstream text;
    document.save(text, "  ");
    return text.str();
}

bool ProfileStore::save(const Profile& profile)
{
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        logWarning("profile: cannot create {}: {}", m_directory.string(), error.message());
        return false;
    }
    if (m_setAside) {
        std::filesystem::path aside = file();
        aside += ".unreadable";
        std::filesystem::rename(file(), aside, error);
        if (error) {
            logWarning("profile: cannot move {} aside: {}", file().string(), error.message());
            return false;
        }
        logInfo("profile: kept the unreadable profile as {}", aside.string());
        m_setAside = false;
    }
    std::filesystem::path temporary = file();
    temporary += ".tmp";
    if (!writeFile(temporary, serialize(profile))) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, file(), error);
    if (error) {
        logWarning("profile: cannot replace {}: {}", file().string(), error.message());
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace fh::profile
