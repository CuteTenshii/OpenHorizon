#pragma once

#include "base/Bytes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fh {

class Vfs;

/// The game's localised text, `media/stringtables/<LANG>.zip` → `*.str`
/// (big-endian):
///
///     "LSB2", header words, at 0x28 u32 count N, then N entries
///     {u16 key, u32 offset} sorted by key, a 0xFFFF sentinel entry, then
///     NUL-terminated UTF-16BE strings; offsets count UTF-16 units from the
///     start of the strings
///
/// Database text such as `_&202713995` refers to key `value & 0xFFFF` of the
/// table named after the database table (Events.str for Events), as the
/// map manager established; the high bits identify the table.
class StringTable {
public:
    /// Parses one `.str` file. Throws DataError if it is malformed.
    static StringTable parse(ByteView data, const std::string& source);

    std::optional<std::string> get(std::uint16_t key) const;
    std::size_t size() const { return m_strings.size(); }

private:
    std::unordered_map<std::uint16_t, std::string> m_strings;
};

/// Every string table of one language, loaded on first use.
class Localisation {
public:
    /// `language` is the folder name under media/stringtables ("en").
    Localisation(const Vfs& vfs, std::string language);

    /// The text for a database reference `_&<number>` looked up in `table`
    /// (e.g. "Events"); anything else is returned unchanged. Unknown keys
    /// give the reference itself, so missing text stays visible.
    std::string resolve(std::string_view table, std::string_view reference);

private:
    const StringTable* table(std::string_view name);

    const Vfs& m_vfs;
    std::string m_language;
    std::unordered_map<std::string, std::optional<StringTable>> m_tables;
};

} // namespace fh
