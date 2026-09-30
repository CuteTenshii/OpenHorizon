#include "ProfileStore.h"

#include "base/Error.h"
#include "base/Host.h"
#include "base/Log.h"

#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <system_error>

namespace openhorizon::profile {

namespace {

constexpr const char* kUnreadableSuffix = ".unreadable";

/// Bits of the leaf states' flag bytes.
constexpr std::uint8_t kFirstFlag = 1;
constexpr std::uint8_t kSecondFlag = 2;

/// Magic, format version and checksum: the smallest file there can be
/// holds these and an empty body.
constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kChecksumSize = 4;

constexpr std::array<std::uint32_t, 256> kCrcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
        }
        table[index] = value;
    }
    return table;
}();

class Writer {
public:
    void byte(std::uint8_t value) { m_bytes.push_back(static_cast<char>(value)); }
    void varint(std::uint64_t value)
    {
        while (value >= 0x80) {
            byte(static_cast<std::uint8_t>(value | 0x80));
            value >>= 7;
        }
        byte(static_cast<std::uint8_t>(value));
    }
    void zigzag(std::int64_t value)
    {
        varint((static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63));
    }
    template <typename T> void little(T value)
    {
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            byte(static_cast<std::uint8_t>(value >> (8 * index)));
        }
    }
    void text(std::string_view text)
    {
        varint(text.size());
        raw(text);
    }
    void raw(std::string_view bytes) { m_bytes.append(bytes); }
    const std::string& bytes() const { return m_bytes; }
    std::string& bytes() { return m_bytes; }

private:
    std::string m_bytes;
};

/// The string table being built: each name once, numbered in order of
/// first use.
class NameTable {
public:
    std::size_t index(std::string_view name)
    {
        const auto found = m_indices.find(name);
        if (found != m_indices.end()) {
            return found->second;
        }
        m_names.emplace_back(name);
        m_indices.emplace(std::string(name), m_names.size() - 1);
        return m_names.size() - 1;
    }
    const std::vector<std::string>& names() const { return m_names; }

private:
    std::vector<std::string> m_names;
    std::map<std::string, std::size_t, std::less<>> m_indices;
};

/// The value `value` holds as a T, or zero when it holds another kind,
/// which Profile::set and the readers never let a property have.
template <typename T> T valueAs(const PropertyValue& value)
{
    const T* held = std::get_if<T>(&value);
    return held != nullptr ? *held : T{};
}

void writeList(Writer& out, NameTable& names, const std::vector<Property>& properties)
{
    out.varint(properties.size());
    for (const Property& property : properties) {
        out.varint(names.index(property.name));
        out.byte(static_cast<std::uint8_t>(property.type));
        switch (property.type) {
        case PropertyType::PropertyBag:
            writeList(out, names, property.children);
            break;
        case PropertyType::Bool:
            out.byte(static_cast<std::uint8_t>(valueAs<bool>(property.value)));
            break;
        case PropertyType::UInt8:
        case PropertyType::UInt16:
        case PropertyType::UInt32:
        case PropertyType::UInt64:
            out.varint(valueAs<std::uint64_t>(property.value));
            break;
        case PropertyType::Int8:
        case PropertyType::Int16:
        case PropertyType::Int32:
        case PropertyType::Int64:
            out.zigzag(valueAs<std::int64_t>(property.value));
            break;
        case PropertyType::Float32:
            out.little(std::bit_cast<std::uint32_t>(valueAs<float>(property.value)));
            break;
        case PropertyType::Float64:
            out.little(std::bit_cast<std::uint64_t>(valueAs<double>(property.value)));
            break;
        case PropertyType::StringNarrow:
        case PropertyType::StringWide:
            out.text(valueAs<std::string>(property.value));
            break;
        }
    }
}

/// Appends one state group: `states` holds `count` states' names and
/// bodies.
void writeGroup(
    Writer& out, NameTable& names, std::string_view type, int version, std::size_t count, const Writer& states)
{
    out.varint(names.index(type));
    out.zigzag(version);
    out.varint(count);
    out.varint(states.bytes().size());
    out.raw(states.bytes());
}

