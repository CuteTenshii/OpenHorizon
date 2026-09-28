#include "VectorFont.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fh::ui {

namespace {

constexpr std::size_t kSectionTable = 0x50;
constexpr std::size_t kSectionEntrySize = 0x28;
constexpr std::size_t kGlyphRecordSize = 40;

struct CaffSection {
    std::string name;
    std::size_t offset = 0;
    std::size_t size = 0;
};

std::vector<CaffSection> readSections(ByteView data, const std::string& source)
{
    BeReader in(data, source);
    if (in.fixedString(4) != "CAFF") {
        in.fail("missing CAFF signature");
    }
    in.seek(0x38);
    const std::uint32_t count = in.u32();
    in.seek(0x48);
    std::size_t offset = in.u32();
    if (count == 0 || count > 16) {
        in.fail(std::format("implausible section count {}", count));
    }
    std::vector<CaffSection> sections;
    for (std::uint32_t i = 0; i < count; ++i) {
        in.seek(kSectionTable + i * kSectionEntrySize);
        CaffSection section;
        section.name = in.fixedString(8);
        in.skip(8);
        section.size = in.u32();
        section.offset = offset;
        if (section.offset + section.size > data.size()) {
            in.fail(std::format(
                "section {} (0x{:x} + 0x{:x}) runs past the end", section.name, section.offset, section.size));
        }
        offset += section.size;
        sections.push_back(section);
    }
    return sections;
}

const CaffSection& findSection(
    const std::vector<CaffSection>& sections, std::string_view name, const std::string& source)
{
    for (const CaffSection& section : sections) {
        if (section.name == name) {
            return section;
        }
    }
    throw DataError("{}: no {} section", source, name);
}

} // namespace

