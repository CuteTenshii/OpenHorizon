#pragma once

#include "base/Bytes.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace openhorizon::audio {

/// Bounds-checked little-endian reader: FMOD's FSB and FEV files are little-endian even on the Xbox 360.
/// Reading past the end throws DataError naming the source and offset.
class LeReader {
public:
    LeReader(ByteView data, std::string source)
        : m_data(data)
        , m_source(std::move(source))
    {
    }

    std::size_t pos() const { return m_pos; }
    std::size_t size() const { return m_data.size(); }
    std::size_t remaining() const { return m_data.size() - m_pos; }
    bool atEnd() const { return m_pos >= m_data.size(); }
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
        const std::uint16_t v = loadLe16(m_data.data() + m_pos);
        m_pos += 2;
        return v;
    }

    std::uint32_t u32()
    {
        need(4);
        const std::uint32_t v = loadLe32(m_data.data() + m_pos);
        m_pos += 4;
        return v;
    }

    std::uint64_t u64()
    {
        const std::uint64_t low = u32();
        return low | (std::uint64_t{u32()} << 32);
    }

    std::int16_t s16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t s32() { return static_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }

    ByteView bytes(std::size_t count)
    {
        need(count);
        const ByteView view = m_data.subspan(m_pos, count);
        m_pos += count;
        return view;
    }

    /// `length` bytes of text with everything from the first NUL removed.
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

    /// A u32 byte count including the NUL, then the bytes; 0 is empty with no bytes.
    std::string countedString()
    {
        const std::uint32_t length = u32();
        if (length == 0) {
            return {};
        }
        const ByteView raw = bytes(length);
        if (raw.back() != 0) {
            m_pos -= length + 4;
            fail(std::format("string of {} bytes is not NUL terminated", length));
        }
        return std::string(reinterpret_cast<const char*>(raw.data()), raw.size() - 1);
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

} // namespace openhorizon::audio