/// The state tree after its version and size: the groups.
std::string writeTree(NameTable& names, const GameState& state)
{
    Writer groups;
    std::size_t groupCount = 0;
    if (state.roads) {
        const std::vector<bool>& visited = state.roads->visited;
        Writer body;
        body.text(RoadDiscoveryState::kName);
        body.varint(visited.size());
        for (std::size_t word = 0; word * 32 < visited.size(); ++word) {
            std::uint32_t value = 0;
            for (std::size_t bit = 0; bit < 32 && word * 32 + bit < visited.size(); ++bit) {
                if (visited[word * 32 + bit]) {
                    value |= 1U << bit;
                }
            }
            body.little(value);
        }
        writeGroup(groups, names, RoadDiscoveryState::kType, RoadDiscoveryState::kVersion, 1, body);
        ++groupCount;
    }
    const auto flags = [](bool first, bool second) {
        return static_cast<std::uint8_t>((first ? kFirstFlag : 0) | (second ? kSecondFlag : 0));
    };
    if (!state.speedCameras.empty()) {
        Writer body;
        for (const auto& [name, camera] : state.speedCameras) {
            body.text(name);
            body.byte(flags(camera.activityDiscovered, camera.discovered));
            body.little(std::bit_cast<std::uint32_t>(camera.personalBest));
        }
        writeGroup(groups, names, SpeedCameraState::kType, SpeedCameraState::kVersion, state.speedCameras.size(), body);
        ++groupCount;
    }
    if (!state.flyers.empty()) {
        Writer body;
        for (const auto& [name, flyer] : state.flyers) {
            body.text(name);
            body.byte(flags(flyer.activityDiscovered, flyer.collected));
        }
        writeGroup(groups, names, FlyerState::kType, FlyerState::kVersion, state.flyers.size(), body);
        ++groupCount;
    }
    if (!state.gasStations.empty()) {
        Writer body;
        for (const auto& [name, station] : state.gasStations) {
            body.text(name);
            body.byte(flags(station.discovered, false));
        }
        writeGroup(groups, names, GasStationState::kType, GasStationState::kVersion, state.gasStations.size(), body);
        ++groupCount;
    }
    // One group per run of states of the same type and version, so the
    // order they were read in is kept.
    for (std::size_t first = 0; first < state.otherStates.size();) {
        const OtherState& head = state.otherStates[first];
        std::size_t last = first;
        Writer body;
        while (last < state.otherStates.size() && state.otherStates[last].type == head.type
            && state.otherStates[last].version == head.version) {
            body.text(state.otherStates[last].name);
            writeList(body, names, state.otherStates[last].fields);
            ++last;
        }
        writeGroup(groups, names, head.type, head.version, last - first, body);
        ++groupCount;
        first = last;
    }
    Writer tree;
    tree.varint(groupCount);
    tree.raw(groups.bytes());
    return std::move(tree.bytes());
}

/// Reads the binary layout; throws DataError when the bytes run out or do
/// not have it.
class Reader {
public:
    explicit Reader(std::string_view bytes)
        : m_bytes(bytes)
    {
    }
    bool atEnd() const { return m_offset == m_bytes.size(); }
    std::string_view raw(std::size_t size)
    {
        if (size > m_bytes.size() - m_offset) {
            throw DataError("truncated at byte {}", m_offset);
        }
        const std::string_view bytes = m_bytes.substr(m_offset, size);
        m_offset += size;
        return bytes;
    }
    std::size_t remaining() const { return m_bytes.size() - m_offset; }
    std::uint8_t byte() { return static_cast<std::uint8_t>(raw(1)[0]); }
    std::uint64_t varint()
    {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t next = byte();
            const std::uint64_t bits = next & 0x7FU;
            if (shift == 63 && bits > 1) {
                throw DataError("a number at byte {} does not fit 64 bits", m_offset);
            }
            value |= bits << shift;
            if ((next & 0x80U) == 0) {
                return value;
            }
        }
        throw DataError("a number at byte {} does not fit 64 bits", m_offset);
    }
    std::int64_t zigzag()
    {
        const std::uint64_t value = varint();
        return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1U);
    }
    template <typename T> T little()
    {
        const std::string_view bytes = raw(sizeof(T));
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            value |= std::uint64_t{static_cast<std::uint8_t>(bytes[index])} << (8 * index);
        }
        return static_cast<T>(value);
    }
    std::string_view text() { return raw(varint()); }
    /// The next `size` bytes, as a reader of their own.
    Reader part(std::uint64_t size) { return Reader(raw(size)); }

private:
    std::string_view m_bytes;
    std::size_t m_offset = 0;
};

