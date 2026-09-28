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

namespace fh {
class Vfs;
}

namespace fh::ui {

/// One corner of a glyph triangle. Glyphs are meshes covering their box
/// (with a margin) whose corners carry curve coordinates: a point is inside
/// the glyph where u*u - v < 0, with the test reversed on triangles marked
/// `outside` (the region between the outline and the box). Straight edges
/// have u = 0, so the value falls linearly to 0 at the outline; curved edges
/// use the quadratic Loop-Blinn coordinates (-1,1), (0,-1), (1,1). Scaling
/// the value by its screen-space gradient gives an anti-aliased edge at any
/// size, which is how the game draws its "Vector AA" fonts.
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
    /// Triangle list, three vertices per triangle.
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
///       geometry header: u32 unknown, u32 glyph records offset, u32 0,
///         u32 1, u32 0, u32 vertex bytes, u32 vertex count, u32 index
///         count, f32 space advance (ems), f32 build size (14), f32 line
///         height (ems), u32 8, 8 x s32 metrics in font units
///         (ascent, descent, ...), u32
///       glyph records, 40 bytes: u32 code point, f32 advance, u32
///         0xFFFF0000, u32 vertex byte offset, u32 vertex count, u32 first
///         index, u32 triangle count, f32 x offset (unused: vertices are
///         already placed), f32 0, f32 1
///     .gpu: vertices of four halfs {x, y, u, v}, where the sign of x marks
///       an outside triangle and |x| is the position, then u16 indices
///       (relative to the glyph's first vertex)
///
/// Fonts have no kerning pairs ("num kerns 0" in the compiler logs).
class VectorFont {
public:
    /// Throws DataError if the data is not a vfont.
    static VectorFont parse(ByteView data, const std::string& source);

    const Glyph* glyph(char32_t codePoint) const;
    /// The glyph used for characters the font lacks: U+25A1, then '?'.
    const Glyph* fallback() const;
    const std::map<char32_t, Glyph>& glyphs() const { return m_glyphs; }

    float spaceAdvance() const { return m_spaceAdvance; }
    float lineHeight() const { return m_lineHeight; }
    /// Height of capitals above the baseline, in ems (from 'H').
    float capHeight() const { return m_capHeight; }
    float ascent() const { return m_ascent; }
    float descent() const { return m_descent; }

    /// Advance of `utf8` in ems, with `trackingEms` added between glyphs.
    float measure(std::string_view utf8, float trackingEms = 0.0F) const;

private:
    std::map<char32_t, Glyph> m_glyphs;
    char32_t m_defaultChar = 0;
    float m_spaceAdvance = 0.25F;
    float m_lineHeight = 1.0F;
    float m_capHeight = 0.7F;
    float m_ascent = 0.75F;
    float m_descent = 0.25F;
};

/// Decodes one code point from `text` at `i` and advances `i`. Invalid
/// sequences give U+FFFD.
char32_t decodeUtf8(std::string_view text, std::size_t& i);

/// The fonts of `media/ui/fonts`, found through fontmap.xml: a scene names a
/// face such as "horizon_e" or "DG2_lcd", the map gives a target ("E") and
/// the file is `<target>_vector_aa.dt`, or `<target>ru_vector_aa.dt` for
/// Russian. Fonts load on first use.
class FontLibrary {
public:
    FontLibrary(const Vfs& vfs, std::string language);

    /// The font for a face name; unknown names use font A (the game's
    /// default). Returns nullptr only if no font can be loaded at all.
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

} // namespace fh::ui
