#include "Archive.h"

#include <lzx/zip.h>

#include <charconv>
#include <unordered_set>

namespace fh::world {

namespace {

constexpr unsigned kWindowBits = 17;

} // namespace

Bytes readEntry(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry)
{
    try {
        return archive.read(entry);
    } catch (const std::exception& e) {
        throw DataError("{}: {}", entry.name, e.what());
    }
}

Bytes readEntryPrefix(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry, std::size_t size)
{
    try {
        switch (entry.method) {
        case lzx::kMethodStored: {
            Bytes raw = archive.rawData(entry);
            raw.resize(std::min(raw.size(), size));
            return raw;
        }
        case lzx::kMethodXMemCompress: {
            const Bytes raw = archive.rawData(entry);
            Bytes data = lzx::decompress(raw, entry.uncompressedSize, kWindowBits, size);
            data.resize(std::min(data.size(), size));
            return data;
        }
        default: {
            Bytes data = archive.read(entry);
            data.resize(std::min(data.size(), size));
            return data;
        }
        }
    } catch (const std::exception& e) {
        throw DataError("{}: {}", entry.name, e.what());
    }
}

std::vector<std::size_t> distinctEntries(const lzx::ZipArchive& archive, std::string_view suffix)
{
    std::vector<std::size_t> result;
    std::unordered_set<std::string> seen;
    const auto& entries = archive.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (endsWithNoCase(entries[i].name, suffix)
            && seen.insert(lzx::ZipArchive::normalizeName(entries[i].name)).second) {
            result.push_back(i);
        }
    }
    return result;
}

std::optional<std::uint32_t> numberBefore(std::string_view text, char prefix, std::string_view suffix)
{
    if (!endsWithNoCase(text, suffix)) {
        return std::nullopt;
    }
    const std::size_t end = text.size() - suffix.size();
    std::size_t start = end;
    while (start > 0 && text[start - 1] >= '0' && text[start - 1] <= '9') {
        --start;
    }
    if (start == end || start == 0 || !iequals(text.substr(start - 1, 1), std::string_view(&prefix, 1))) {
        return std::nullopt;
    }
    std::uint32_t number = 0;
    const auto [ptr, ec] = std::from_chars(text.data() + start, text.data() + end, number);
    if (ec != std::errc() || ptr != text.data() + end) {
        return std::nullopt;
    }
    return number;
}

} // namespace fh::world