const std::string& readName(Reader& in, const std::vector<std::string>& names)
{
    const std::uint64_t index = in.varint();
    if (index >= names.size()) {
        throw DataError("name {} is not in the string table of {}", index, names.size());
    }
    return names[index];
}

PropertyType readType(Reader& in)
{
    const std::uint8_t type = in.byte();
    if (type > static_cast<std::uint8_t>(PropertyType::StringWide)
        && type != static_cast<std::uint8_t>(PropertyType::PropertyBag)) {
        throw DataError("unknown property type {}", type);
    }
    return static_cast<PropertyType>(type);
}

/// A value of `type`, which is not a bag; nothing when it does not fit the
/// type (the bytes are read all the same).
std::optional<PropertyValue> readValue(Reader& in, PropertyType type)
{
    PropertyValue value;
    switch (type) {
    case PropertyType::PropertyBag:
        return std::nullopt;
    case PropertyType::Bool: {
        const std::uint8_t flag = in.byte();
        if (flag > 1) {
            return std::nullopt;
        }
        value = flag == 1;
        break;
    }
    case PropertyType::UInt8:
    case PropertyType::UInt16:
    case PropertyType::UInt32:
    case PropertyType::UInt64:
        value = in.varint();
        break;
    case PropertyType::Int8:
    case PropertyType::Int16:
    case PropertyType::Int32:
    case PropertyType::Int64:
        value = in.zigzag();
        break;
    case PropertyType::Float32:
        value = std::bit_cast<float>(in.little<std::uint32_t>());
        break;
    case PropertyType::Float64:
        value = std::bit_cast<double>(in.little<std::uint64_t>());
        break;
    case PropertyType::StringNarrow:
    case PropertyType::StringWide:
        value = std::string(in.text());
        break;
    }
    if (!valueFits(type, value)) {
        return std::nullopt;
    }
    return value;
}

/// Bags nest one deep in the game's schema; the limit only keeps a crafted
/// file from exhausting the stack.
constexpr int kMaxBagDepth = 64;

/// Reads a property list over `target`, which comes from the schema when
/// `target` is not new: a property saved with another type than the
/// schema's, or with a value that does not fit its type, keeps the
/// default; one the schema does not have is kept as read.
void readList(Reader& in, const std::vector<std::string>& names, std::vector<Property>& target, const std::string& path,
    int depth = 0)
{
    if (depth > kMaxBagDepth) {
        throw DataError("{} holds bags nested deeper than {}", path, kMaxBagDepth);
    }
    const std::uint64_t count = in.varint();
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::string& name = readName(in, names);
        const PropertyType type = readType(in);
        std::string fullPath = path;
        if (!fullPath.empty()) {
            fullPath += '/';
        }
        fullPath += name;
        Property* existing = nullptr;
        for (Property& property : target) {
            if (property.name == name) {
                existing = &property;
                break;
            }
        }
        bool usable = true;
        if (name.empty()) {
            logWarning("profile: {} has a property with no name; ignored", path);
            usable = false;
        } else if (existing != nullptr && existing->type != type) {
            logWarning("profile: {} is saved as {} but the schema has {}; the default is kept", fullPath,
                propertyTypeName(type), propertyTypeName(existing->type));
            usable = false;
        }
        if (type == PropertyType::PropertyBag) {
            if (!usable) {
                std::vector<Property> skipped;
                readList(in, names, skipped, fullPath, depth + 1);
                continue;
            }
            if (existing == nullptr) {
                // Not in the schema: kept as saved, so nothing is lost when
                // another schema had it.
                target.push_back(Property{name, type, {}, {}});
                existing = &target.back();
            }
            readList(in, names, existing->children, fullPath, depth + 1);
            continue;
        }
        std::optional<PropertyValue> value = readValue(in, type);
        if (!usable) {
            continue;
        }
        if (!value) {
            logWarning("profile: {} has no valid {} value; {}", fullPath, propertyTypeName(type),
                existing != nullptr ? "the default is kept" : "ignored");
            continue;
        }
        if (existing == nullptr) {
            target.push_back(Property{name, type, std::move(*value), {}});
        } else {
            existing->value = std::move(*value);
        }
    }
}

