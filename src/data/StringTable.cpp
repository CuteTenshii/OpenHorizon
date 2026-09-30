#include "StringTable.h"

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <cstdlib>
#include <format>
#include <vector>

namespace openhorizon {

namespace {

constexpr std::size_t kCountOffset = 0x28;
constexpr std::size_t kEntrySize = 6;

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

} // namespace

StringTable StringTable::parse(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    if (data.size() < 4 || std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "LSB2") {
        r.fail("not an LSB2 string table");
    }
    r.seek(kCountOffset);
    const std::uint32_t count = r.u32();
    struct Entry {
        std::uint16_t key;
        std::uint32_t offset;
    };
    std::vector<Entry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t key = r.u16();
        const std::uint32_t offset = r.u32();
        entries.push_back({key, offset});
    }
    // The sentinel entry that closes the table.
    r.skip(kEntrySize);
    const std::size_t stringsStart = r.pos();
    StringTable table;
    for (const Entry& entry : entries) {
        std::size_t at = stringsStart + static_cast<std::size_t>(entry.offset) * 2;
        std::string text;
        while (at + 1 < data.size()) {
            char32_t unit = loadBe16(data.data() + at);
            at += 2;
            if (unit == 0) {
                break;
            }
            // Surrogate pairs, for the few characters outside the BMP.
            if (unit >= 0xD800 && unit < 0xDC00 && at + 1 < data.size()) {
                const char32_t low = loadBe16(data.data() + at);
                if (low >= 0xDC00 && low < 0xE000) {
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                    at += 2;
                }
            }
            appendUtf8(text, unit);
        }
        table.m_strings.emplace(entry.key, std::move(text));
    }
    return table;
}

std::optional<std::string> StringTable::get(std::uint16_t key) const
{
    const auto it = m_strings.find(key);
    if (it == m_strings.end()) {
        return std::nullopt;
    }
    return it->second;
}

Localisation::Localisation(const Vfs& vfs, std::string language)
    : m_vfs(vfs)
    , m_language(std::move(language))
{
}

const StringTable* Localisation::table(std::string_view name)
{
    const std::string key = toLower(name);
    auto it = m_tables.find(key);
    if (it == m_tables.end()) {
        std::optional<StringTable> loaded;
        const std::string path = std::format("media/stringtables/{}/{}.str", m_language, key);
        try {
            if (const std::optional<Bytes> data = m_vfs.read(path)) {
                loaded = StringTable::parse(*data, path);
            }
        } catch (const std::exception& e) {
            logWarning("strings: {}", e.what());
        }
        it = m_tables.emplace(key, std::move(loaded)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::string Localisation::resolve(std::string_view tableName, std::string_view reference)
{
    if (!reference.starts_with("_&")) {
        return std::string(reference);
    }
    const std::string number(reference.substr(2));
    char* end = nullptr;
    const unsigned long long value = std::strtoull(number.c_str(), &end, 10);
    if (end == number.c_str()) {
        return std::string(reference);
    }
    const StringTable* strings = table(tableName);
    if (!strings) {
        return std::string(reference);
    }
    return strings->get(static_cast<std::uint16_t>(value & 0xFFFF)).value_or(std::string(reference));
}

} // namespace openhorizon