char32_t decodeUtf8(std::string_view text, std::size_t& i)
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = 0;
    char32_t value = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        value = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        value = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        value = c & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(extra) >= text.size()) {
        i = text.size();
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; ++k) {
        const unsigned char next = byte(i + static_cast<std::size_t>(k));
        if ((next & 0xC0) != 0x80) {
            i += static_cast<std::size_t>(k);
            return 0xFFFD;
        }
        value = (value << 6) | (next & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return value;
}

VectorFont VectorFont::parse(ByteView data, const std::string& source)
{
    const std::vector<CaffSection> sections = readSections(data, source);
    const CaffSection& dataSection = findSection(sections, ".data", source);
    const CaffSection& gpuSection = findSection(sections, ".gpu", source);
    BeReader in(data.subspan(dataSection.offset, dataSection.size), source + " .data");
    if (in.fixedString(6) != "vfont") {
        in.fail("not a vfont");
    }
    in.seek(0x18);
    const std::uint32_t charsetOffset = in.u32();
    const std::uint32_t geometryOffset = in.u32();

    VectorFont font;
    in.seek(charsetOffset + 0x0C);
    const std::uint32_t glyphCount = in.u32();
    font.m_defaultChar = in.u32();

    in.seek(geometryOffset + 4);
    const std::uint32_t recordsOffset = in.u32();
    in.skip(12);
    const std::uint32_t vertexBytes = in.u32();
    const std::uint32_t vertexCount = in.u32();
    const std::uint32_t indexCount = in.u32();
    font.m_spaceAdvance = in.f32();
    in.skip(4);
    font.m_lineHeight = in.f32();
    in.skip(4);
    const float unitsAscent = static_cast<float>(in.s32());
    const float unitsDescent = static_cast<float>(in.s32());
    if (vertexBytes != vertexCount * 8 || vertexBytes + indexCount * 2 > gpuSection.size) {
        in.fail(std::format("geometry of {} vertices and {} indices does not fit the {} byte .gpu section", vertexCount,
            indexCount, gpuSection.size));
    }
    if (unitsAscent > 0.0F && unitsDescent > 0.0F) {
        font.m_ascent = font.m_lineHeight * unitsAscent / (unitsAscent + unitsDescent);
        font.m_descent = font.m_lineHeight - font.m_ascent;
    }

    const std::uint8_t* gpu = data.data() + gpuSection.offset;
    const std::uint8_t* indices = gpu + vertexBytes;
    for (std::uint32_t g = 0; g < glyphCount; ++g) {
        in.seek(recordsOffset + g * kGlyphRecordSize);
        Glyph glyph;
        glyph.codePoint = in.u32();
        glyph.advance = in.f32();
        in.skip(4);
        const std::uint32_t vertexOffset = in.u32();
        const std::uint32_t count = in.u32();
        const std::uint32_t firstIndex = in.u32();
        const std::uint32_t triangles = in.u32();
        if (vertexOffset % 8 != 0 || vertexOffset + count * 8 > vertexBytes
            || (firstIndex + triangles * 3) > indexCount) {
            in.fail(std::format(
                "glyph U+{:04X} geometry lies outside the buffers", static_cast<std::uint32_t>(glyph.codePoint)));
        }
        const std::uint8_t* vertices = gpu + vertexOffset;
        glyph.inkMin = glm::vec2(std::numeric_limits<float>::max());
        glyph.inkMax = glm::vec2(std::numeric_limits<float>::lowest());
        glyph.triangles.reserve(triangles * 3);
        for (std::uint32_t t = 0; t < triangles; ++t) {
            GlyphVertex corners[3];
            int outsideVotes = 0;
            int insideVotes = 0;
            for (int c = 0; c < 3; ++c) {
                const std::uint16_t index
                    = loadBe16(indices + (firstIndex + t * 3 + static_cast<std::uint32_t>(c)) * 2);
                if (index >= count) {
                    in.fail(std::format(
                        "glyph U+{:04X} index {} of {}", static_cast<std::uint32_t>(glyph.codePoint), index, count));
                }
                const std::uint8_t* v = vertices + index * 8;
                const std::uint16_t rawX = loadBe16(v);
                const float x = halfToFloat(rawX);
                GlyphVertex& corner = corners[c];
                corner.position = {std::fabs(x), halfToFloat(loadBe16(v + 2))};
                corner.curve = {halfToFloat(loadBe16(v + 4)), halfToFloat(loadBe16(v + 6))};
                if ((rawX & 0x7FFFU) != 0) {
                    ((rawX & 0x8000U) ? outsideVotes : insideVotes) += 1;
                }
                if (std::fabs(corner.curve.x * corner.curve.x - corner.curve.y) < 1.0e-3F) {
                    glyph.inkMin = glm::min(glyph.inkMin, corner.position);
                    glyph.inkMax = glm::max(glyph.inkMax, corner.position);
                }
            }
            // A corner at x = 0 carries no sign; the others decide.
            const bool outside = outsideVotes > insideVotes;
            for (GlyphVertex& corner : corners) {
                corner.outside = outside;
                glyph.triangles.push_back(corner);
            }
        }
        if (glyph.inkMin.x > glyph.inkMax.x) {
            glyph.inkMin = glyph.inkMax = glm::vec2(0.0F);
        }
        font.m_glyphs.emplace(glyph.codePoint, std::move(glyph));
    }
    if (const Glyph* h = font.glyph(U'H'); h && h->inkMax.y > 0.0F) {
        font.m_capHeight = h->inkMax.y;
    } else if (const Glyph* zero = font.glyph(U'0'); zero && zero->inkMax.y > 0.0F) {
        font.m_capHeight = zero->inkMax.y;
    }
    return font;
}

const Glyph* VectorFont::glyph(char32_t codePoint) const
{
    const auto it = m_glyphs.find(codePoint);
    return it == m_glyphs.end() ? nullptr : &it->second;
}

const Glyph* VectorFont::fallback() const
{
    if (const Glyph* g = glyph(m_defaultChar)) {
        return g;
    }
    if (const Glyph* g = glyph(0x25A1)) {
        return g;
    }
    return glyph(U'?');
}

float VectorFont::measure(std::string_view utf8, float trackingEms) const
{
    float width = 0.0F;
    std::size_t i = 0;
    int count = 0;
    while (i < utf8.size()) {
        const char32_t c = decodeUtf8(utf8, i);
        if (c == U' ' || c == 0xA0) {
            width += m_spaceAdvance;
        } else if (const Glyph* g = glyph(c)) {
            width += g->advance;
        } else if (const Glyph* f = fallback()) {
            width += f->advance;
        }
        ++count;
    }
    if (count > 1) {
        width += trackingEms * static_cast<float>(count - 1);
    }
    return width;
}

FontLibrary::FontLibrary(const Vfs& vfs, std::string language)
    : m_vfs(vfs)
    , m_language(std::move(language))
{
    const std::optional<std::string> xml = vfs.readText("media/ui/fonts/fontmap.xml");
    if (!xml) {
        logWarning("ui: media/ui/fonts/fontmap.xml is missing; every face uses font A");
        return;
    }
    pugi::xml_document document;
    if (!document.load_buffer(xml->data(), xml->size())) {
        logWarning("ui: fontmap.xml does not parse; every face uses font A");
        return;
    }
    for (const pugi::xml_node mapping : document.child("fontmap").children("mapping")) {
        const std::string face = toLower(mapping.attribute("fontname").as_string());
        const std::string target = mapping.attribute("target").as_string();
        if (!face.empty() && !target.empty()) {
            m_faceToTarget[face] = target;
        }
    }
}

std::vector<std::string> FontLibrary::targets() const
{
    std::vector<std::string> out;
    for (const auto& [face, target] : m_faceToTarget) {
        if (std::find(out.begin(), out.end(), target) == out.end()) {
            out.push_back(target);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string FontLibrary::fileFor(std::string_view target) const
{
    const std::string lower = toLower(target);
    if (m_language == "ru") {
        const std::string russian = "media/ui/fonts/" + lower + "ru_vector_aa.dt";
        if (m_vfs.exists(russian)) {
            return russian;
        }
    }
    return "media/ui/fonts/" + lower + "_vector_aa.dt";
}

const VectorFont* FontLibrary::font(std::string_view faceName)
{
    const std::string face = toLower(faceName);
    const auto mapped = m_faceToTarget.find(face);
    const std::string target = mapped != m_faceToTarget.end() ? mapped->second : "A";
    if (const VectorFont* found = load(target)) {
        return found;
    }
    return target == "A" ? nullptr : load("A");
}

const VectorFont* FontLibrary::load(const std::string& target)
{
    const auto it = m_fonts.find(target);
    if (it != m_fonts.end()) {
        return it->second.get();
    }
    std::unique_ptr<VectorFont> loaded;
    const std::string path = fileFor(target);
    try {
        if (const std::optional<Bytes> data = m_vfs.read(path)) {
            loaded = std::make_unique<VectorFont>(VectorFont::parse(*data, path));
        } else {
            logWarning("ui: font {} is missing", path);
        }
    } catch (const DataError& e) {
        logWarning("ui: {}", e.what());
    }
    const VectorFont* result = loaded.get();
    m_fonts.emplace(target, std::move(loaded));
    return result;
}

} // namespace fh::ui