std::optional<std::vector<bool>> readRoadBits(Reader& in)
{
    const std::uint64_t count = in.varint();
    // Checked before the sizes below are computed, so they cannot overflow.
    if (count > in.remaining() * 8) {
        throw DataError("{} road nodes do not fit the {} bytes left", count, in.remaining());
    }
    std::vector<bool> bits(count, false);
    bool fits = true;
    for (std::uint64_t word = 0; word < (count + 31) / 32; ++word) {
        const auto value = in.little<std::uint32_t>();
        for (std::uint64_t bit = 0; bit < 32; ++bit) {
            const std::uint64_t node = word * 32 + bit;
            if ((value >> bit & 1U) == 0) {
                continue;
            }
            if (node < count) {
                bits[node] = true;
            } else {
                fits = false;
            }
        }
    }
    if (!fits) {
        return std::nullopt;
    }
    return bits;
}

void readGroup(Reader& tree, const std::vector<std::string>& names, GameState& state)
{
    const std::string& type = readName(tree, names);
    const std::int64_t savedVersion = tree.zigzag();
    if (savedVersion < std::numeric_limits<int>::min() || savedVersion > std::numeric_limits<int>::max()) {
        throw DataError("state version {} is out of range", savedVersion);
    }
    const auto version = static_cast<int>(savedVersion);
    const std::uint64_t count = tree.varint();
    Reader states = tree.part(tree.varint());
    int expected = -1;
    if (type == RoadDiscoveryState::kType) {
        expected = RoadDiscoveryState::kVersion;
    } else if (type == SpeedCameraState::kType) {
        expected = SpeedCameraState::kVersion;
    } else if (type == FlyerState::kType) {
        expected = FlyerState::kVersion;
    } else if (type == GasStationState::kType) {
        expected = GasStationState::kVersion;
    }
    if (expected >= 0 && version != expected) {
        logWarning("profile: {} {} states have version {}, not {}; dropped", count, type, version, expected);
        return;
    }
    for (std::uint64_t index = 0; index < count; ++index) {
        std::string name(states.text());
        if (type == RoadDiscoveryState::kType) {
            std::optional<std::vector<bool>> bits = readRoadBits(states);
            if (!bits) {
                logWarning("profile: {} has bits past its node count; dropped", name);
                continue;
            }
            state.roads = RoadDiscoveryState{std::move(*bits)};
        } else if (type == SpeedCameraState::kType) {
            const std::uint8_t flags = states.byte();
            const auto best = std::bit_cast<float>(states.little<std::uint32_t>());
            state.speedCameras.insert_or_assign(
                std::move(name), SpeedCameraState{(flags & kFirstFlag) != 0, (flags & kSecondFlag) != 0, best});
        } else if (type == FlyerState::kType) {
            const std::uint8_t flags = states.byte();
            state.flyers.insert_or_assign(
                std::move(name), FlyerState{(flags & kFirstFlag) != 0, (flags & kSecondFlag) != 0});
        } else if (type == GasStationState::kType) {
            const std::uint8_t flags = states.byte();
            state.gasStations.insert_or_assign(std::move(name), GasStationState{(flags & kFirstFlag) != 0});
        } else {
            OtherState other{std::move(name), type, version, {}};
            readList(states, names, other.fields, other.name);
            state.otherStates.push_back(std::move(other));
        }
    }
    if (!states.atEnd()) {
        throw DataError("{} states hold {} bytes more than their {} states", type, states.remaining(), count);
    }
}

/// Reads the binary file's content after its header and before its
/// checksum over `profile`. Throws DataError.
void readBinary(std::string_view body, Profile& profile)
{
    Reader in(body);
    const std::uint64_t nameCount = in.varint();
    // Every name takes at least its length byte.
    if (nameCount > in.remaining()) {
        throw DataError("{} names do not fit the {} bytes left", nameCount, in.remaining());
    }
    std::vector<std::string> names;
    names.reserve(nameCount);
    for (std::uint64_t index = 0; index < nameCount; ++index) {
        names.emplace_back(in.text());
    }
    readList(in, names, profile.bags(), "");
    const std::uint64_t treeVersion = in.varint();
    Reader tree = in.part(in.varint());
    if (!in.atEnd()) {
        throw DataError("{} bytes follow the game state", in.remaining());
    }
    if (treeVersion != GameState::kVersion) {
        logWarning("profile: game state version {} is not {}; not read", treeVersion, GameState::kVersion);
        return;
    }
    const std::uint64_t groups = tree.varint();
    for (std::uint64_t index = 0; index < groups; ++index) {
        readGroup(tree, names, profile.state);
    }
    if (!tree.atEnd()) {
        throw DataError("{} bytes follow the state groups", tree.remaining());
    }
}

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace

