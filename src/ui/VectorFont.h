#pragma once

#include "base/Bytes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::ui {

/// A glyph mesh corner: inside where u*u - v < 0, reversed on `outside`
/// triangles (between outline and box). Straight edges have u = 0; curves use
/// Loop-Blinn (-1,1), (0,-1), (1,1), the game's "Vector AA".
struct GlyphVertex {
    /// Position in ems from the pen, y up.
    glm::vec2 position{0.0F};
    glm::vec2 curve{0.0F};
    bool outside = false;
};

struct Glyph {
    char32_t codePoint = 0;
    /// Pen advance in ems.
    float advance = 0.0F;
    /// Three vertices per triangle, the record's x offset applied.
    std::vector<GlyphVertex> triangles;
    /// Extent of the outline (not the covering mesh) in ems.
    glm::vec2 inkMin{0.0F};
    glm::vec2 inkMax{0.0F};
};

/// A Forza "vfont" (`media/ui/fonts/*_vector_aa.dt`), compiled by Turn 10's
/// FontCompiler from a TrueType face. Layout, big-endian:
///
///     CAFF container: "CAFF21.11.05.0034", u32 section count at 0x38,
///       u32 offset of the first section at 0x48, a table of 0x28-byte
///       section entries from 0x50 ({char name[8] ".data"/".gpu", u32,
///       u32, u32 size, ...}); sections follow each other from that offset.
///     .data: "vfont\0" "12.07.06.0035\0", u32 at 0x18 = character set
///       offset, u32 at 0x1C = geometry header offset (from .data start)
///       character set: u32 at 0x0C = glyph count, 0x10 = default code
///         point, 0x20/0x24 = offset and size of a hash table of
///         {u16 code point, u16 glyph} (not needed: glyphs carry their
///         code point)
///       geometry header: u32 offset of the metrics block that starts at
///         0x0C (the runtime copies 72 bytes from it), u32 glyph records
///         offset, u32 0, u32 1, u32 0, u32 vertex bytes, u32 vertex count,
///         u32 index count, f32 space advance (ems), f32 build size (14),
///         f32 (about 1.02 to 1.04 in the text fonts; not read by the
///         layout), u32 8, 8 x s32 metrics in font units ({1115, 372, 1115,
///         -372, 0, 1115, -372, 0} in fonts A, B and E), s32 font units per
///         em (991 in A, B and E)
///       glyph records, 40 bytes: u32 code point, f32 advance, s16 first
///         kerning pair (-1 for none) and a u16 the game does not read, u32 vertex
///         byte offset, u32 vertex count, u32 first index, u32 triangle
///         count, f32 x offset, f32 y offset (0 in every font), f32 scale
///         (1 in every font). The draw (0x82442cb4) passes the last three
///         to the vertex shader: the meshes start at the pen, and the x
///         offset (about -0.04 to -0.25 em) moves each glyph onto it.
///     .gpu: vertices of four halfs {x, y, u, v}, where the sign of x marks
///       an outside triangle and |x| is the position, then u16 indices
///       (relative to the glyph's first vertex)
///
/// Vertical metrics, verified (0x8242fff0, 0x82430468; line breaker
/// 0x8242fa08): ascent the s32 at 0x44, descent minus the s32 at 0x48, both
/// over units per em at 0x50; 1.125 em and 0.375 em in every text font.
///
/// No kerning: every glyph's first pair is -1 (lookup 0x82ef4cd8). The layout
/// (0x82442d4c) advances by advance times size, unrounded.
class VectorFont {
public:
    /// Throws DataError if the data is not a vfont.
    static VectorFont parse(ByteView data, const std::string& source);

    const Glyph* glyph(char32_t codePoint) const;
    /// The glyph used for characters the font lacks: U+25A1, then '?'.
    const Glyph* fallback() const;
    const std::map<char32_t, Glyph>& glyphs() const { return m_glyphs; }

    float spaceAdvance() const { return m_spaceAdvance; }
    /// Height of a line of text in ems: ascent() + descent().
    float lineHeight() const { return m_lineHeight; }
    /// Height of capitals above the baseline, in ems (from 'H').
    float capHeight() const { return m_capHeight; }
    /// Distance from the top of a line to its baseline, in ems.
    float ascent() const { return m_ascent; }
    /// Distance from the baseline to the bottom of a line, in ems.
    float descent() const { return m_descent; }

    /// Advance of `utf8` in ems, with `trackingEms` added between glyphs.
    float measure(std::string_view utf8, float trackingEms = 0.0F) const;

private:
    std::map<char32_t, Glyph> m_glyphs;
    char32_t m_defaultChar = 0;
    float m_spaceAdvance = 0.25F;
    float m_lineHeight = 0.0F;
    float m_capHeight = 0.7F;
    float m_ascent = 0.0F;
    float m_descent = 0.0F;
};

/// Decodes one code point from `text` at `i` and advances `i`. Invalid
/// sequences give U+FFFD.
char32_t decodeUtf8(std::string_view text, std::size_t& i);

/// Fonts by face name through fontmap.xml ("horizon_e" to "E", file
/// `<target>_vector_aa.dt`, or `<target>ru_vector_aa.dt` for Russian).
class FontLibrary {
public:
    FontLibrary(const Vfs& vfs, std::string language);

    /// Unknown names use font A, the game's default; nullptr only when no font
    /// loads.
    const VectorFont* font(std::string_view faceName);
    /// Every target the map lists, for tools.
    std::vector<std::string> targets() const;
    /// The file a target loads from.
    std::string fileFor(std::string_view target) const;

private:
    const VectorFont* load(const std::string& target);

    const Vfs& m_vfs;
    std::string m_language;
    std::unordered_map<std::string, std::string> m_faceToTarget;
    std::unordered_map<std::string, std::unique_ptr<VectorFont>> m_fonts;
};

} // namespace openhorizon::ui
