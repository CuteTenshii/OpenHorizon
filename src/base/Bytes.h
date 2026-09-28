#pragma once

#include "Error.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

inline std::uint16_t loadBe16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t loadBe32(const std::uint8_t* p)
{
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

inline float loadBeF32(const std::uint8_t* p)
{
    return std::bit_cast<float>(loadBe32(p));
}

inline std::uint32_t loadLe32(const std::uint8_t* p)
{
    return (std::uint32_t{p[3]} << 24) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[1]} << 8) | p[0];
}

inline std::uint16_t loadLe16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[1] << 8) | p[0]);
}

/// IEEE half float to float, as the Xenos GPU and D3DX store them.
float halfToFloat(std::uint16_t half);

/// Bounds-checked reader of big-endian data, the byte order of every Xbox 360
/// file. Reading past the end throws DataError naming the source and offset,
/// so parsers can read whole structures and catch once.
class BeReader {
public:
    BeReader(ByteView data, std::string source)
        : m_data(data)
        , m_source(std::move(source))
    {
    }

    std::size_t pos() const { return m_pos; }
    std::size_t size() const { return m_data.size(); }
    std::size_t remaining() const { return m_data.size() - m_pos; }
    bool atEnd() const { return m_pos >= m_data.size(); }
    ByteView data() const { return m_data; }
    const std::string& source() const { return m_source; }

    void seek(std::size_t pos)
    {
        if (pos > m_data.size()) {
            fail(std::format("seek to 0x{:x} past the end (0x{:x})", pos, m_data.size()));
        }
        m_pos = pos;
    }

    void skip(std::size_t bytes)
    {
        need(bytes);
        m_pos += bytes;
    }

    void align(std::size_t alignment)
    {
        const std::size_t aligned = (m_pos + alignment - 1) / alignment * alignment;
        seek(aligned);
    }

    void need(std::size_t bytes) const
    {
        if (bytes > m_data.size() - m_pos) {
            fail(std::format("need {} bytes, {} left", bytes, m_data.size() - m_pos));
        }
    }

    std::uint8_t u8()
    {
        need(1);
        return m_data[m_pos++];
    }

    std::uint16_t u16()
    {
        need(2);
        const std::uint16_t v = loadBe16(m_data.data() + m_pos);
        m_pos += 2;
        return v;
    }

    std::uint32_t u32()
    {
        need(4);
        const std::uint32_t v = loadBe32(m_data.data() + m_pos);
        m_pos += 4;
        return v;
    }

    std::uint64_t u64()
    {
        const std::uint64_t high = u32();
        return (high << 32) | u32();
    }

    std::int32_t s32() { return static_cast<std::int32_t>(u32()); }
    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }
    float f32() { return std::bit_cast<float>(u32()); }
    float f16() { return halfToFloat(u16()); }

    /// Reads a u32 and fails unless it equals `value`.
    void expect(std::uint32_t value, const char* what)
    {
        const std::uint32_t got = u32();
        if (got != value) {
            m_pos -= 4;
            fail(std::format("expected {} = {}, found {}", what, value, got));
        }
    }

    ByteView bytes(std::size_t count)
    {
        need(count);
        const ByteView view = m_data.subspan(m_pos, count);
        m_pos += count;
        return view;
    }

    /// `length` bytes of text with trailing NULs removed.
    std::string fixedString(std::size_t length)
    {
        const ByteView raw = bytes(length);
        std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
        const std::size_t nul = text.find('\0');
        if (nul != std::string::npos) {
            text.resize(nul);
        }
        return text;
    }

    /// Text up to the next NUL, which is consumed.
    std::string cString()
    {
        std::string text;
        while (true) {
            const std::uint8_t c = u8();
            if (c == 0) {
                return text;
            }
            text.push_back(static_cast<char>(c));
        }
    }

    [[noreturn]] void fail(const std::string& message) const
    {
        throw DataError("{}: offset 0x{:x}: {}", m_source, m_pos, message);
    }

private:
    ByteView m_data;
    std::string m_source;
    std::size_t m_pos = 0;
};

/// ASCII lower case, for the case-insensitive names the game uses.
std::string toLower(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
bool endsWithNoCase(std::string_view text, std::string_view suffix);
bool startsWithNoCase(std::string_view text, std::string_view prefix);

} // namespace fh