std::uint32_t crc32(std::string_view data)
{
    std::uint32_t value = 0xFFFFFFFFU;
    for (const char byte : data) {
        value = kCrcTable[(value ^ static_cast<std::uint8_t>(byte)) & 0xFFU] ^ (value >> 8);
    }
    return value ^ 0xFFFFFFFFU;
}

std::filesystem::path ProfileStore::defaultDirectory()
{
    const std::filesystem::path folder = userFolder(UserFolder::Data);
    if (folder.empty()) {
        return std::filesystem::path("openhorizon-profile");
    }
    return folder / "profile";
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
    if (std::filesystem::exists(file(), error)) {
        result.status = loadBinary(schema, result.profile);
        m_setAside = result.status != LoadStatus::Loaded;
    }
    return result;
}

ProfileStore::LoadStatus ProfileStore::loadBinary(const ProfileSchema& schema, Profile& profile) const
{
    const std::string where = pathToUtf8(file());
    const std::optional<std::string> bytes = readFile(file());
    if (!bytes) {
        logWarning("profile: cannot read {}; starting a new one", where);
        return LoadStatus::Unreadable;
    }
    const std::string_view data = *bytes;
    if (!data.starts_with(kMagic)) {
        logWarning("profile: {} is not a profile; starting a new one", where);
        return LoadStatus::Unreadable;
    }
    if (data.size() < kHeaderSize + kChecksumSize) {
        logWarning("profile: {} is truncated; starting a new one", where);
        return LoadStatus::Unreadable;
    }
    Reader header(data.substr(kMagic.size(), 2));
    if (const auto version = header.little<std::uint16_t>(); version != kFormatVersion) {
        logWarning("profile: {} has format version {}, this game reads {}; starting a new one", where, version,
            kFormatVersion);
        return LoadStatus::VersionMismatch;
    }
    const std::string_view content = data.substr(0, data.size() - kChecksumSize);
    const auto saved = Reader(data.substr(content.size())).little<std::uint32_t>();
    if (const std::uint32_t computed = crc32(content); computed != saved) {
        logWarning("profile: {} is damaged or truncated (checksum {:08x}, not {:08x}); starting a new one", where,
            computed, saved);
        return LoadStatus::Unreadable;
    }
    Profile read(schema);
    try {
        readBinary(content.substr(kHeaderSize), read);
    } catch (const DataError& e) {
        logWarning("profile: {} is not a profile ({}); starting a new one", where, e.what());
        return LoadStatus::Unreadable;
    }
    profile = std::move(read);
    return LoadStatus::Loaded;
}

std::string ProfileStore::serialize(const Profile& profile)
{
    NameTable names;
    Writer body;
    writeList(body, names, profile.bags());
    const std::string tree = writeTree(names, profile.state);
    body.varint(GameState::kVersion);
    body.varint(tree.size());
    body.raw(tree);

    Writer out;
    out.raw(kMagic);
    out.little(static_cast<std::uint16_t>(kFormatVersion));
    out.varint(names.names().size());
    for (const std::string& name : names.names()) {
        out.text(name);
    }
    out.raw(body.bytes());
    out.little(crc32(out.bytes()));
    return std::move(out.bytes());
}

bool ProfileStore::save(const Profile& profile)
{
    std::error_code error;
    std::filesystem::create_directories(m_directory, error);
    if (error) {
        logWarning("profile: cannot create {}: {}", pathToUtf8(m_directory), error.message());
        return false;
    }
    if (m_setAside) {
        std::filesystem::path aside = file();
        aside += kUnreadableSuffix;
        std::filesystem::rename(file(), aside, error);
        if (error) {
            logWarning("profile: cannot move {} aside: {}", pathToUtf8(file()), error.message());
            return false;
        }
        logInfo("profile: kept the unreadable profile as {}", pathToUtf8(aside));
        m_setAside = false;
    }
    std::filesystem::path temporary = file();
    temporary += ".tmp";
    // Written to the disk before the rename, so the name never points at a
    // file whose data is not there yet.
    if (std::string written; !writeFileToDisk(temporary, serialize(profile), &written)) {
        logWarning("profile: {}", written);
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, file(), error);
    if (error) {
        logWarning("profile: cannot replace {}: {}", pathToUtf8(file()), error.message());
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace openhorizon::profile
