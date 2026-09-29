// Tests of the UI layer: the Anark scene readers (presentation, scene graph
// and render data), the vector font reader and the scene runtime on data
// built here in the documented layouts, and, when FH1_GAME_DIR names an
// extracted disc, every scene, digital gauge and font on it plus the HUD
// scene's structure. Nothing here needs an OpenGL context.

#include "TestHarness.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "ui/AnarkFormats.h"
#include "ui/Minimap.h"
#include "ui/Presentation.h"
#include "ui/VectorFont.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fh::Bytes;
using namespace fh::ui;

/// Builds big-endian test data, the byte order of every file read here.
struct Writer {
    Bytes data;

    void u8(std::uint32_t v) { data.push_back(static_cast<std::uint8_t>(v)); }
    void u16(std::uint32_t v)
    {
        u8(v >> 8);
        u8(v & 0xFFU);
    }
    void u32(std::uint32_t v)
    {
        u16(v >> 16);
        u16(v & 0xFFFFU);
    }
    void s32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void bytes(std::string_view text) { data.insert(data.end(), text.begin(), text.end()); }
    void sized(std::string_view text)
    {
        u32(static_cast<std::uint32_t>(text.size()));
        bytes(text);
    }
    void zeros(std::size_t count) { data.insert(data.end(), count, 0); }
    void padTo(std::size_t size) { data.resize(std::max(data.size(), size), 0); }
    std::size_t size() const { return data.size(); }
    void patch32(std::size_t at, std::uint32_t v)
    {
        data[at] = static_cast<std::uint8_t>(v >> 24);
        data[at + 1] = static_cast<std::uint8_t>(v >> 16);
        data[at + 2] = static_cast<std::uint8_t>(v >> 8);
        data[at + 3] = static_cast<std::uint8_t>(v);
    }
    /// Writes a u32 placeholder for a section size and returns where.
    std::size_t beginSection()
    {
        const std::size_t at = size();
        u32(0);
        return at;
    }
    void endSection(std::size_t at) { patch32(at, static_cast<std::uint32_t>(size() - at - 4)); }
};

/// Half-float bits of the few values the font tests use.
std::uint16_t half(float value)
{
    if (value == 0.0F) {
        return 0;
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFU) - 127 + 15;
    const std::uint32_t mantissa = (bits >> 13) & 0x3FFU;
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | mantissa);
}

TEST(hashes_match_the_game)
{
    // Values the game's files carry for these names.
    CHECK_EQ(anarkHash("textstring"), 0x1124062U);
    CHECK_EQ(anarkHash("scale.x"), 0x465440cU);
    CHECK_EQ(anarkHash("position.x"), 0x27c230dU);
    CHECK_EQ(anarkHash("Scene"), 0x68c6274U);
    CHECK_EQ(stringIdHash("IDS_Place"), 23058);
    CHECK_EQ(stringIdHash("ids_place"), 23058);
    CHECK_EQ(stringIdHash("IDS_Race_Starts_In"), 63494);
}

TEST(track_evaluates_bezier_keys)
{
    Track track;
    track.keys = {{0.0F, 0.0F, 100.0F / 3.0F, 200.0F / 3.0F}, {1000.0F, 100.0F, -1.0F, -1.0F}};
    CHECK_NEAR(track.evaluate(-5.0F), 0.0, 1e-4);
    CHECK_NEAR(track.evaluate(250.0F), 25.0, 1e-3);
    CHECK_NEAR(track.evaluate(500.0F), 50.0, 1e-3);
    CHECK_NEAR(track.evaluate(2000.0F), 100.0, 1e-4);
}

TEST(uv_transform_matches_exported_matrices)
{
    // DIRTTILE01 in the HUD: the exporter stored u' = 0.6u - 16.8 and
    // v' = -0.4v + 2.2 next to these attributes.
    UvTransform dirt;
    dirt.position = {-28.0F, -3.0F};
    dirt.scale = {0.6F, 0.4F};
    glm::vec3 row0;
    glm::vec3 row1;
    dirt.matrix(row0, row1);
    CHECK_NEAR(row0.x, 0.6, 1e-5);
    CHECK_NEAR(row0.y, 0.0, 1e-5);
    CHECK_NEAR(row0.z, -16.8, 1e-4);
    CHECK_NEAR(row1.x, 0.0, 1e-5);
    CHECK_NEAR(row1.y, -0.4, 1e-5);
    CHECK_NEAR(row1.z, 2.2, 1e-4);

    // POPULARITYBARFILL: rotated 269 degrees about the centre at half scale.
    UvTransform fill;
    fill.rotationDegrees = 269.0F;
    fill.scale = {0.5F, 0.5F};
    fill.pivot = {0.5F, 0.5F};
    fill.matrix(row0, row1);
    CHECK_NEAR(row0.x, -0.0087, 1e-3);
    CHECK_NEAR(row0.y, 0.5, 1e-3);
    CHECK_NEAR(row0.z, 0.5, 1e-3);
    CHECK_NEAR(row1.x, 0.5, 1e-3);
    CHECK_NEAR(row1.y, 0.0087, 1e-3);
    CHECK_NEAR(row1.z, 0.5, 1e-3);
}

TEST(texture_paths_follow_the_game)
{
    CHECK_EQ(texturePathForScene("GAME:\\MEDIA\\UI\\TEXTURES\\HORIZON\\HUD\\DIALS\\NEEDLE.TGA"),
        std::string("media/ui/textures/horizon/hud/dials/needle.xds"));
    CHECK_EQ(texturePathForScene("GAME:\\MEDIA\\UI\\TEXTURES\\HORIZON\\HUD\\DIALS\\TCS ON.TGA"),
        std::string("media/ui/textures/horizon/hud/dials/tcs on.xds"));
}

TEST(utf8_decoding)
{
    const std::string text = "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xFF";
    std::size_t i = 0;
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(text, i)), 0x41U);
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(text, i)), 0xE9U);
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(text, i)), 0x20ACU);
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(text, i)), 0x1F600U);
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(text, i)), 0xFFFDU);
    CHECK_EQ(i, text.size());
    const std::string cut = "\xE2\x82";
    i = 0;
    CHECK_EQ(static_cast<std::uint32_t>(decodeUtf8(cut, i)), 0xFFFDU);
    CHECK_EQ(i, cut.size());
}

/// A presentation of a root component (element 0, id 1) and a model
/// (element 1, id 2), with a slide "Move" animating the model's x from 0 to
/// 100 over a second.
Bytes buildPresentation()
{
    Writer w;
    w.u8(1);
    w.u8(4);
    w.u32(26);
    w.bytes("AnarkBGF");
    w.u32(3);
    w.u32(0);
    w.u16(1280);
    w.u16(720);
    w.u8(1);
    w.u32(6);
    w.u8(0);
    w.u32(100);
    for (int i = 0; i < 25; ++i) {
        w.u32(i == 0 ? 2U : (i == 6 ? 2U : 0U));
    }
    std::size_t section = w.beginSection();
    w.sized("hello");
    w.sized("world");
    w.endSection(section);

    section = w.beginSection();
    w.u32((7U << 27) | anarkHash("Scene"));
    w.s32(-1);
    w.u32(1);
    w.u8(7);
    w.u8(4);
    w.u8(2);
    w.u32(10000);
    w.u32(1);
    w.u32((1U << 27) | anarkHash("endtime"));
    w.u32(10000);
    w.u32((1U << 27) | anarkHash("Box"));
    w.s32(0);
    w.u32(2);
    w.u8(1);
    w.u8(1);
    w.u32(2);
    w.u32((3U << 27) | anarkHash("opacity"));
    w.f32(80.0F);
    w.u32((5U << 27) | anarkHash("textstring"));
    w.u32(1);
    w.endSection(section);

    w.u32(0);
    section = w.beginSection();
    w.u32(1);
    w.u32(anarkHash("position.x"));
    w.u8(1);
    w.u8(0);
    w.u16(2);
    for (const auto& [time, value] : {std::pair{0U, 0.0F}, std::pair{1000U, 100.0F}}) {
        w.u32(time);
        w.f32(value);
        w.u32(0);
        w.f32(value == 0.0F ? 100.0F / 3.0F : -1.0F);
        w.u32(0);
        w.f32(value == 0.0F ? 200.0F / 3.0F : -1.0F);
    }
    w.endSection(section);
    // Logic: the root owns a handler for "SHOW_BOX" that sets the box's
    // opacity (a contract-typed value) and goes to the slide "Move".
    section = w.beginSection();
    w.u32(0);
    w.u32(1);
    w.u32((20U << 27) | anarkHash("SHOW_BOX"));
    w.u32(2);
    for (const std::uint32_t v : {1U, 0U, (1U << 27) | anarkHash("COMMAND_SETPROPERTY"),
             (19U << 27) | anarkHash("opacity"), std::bit_cast<std::uint32_t>(50.0F)}) {
        w.u32(v);
    }
    for (const std::uint32_t v :
        {0U, 0U, (1U << 27) | anarkHash("COMMAND_GOTOSLIDENAME"), (5U << 27) | anarkHash("Move"), 0U}) {
        w.u32(v);
    }
    w.endSection(section);
    w.u32(0);

    section = w.beginSection();
    const auto slide = [&](std::string_view name, bool active, bool withTrack) {
        std::string padded(name);
        padded.resize(16, '\0');
        w.bytes(padded);
        w.u32(0);
        w.u8(9);
        w.u32(0);
        w.u32(1000);
        w.u32(2);
        w.u32(0);
        w.u8(active ? 1 : 0);
        w.u32(0);
        w.u32(0);
        w.u32(1);
        w.u8(active ? 1 : 0);
        w.u32(0);
        w.u32(withTrack ? 2 : 0);
        if (withTrack) {
            w.u8(0);
            w.u8(1);
            w.u16(0);
            // An action of the logic section, which the reader skips.
            w.u8(1);
            w.u8(1);
            w.u16(7);
        }
    };
    slide("Master Slide", false, false);
    slide("Still", true, false);
    slide("Move", true, true);
    w.endSection(section);
    return w.data;
}

TEST(bgf_reads_elements_tracks_and_slides)
{
    const Bytes data = buildPresentation();
    const BgfFile file = BgfFile::parse(data, "test.bgf");
    CHECK_EQ(file.width, 1280);
    CHECK_EQ(file.height, 720);
    CHECK_EQ(file.strings.size(), std::size_t{2});
    CHECK_EQ(file.strings[1], std::string("world"));
    CHECK_EQ(file.elements.size(), std::size_t{2});
    CHECK_EQ(file.elements[0].nameHash, anarkHash("Scene"));
    CHECK(file.elements[0].kind == ElementKind::Component);
    CHECK_EQ(file.elements[1].parent, 0);
    CHECK_EQ(file.elements[1].id, 2U);
    CHECK_EQ(file.elements[1].attributes.size(), std::size_t{2});
    CHECK_NEAR(file.elements[1].attributes[0].asFloat(), 80.0, 1e-6);
    CHECK_EQ(file.tracks.size(), std::size_t{1});
    CHECK_EQ(file.tracks[0].keys.size(), std::size_t{2});
    CHECK_EQ(file.slides.size(), std::size_t{3});
    CHECK_EQ(file.slides[2].name, std::string("Move"));
    CHECK_EQ(file.slides[2].entries[1].tracks.size(), std::size_t{1});
    CHECK_EQ(file.slides[2].playCount, 1);
    CHECK(file.slides[2].pingPong);
    CHECK(!file.slides[2].startsPaused);
    CHECK(!file.slides[2].nextAtEnd);
    CHECK_EQ(file.logic.size(), std::size_t{1});
    if (!file.logic.empty()) {
        CHECK_EQ(file.logic[0].handlers.size(), std::size_t{1});
        CHECK_EQ(file.logic[0].handlers[0].event, anarkHash("SHOW_BOX"));
        CHECK_EQ(file.logic[0].handlers[0].actions.size(), std::size_t{2});
        CHECK_EQ(file.logic[0].handlers[0].actions[0].command, command::SetProperty);
        CHECK_EQ(file.logic[0].handlers[0].actions[1].target, 0U);
    }

    Bytes truncated = data;
    truncated.resize(truncated.size() - 3);
    CHECK_THROWS(BgfFile::parse(truncated, "truncated.bgf"));
    Bytes wrongSignature = data;
    wrongSignature[6] = 'X';
    CHECK_THROWS(BgfFile::parse(wrongSignature, "signature.bgf"));
}

Bytes buildSceneGraph()
{
    Writer w;
    w.u8(1);
    w.u8(4);
    w.u16(0);
    w.u32(24);
    w.bytes("FGBkranA");
    for (const std::uint32_t v : {2U, 2U, 0U, 0U, 8U}) {
        w.u32(v);
    }
    w.u32(2);
    w.u32(0);
    w.u32(2 * 96);
    const auto node = [&](std::int32_t parent, std::uint32_t id, std::uint8_t type, glm::vec3 position) {
        w.s32(parent);
        w.u32(id);
        w.u8(type);
        w.zeros(3);
        for (const float v : {position.x, position.y, position.z, 0.0F, 0.0F, 0.5F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F,
                 -50.0F, -50.0F, 0.0F, 50.0F, 50.0F, 0.0F, 0.75F}) {
            w.f32(v);
        }
        w.u8(1);
        w.zeros(7);
    };
    node(-1, 1, 1, {0.0F, 0.0F, 0.0F});
    node(0, 2, 2, {10.0F, 20.0F, -5.0F});
    w.u32(0);
    return w.data;
}

TEST(bsg_reads_nodes)
{
    const BsgFile file = BsgFile::parse(buildSceneGraph(), "test.bsg");
    CHECK_EQ(file.nodes.size(), std::size_t{2});
    const BsgNode& model = file.nodes[1];
    CHECK_EQ(model.parent, 0);
    CHECK_EQ(model.id, 2U);
    CHECK(model.type == BsgNodeType::Model);
    CHECK_NEAR(model.position.y, 20.0, 1e-6);
    CHECK_NEAR(model.rotation.z, 0.5, 1e-6);
    CHECK_NEAR(model.boundsMax.x, 50.0, 1e-6);
    CHECK_NEAR(model.opacity, 0.75, 1e-6);
    CHECK_EQ(static_cast<int>(model.layer), 1);
    Bytes wrong = buildSceneGraph();
    wrong[8] = 'x';
    CHECK_THROWS(BsgFile::parse(wrong, "wrong.bsg"));
}

Bytes buildRenderData()
{
    Writer w;
    w.u32(1008);
    w.u32(5);
    const auto header = [&](std::uint32_t kind, std::uint32_t id, std::string_view name) {
        w.u32(kind);
        w.u32(id);
        w.sized(name);
    };
    header(0, 1, "Root");
    header(1, 2, "TEXT_PLACE");
    w.u32(0);
    w.sized("");
    w.sized("InGame:IDS_Place");
    w.f32(0.25F);
    w.f32(0.25F);
    w.sized("horizon_e");
    for (const std::uint32_t v : {48U, 2U, 1U, 2U, 0U}) {
        w.u32(v);
    }
    w.f32(-1.0F);
    w.f32(3.0F);
    w.u32(0);
    w.u8(1);
    w.f32(47.0F);
    w.u8(1);
    w.zeros(59);
    header(4, 3, "Box");
    w.u32(0);
    w.f32(1.0F);
    header(5, 4, "Material1");
    for (const float v : {1.0F, 0.0F, 0.5F, 1.0F}) {
        w.f32(v);
    }
    w.zeros(13 * 4);
    for (const std::uint32_t v : {3U, 0U, 2U, 0U, 4U, 6U, 1U, 0U, 2U}) {
        w.u32(v);
    }
    header(7, 5, "TEST.TGA");
    w.u32(4);
    w.sized("GAME:\\MEDIA\\UI\\TEXTURES\\TEST.TGA");
    w.zeros(64);
    for (const float v : {0.0F, -28.0F, -3.0F, 0.6F, 0.4F, 0.0F, 0.0F}) {
        w.f32(v);
    }
    for (const std::uint32_t v : {0U, 4U, 2U, 1U, 0U, 2U}) {
        w.u32(v);
    }

    // One double-sided rectangle: four vertices and their back copies.
    constexpr std::uint32_t kVertexBytes = 8 * 32;
    constexpr std::uint32_t kIndexBytes = 12 * 2;
    w.u32(1);
    w.u32(36);
    w.u32(kVertexBytes + kIndexBytes);
    const std::size_t descriptor = w.size();
    w.zeros(96);
    const auto put = [&](std::size_t at, std::uint32_t v) { w.patch32(at, v); };
    const auto putFloat = [&](std::size_t at, float v) { w.patch32(at, std::bit_cast<std::uint32_t>(v)); };
    putFloat(descriptor + 16, -50.0F);
    putFloat(descriptor + 20, -50.0F);
    putFloat(descriptor + 32, 50.0F);
    putFloat(descriptor + 36, 50.0F);
    put(descriptor + 48, 1);
    put(descriptor + 72, 0 | 3);
    put(descriptor + 76, ((kVertexBytes / 4) << 2) | 2);
    put(descriptor + 80, 0);
    const std::size_t group = w.size();
    w.zeros(36);
    put(group + 28, kVertexBytes);
    put(group + 32, kIndexBytes);
    const glm::vec2 corners[4] = {{-50.0F, -50.0F}, {50.0F, -50.0F}, {50.0F, 50.0F}, {-50.0F, 50.0F}};
    for (int side = 0; side < 2; ++side) {
        for (const glm::vec2& c : corners) {
            for (const float v :
                {c.x, c.y, 0.0F, 0.0F, 0.0F, side == 0 ? 1.0F : -1.0F, c.x > 0 ? 1.0F : 0.0F, c.y > 0 ? 1.0F : 0.0F}) {
                w.f32(v);
            }
        }
    }
    for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U, 4U, 6U, 5U, 4U, 7U, 6U}) {
        w.u16(index);
    }
    return w.data;
}

TEST(fbf_reads_records_and_meshes)
{
    const FbfFile file = FbfFile::parse(buildRenderData(), "test.fbf");
    CHECK_EQ(file.version, 1008U);
    CHECK_EQ(file.records.size(), std::size_t{5});
    const FbfRecord& text = file.records[1];
    CHECK(text.kind == FbfKind::Text);
    CHECK_EQ(text.text.stringId, std::string("InGame:IDS_Place"));
    CHECK_EQ(text.text.font, std::string("horizon_e"));
    CHECK_EQ(text.text.size, 48);
    CHECK_EQ(text.text.horizontalAlign, 2);
    CHECK_NEAR(text.text.tracking, -1.0, 1e-6);
    CHECK_NEAR(text.text.authoredWidth, 47.0, 1e-6);
    const FbfRecord& material = file.records[3];
    CHECK(material.kind == FbfKind::Material);
    CHECK_NEAR(material.material.diffuse.b, 0.5, 1e-6);
    CHECK(material.material.sourceBlend == BlendFactor::SourceAlpha);
    CHECK(material.material.destinationBlend == BlendFactor::One);
    const FbfRecord& image = file.records[4];
    CHECK(image.kind == FbfKind::Image);
    CHECK_EQ(image.image.material, 4U);
    CHECK_NEAR(image.image.uv.position.x, -28.0, 1e-6);
    CHECK(image.image.wrapU == TextureWrap::Repeat);
    CHECK(image.image.wrapV == TextureWrap::Clamp);
    CHECK_EQ(file.meshes.size(), std::size_t{1});
    const Mesh& mesh = file.meshes[0];
    CHECK_EQ(mesh.vertices.size(), std::size_t{8});
    // The back face repeats the front and is dropped.
    CHECK_EQ(mesh.indices.size(), std::size_t{6});
    CHECK_NEAR(mesh.boundsMin.x, -50.0, 1e-6);
    CHECK_NEAR(mesh.vertices[2].uv.y, 1.0, 1e-6);

    Bytes badIndex = buildRenderData();
    badIndex[badIndex.size() - 1] = 40;
    CHECK_THROWS(FbfFile::parse(badIndex, "index.fbf"));
    Bytes badKind = buildRenderData();
    badKind[11] = 42;
    CHECK_THROWS(FbfFile::parse(badKind, "kind.fbf"));
}

/// A vfont with 'H' (one inside triangle) and 'I' (one outside triangle).
Bytes buildFont()
{
    Writer w;
    w.bytes("CAFF21.11.05.0034");
    w.padTo(0x38);
    w.u32(2);
    w.padTo(0x48);
    w.u32(0x100);
    w.padTo(0x50);
    constexpr std::uint32_t kDataSize = 0x200;
    constexpr std::uint32_t kGpuSize = 6 * 8 + 6 * 2;
    w.bytes(".data");
    w.padTo(0x60);
    w.u32(kDataSize);
    w.padTo(0x78);
    w.bytes(".gpu");
    w.padTo(0x88);
    w.u32(kGpuSize);
    w.padTo(0x100);

    const std::size_t data = w.size();
    w.bytes("vfont");
    w.u8(0);
    w.bytes("12.07.06.0035");
    w.padTo(data + 0x18);
    w.u32(0x40);
    w.u32(0x80);
    w.padTo(data + 0x40 + 0x0C);
    w.u32(2);
    w.u32(0x25A1);
    w.padTo(data + 0x80);
    w.u32(0);
    w.u32(0x100);
    w.zeros(12);
    w.u32(6 * 8);
    w.u32(6);
    w.u32(6);
    w.f32(0.25F);
    w.f32(14.0F);
    w.f32(1.0F);
    w.u32(8);
    w.s32(750);
    w.s32(250);
    w.padTo(data + 0x100);
    for (std::uint32_t g = 0; g < 2; ++g) {
        w.u32(g == 0 ? 'H' : 'I');
        w.f32(g == 0 ? 0.6F : 0.3F);
        w.u32(0xFFFF0000U);
        w.u32(g * 3 * 8);
        w.u32(3);
        w.u32(g * 3);
        w.u32(1);
        w.f32(g == 0 ? 0.0F : -0.125F);
        w.f32(0.0F);
        w.f32(1.0F);
    }
    w.padTo(data + kDataSize);

    const auto vertex = [&](float x, float y, float u, float v) {
        w.u16(half(x));
        w.u16(half(y));
        w.u16(half(u));
        w.u16(half(v));
    };
    vertex(0.0F, 0.0F, 0.0F, 0.0F);
    vertex(0.25F, 0.75F, 0.0F, 0.0F);
    vertex(0.5F, 0.0F, 0.0F, 1.0F);
    vertex(-0.25F, 0.0F, 0.0F, 1.0F);
    vertex(-0.5F, 0.5F, 0.0F, 1.0F);
    vertex(-0.75F, 0.0F, 0.0F, 1.0F);
    for (const std::uint32_t index : {0U, 1U, 2U, 0U, 1U, 2U}) {
        w.u16(index);
    }
    return w.data;
}

TEST(vector_font_reads_glyphs)
{
    const VectorFont font = VectorFont::parse(buildFont(), "test.dt");
    CHECK_EQ(font.glyphs().size(), std::size_t{2});
    const Glyph* h = font.glyph(U'H');
    CHECK(h != nullptr);
    if (h) {
        CHECK_NEAR(h->advance, 0.6, 1e-6);
        CHECK_EQ(h->triangles.size(), std::size_t{3});
        CHECK(!h->triangles[0].outside);
        CHECK_NEAR(h->triangles[2].curve.y, 1.0, 1e-3);
        CHECK_NEAR(h->inkMax.y, 0.75, 1e-3);
    }
    const Glyph* i = font.glyph(U'I');
    CHECK(i != nullptr);
    if (i) {
        CHECK(i->triangles[0].outside);
        // The record's x offset moves the mesh onto the pen.
        CHECK_NEAR(i->triangles[1].position.x, 0.5 - 0.125, 1e-3);
    }
    CHECK_NEAR(font.capHeight(), 0.75, 1e-3);
    CHECK_NEAR(font.ascent(), 0.75, 1e-4);
    CHECK_NEAR(font.spaceAdvance(), 0.25, 1e-6);
    CHECK_NEAR(font.measure("HI H", 0.1F), 0.6 + 0.3 + 0.25 + 0.6 + 0.3, 1e-5);
    CHECK(font.glyph(U'Z') == nullptr);

    Bytes broken = buildFont();
    broken[0x100] = 'x';
    CHECK_THROWS(VectorFont::parse(broken, "broken.dt"));
}

/// Scene files for the runtime tests: a root component with slides "Show"
/// (a label shown at half opacity) and "Move" (the box's x animated from 0
/// to 100 over a second, the label absent).
SceneFiles buildScene()
{
    SceneFiles files;
    BgfFile& bgf = files.bgf;
    bgf.width = 1280;
    bgf.height = 720;
    const auto element = [&](std::int32_t parent, std::uint32_t id, ElementKind kind) {
        BgfElement e;
        e.parent = parent;
        e.id = id;
        e.kind = kind;
        bgf.elements.push_back(e);
    };
    element(-1, 1, ElementKind::Component);
    element(0, 2, ElementKind::Node);
    element(0, 3, ElementKind::Text);
    element(1, 4, ElementKind::Material);
    bgf.strings = {"GO!"};
    Track track;
    track.element = 1;
    track.attribute = attr::PositionX;
    track.keys = {{0.0F, 0.0F, 100.0F / 3.0F, 200.0F / 3.0F}, {1000.0F, 100.0F, -1.0F, -1.0F}};
    bgf.tracks.push_back(track);
    const auto entry = [](std::uint32_t e, bool active) {
        SlideEntry out;
        out.element = e;
        out.active = active;
        return out;
    };
    Slide master;
    master.name = "Master Slide";
    master.durationMs = 1000;
    master.entries = {entry(0, false), entry(1, false), entry(3, false)};
    Slide show;
    show.name = "Show";
    show.durationMs = 1000;
    show.entries = {entry(0, true), entry(1, true), entry(2, true), entry(3, true)};
    Attribute half;
    half.hash = attr::Opacity;
    half.type = static_cast<std::uint8_t>(AttributeType::Float);
    half.raw = std::bit_cast<std::uint32_t>(50.0F);
    show.entries[2].attributes.push_back(half);
    Attribute text;
    text.hash = attr::TextString;
    text.type = static_cast<std::uint8_t>(AttributeType::String);
    text.raw = 0;
    show.entries[2].attributes.push_back(text);
    Slide move;
    move.name = "Move";
    move.durationMs = 1000;
    move.flags = 1;
    move.entries = {entry(0, true), entry(1, true), entry(3, true)};
    move.entries[1].tracks.push_back(0);
    bgf.slides = {master, show, move};

    FbfRecord root;
    root.kind = FbfKind::Group;
    root.id = 1;
    root.name = "Scene";
    FbfRecord box;
    box.kind = FbfKind::Model;
    box.id = 2;
    box.name = "Box";
    box.mesh = 0;
    FbfRecord label;
    label.kind = FbfKind::Text;
    label.id = 3;
    label.name = "Label";
    label.text.font = "horizon_b";
    label.text.size = 20;
    FbfRecord material;
    material.kind = FbfKind::Material;
    material.id = 4;
    material.name = "Material";
    material.material.diffuse = glm::vec4(1.0F, 0.5F, 0.25F, 1.0F);
    files.fbf.records = {root, box, label, material};
    Mesh quad;
    quad.vertices.resize(3);
    quad.indices = {0, 1, 2};
    files.fbf.meshes.push_back(quad);

    BsgNode boxNode;
    boxNode.parent = -1;
    boxNode.id = 2;
    boxNode.type = BsgNodeType::Model;
    boxNode.position = {10.0F, 20.0F, 0.0F};
    files.bsg.nodes.push_back(boxNode);
    return files;
}

const DrawItem* findItem(const std::vector<DrawItem>& items, bool text)
{
    for (const DrawItem& item : items) {
        if (item.isText == text) {
            return &item;
        }
    }
    return nullptr;
}

TEST(presentation_applies_slides_tracks_and_overrides)
{
    Presentation scene(buildScene());
    const int root = scene.find("");
    CHECK_EQ(root, 0);
    CHECK_EQ(scene.find("Box"), 1);
    CHECK_EQ(scene.find("Label"), 2);
    CHECK_EQ(scene.find("Missing"), -1);
    CHECK_EQ(scene.currentSlide(root), std::string("Show"));
    CHECK_EQ(scene.slideNames(root).size(), std::size_t{3});

    std::vector<DrawItem> items = scene.evaluate();
    const DrawItem* label = findItem(items, true);
    CHECK(label != nullptr);
    if (label) {
        CHECK_EQ(label->text.text, std::string("GO!"));
        CHECK_NEAR(label->text.color.a, 0.5, 1e-5);
    }
    const DrawItem* box = findItem(items, false);
    CHECK(box != nullptr);
    if (box) {
        CHECK_NEAR(box->model.world[3].x, 10.0, 1e-5);
        CHECK_NEAR(box->model.color.g, 0.5, 1e-3);
    }

    CHECK(scene.goToSlide(root, "Move"));
    CHECK(!scene.goToSlide(root, "Master Slide"));
    scene.update(0.25F);
    items = scene.evaluate();
    CHECK(findItem(items, true) == nullptr);
    box = findItem(items, false);
    CHECK(box != nullptr);
    if (box) {
        CHECK_NEAR(box->model.world[3].x, 25.0, 1e-3);
        CHECK_NEAR(box->model.world[3].y, 20.0, 1e-5);
    }
    scene.seek(root, 750.0F);
    scene.update(1.0F);
    CHECK_NEAR(scene.time(root), 750.0, 1e-3);
    CHECK_NEAR(scene.trackValue(1, attr::PositionX, 500.0F).value_or(-1.0F), 50.0, 1e-3);

    scene.setAttribute(1, attr::PositionY, -7.0F);
    scene.setText(2, "override");
    CHECK_EQ(scene.text(2), std::string("override"));
    items = scene.evaluate();
    box = findItem(items, false);
    CHECK(box != nullptr);
    if (box) {
        CHECK_NEAR(box->model.world[3].y, -7.0, 1e-5);
    }
    scene.setHidden(1, true);
    CHECK(findItem(scene.evaluate(), false) == nullptr);
    CHECK(!scene.present(1));
}

/// A scene whose root component has one slide per play mode, all 1000 ms
/// long, and a model below it.
SceneFiles buildTimelineScene()
{
    SceneFiles files;
    BgfElement root;
    root.nameHash = anarkHash("Scene");
    root.kind = ElementKind::Component;
    root.id = 1;
    BgfElement box;
    box.nameHash = anarkHash("Box");
    box.parent = 0;
    box.id = 2;
    // An event behaviour on the box that fires PING at its parent 500 ms
    // into "Once".
    BgfElement ping;
    ping.nameHash = anarkHash("Ping");
    ping.parent = 1;
    ping.id = 3;
    ping.kind = ElementKind::Behavior;
    files.bgf.strings = {"PING", "parent.parent"};
    files.bgf.elements = {root, box, ping};
    const auto slide = [&](std::string name, std::uint8_t flags) {
        Slide s;
        s.name = std::move(name);
        s.flags = flags;
        s.playCount = flags & 3;
        s.startsPaused = (flags & 4) != 0;
        s.pingPong = (flags & 8) != 0;
        s.nextAtEnd = (flags & 16) != 0;
        s.durationMs = 1000;
        SlideEntry entry;
        entry.element = 1;
        entry.active = true;
        s.entries.push_back(entry);
        files.bgf.slides.push_back(std::move(s));
    };
    slide("Master Slide", 9);
    slide("Once", 9);
    {
        SlideEntry entry;
        entry.element = 2;
        entry.active = true;
        entry.attributes.push_back({anarkHash("eventName"), 21, 0});
        entry.attributes.push_back({anarkHash("TARGET"), 21, 1});
        entry.attributes.push_back({anarkHash("timeoffset"), 1, 500});
        files.bgf.slides.back().entries.push_back(entry);
    }
    slide("Loop", 0);
    slide("PingPong", 8);
    slide("Paused", 13);
    slide("Next", 17);
    slide("After", 9);
    LogicOwner owner;
    owner.element = 0;
    LogicHandler handler;
    handler.event = anarkHash("GO_PAUSED");
    handler.actions.push_back({0, 0, command::GoToSlideName, anarkHash("Paused"), 0});
    handler.actions.push_back({0, 0, command::GoToTime, 600, 0});
    handler.actions.push_back(
        {1, 0, command::SetProperty, (19U << 27) | anarkHash("opacity"), std::bit_cast<std::uint32_t>(25.0F)});
    owner.handlers.push_back(handler);
    LogicHandler play;
    play.event = anarkHash("PLAY_IT");
    play.actions.push_back({0, 0, command::Play, 0, 0});
    owner.handlers.push_back(play);
    files.bgf.logic.push_back(owner);
    return files;
}

TEST(timelines_follow_the_play_flags)
{
    Presentation scene(buildTimelineScene());
    CHECK_EQ(scene.currentSlide(0), std::string("Once"));
    scene.update(0.4F);
    CHECK_NEAR(scene.time(0), 400.0, 1e-3);
    CHECK(scene.takeFiredEvents().empty());
    scene.update(0.2F);
    const std::vector<Presentation::FiredEvent> fired = scene.takeFiredEvents();
    CHECK_EQ(fired.size(), std::size_t{1});
    if (!fired.empty()) {
        CHECK_EQ(fired[0].target, 0);
        CHECK_EQ(fired[0].event, std::string("PING"));
    }
    scene.update(1.0F);
    CHECK(scene.takeFiredEvents().empty());
    CHECK_NEAR(scene.time(0), 1000.0, 1e-3);

    CHECK(scene.goToSlide(0, "Loop"));
    scene.update(1.25F);
    CHECK_NEAR(scene.time(0), 250.0, 1e-2);

    CHECK(scene.goToSlide(0, "PingPong"));
    scene.update(1.25F);
    CHECK_NEAR(scene.time(0), 750.0, 1e-2);
    scene.update(1.0F);
    CHECK_NEAR(scene.time(0), 250.0, 1e-2);

    CHECK(scene.goToSlide(0, "Paused"));
    scene.update(0.5F);
    CHECK_NEAR(scene.time(0), 0.0, 1e-6);
    scene.goToProgress(0, 0.3F);
    CHECK_NEAR(scene.time(0), 300.0, 1e-3);
    CHECK_NEAR(scene.progress(0), 0.3, 1e-4);
    scene.animateProgress(0, 0.7F, 1.0F, 1);
    scene.update(0.5F);
    CHECK_NEAR(scene.time(0), 500.0, 1.0);
    scene.update(0.6F);
    CHECK_NEAR(scene.time(0), 700.0, 1.0);
    scene.animateProgress(0, 1.0F, 1.0F, 4);
    scene.update(0.5F);
    // Half way through the ease-in-out: 0.7 + 0.3 * 0.5.
    CHECK_NEAR(scene.time(0), 850.0, 1.0);

    CHECK(scene.goToSlide(0, "Next"));
    scene.update(1.1F);
    CHECK_EQ(scene.currentSlide(0), std::string("After"));

    CHECK(scene.goToSlideIndex(0, 3));
    CHECK_EQ(scene.currentSlide(0), std::string("PingPong"));
    CHECK(!scene.goToSlideIndex(0, 99));

    scene.reset();
    CHECK_EQ(scene.currentSlide(0), std::string("Once"));
}

TEST(events_run_their_actions)
{
    Presentation scene(buildTimelineScene());
    CHECK(scene.fireEvent(0, "GO_PAUSED"));
    CHECK_EQ(scene.currentSlide(0), std::string("Paused"));
    CHECK_NEAR(scene.time(0), 600.0, 1e-3);
    CHECK_NEAR(scene.attribute(1, attr::Opacity).value_or(-1.0F), 25.0, 1e-5);
    scene.update(0.2F);
    CHECK_NEAR(scene.time(0), 600.0, 1e-3);
    CHECK(scene.fireEvent(0, "PLAY_IT"));
    scene.update(0.2F);
    CHECK_NEAR(scene.time(0), 800.0, 1e-2);
    CHECK(!scene.fireEvent(0, "UNKNOWN"));
    CHECK(!scene.fireEvent(1, "GO_PAUSED"));
}

Bytes buildRoadNetwork()
{
    Writer w;
    const std::vector<std::string> keys{"oneway", "road_type"};
    const std::vector<std::string> values{"a", "dirt", "true"};
    std::string keyText;
    std::vector<std::uint32_t> keyOffsets;
    for (const std::string& key : keys) {
        keyOffsets.push_back(static_cast<std::uint32_t>(keyText.size()));
        keyText += key + '\0';
    }
    std::string valueText;
    std::vector<std::uint32_t> valueOffsets;
    for (const std::string& value : values) {
        valueOffsets.push_back(static_cast<std::uint32_t>(valueText.size()));
        valueText += value + '\0';
    }
    for (const std::uint32_t v : {0x0E177551U, 3U, 2U, 3U, 4U, 4U, 2U, 3U, static_cast<std::uint32_t>(keyText.size()),
             static_cast<std::uint32_t>(valueText.size())}) {
        w.u32(v);
    }
    const std::array<glm::vec3, 3> nodes{
        glm::vec3(0.0F, 5.0F, 0.0F), glm::vec3(100.0F, 5.0F, 0.0F), glm::vec3(100.0F, 6.0F, 50.0F)};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        w.u32(static_cast<std::uint32_t>(i + 10));
        w.f32(nodes[i].x);
        w.f32(nodes[i].y);
        w.f32(nodes[i].z);
        w.zeros(16);
    }
    // Way 0: nodes 0, 1 as an A road, one-way; way 1: nodes 1, 2, dirt.
    for (const std::uint32_t v : {2U, 0U, 2U, 0U, 2U, 2U, 1U, 2U}) {
        w.u32(v);
    }
    for (const std::uint32_t v : {0U, 1U, 1U, 2U}) {
        w.u32(v);
    }
    w.zeros(4 * 8);
    for (const std::uint32_t v : {1U, 0U, 0U, 2U, 1U, 1U}) {
        w.u32(v);
    }
    for (const std::uint32_t v : keyOffsets) {
        w.u32(v);
    }
    for (const std::uint32_t v : valueOffsets) {
        w.u32(v);
    }
    w.bytes(keyText);
    w.bytes(valueText);
    w.u32(0x0E177551U);
    return w.data;
}

TEST(road_network_reads_ways_and_tags)
{
    const Bytes data = buildRoadNetwork();
    const NavFile nav = NavFile::parse(data, "test.nav");
    CHECK_EQ(nav.ways.size(), std::size_t{2});
    if (nav.ways.size() == 2) {
        CHECK_EQ(nav.ways[0].points.size(), std::size_t{2});
        CHECK_NEAR(nav.ways[0].points[1].x, 100.0, 1e-6);
        CHECK_EQ(std::string(nav.ways[0].tag("road_type")), std::string("a"));
        CHECK_EQ(std::string(nav.ways[0].tag("oneway")), std::string("true"));
        CHECK_EQ(std::string(nav.ways[1].tag("road_type")), std::string("dirt"));
        CHECK_NEAR(nav.ways[1].points[1].y, 50.0, 1e-6);
        CHECK(nav.ways[1].tag("oneway").empty());
    }
    Bytes truncated = data;
    truncated.resize(truncated.size() - 4);
    CHECK_THROWS(NavFile::parse(truncated, "truncated.nav"));
}

TEST(map_profile_reads_groups)
{
    const std::string xml = R"(<MapRenderProfiles>
  <Profile name="other"><Group name="x"><Filter tag="a" value="b"/></Group></Profile>
  <Profile name="minimap">
    <Group name="b roads">
      <Symbolizer type="line_fade_lr" sort_order="90"><Size value="14"/>
        <ColourPrimary r="0" g="0" b="0" a="128"/></Symbolizer>
      <Symbolizer type="icon" sort_order="600"><DynamicSize value="45"/>
        <Atlas x="2" y="3" x_slots="8" y_slots="8"/><Texture value="horizon\map\icons\sheet.tga"/>
        <PlacementRatio value="0.5"/></Symbolizer>
      <Filter tag="road_type" value="b"/>
    </Group>
  </Profile>
</MapRenderProfiles>)";
    const MapProfile profile = MapProfile::parse(xml, "test.xml", "minimap");
    CHECK_EQ(profile.groups.size(), std::size_t{1});
    if (!profile.groups.empty()) {
        const MapGroup& group = profile.groups[0];
        CHECK_EQ(group.filterTag, std::string("road_type"));
        CHECK_EQ(group.symbolizers.size(), std::size_t{2});
        CHECK_EQ(group.symbolizers[0].sortOrder, 90);
        CHECK_NEAR(group.symbolizers[0].size, 14.0, 1e-6);
        CHECK(!group.symbolizers[0].dynamicSize);
        CHECK_NEAR(group.symbolizers[0].primary.value_or(glm::vec4(0.0F)).a, 128.0, 1e-6);
        CHECK(group.symbolizers[1].dynamicSize);
        CHECK_EQ(group.symbolizers[1].atlasY, 3);
        CHECK_EQ(group.symbolizers[1].texture, std::string("horizon\\map\\icons\\sheet.tga"));
        CHECK_NEAR(group.symbolizers[1].placementRatio.value_or(-1.0F), 0.5, 1e-6);
        CHECK(!group.symbolizers[0].placementRatio);
    }
    CHECK_THROWS(MapProfile::parse(xml, "test.xml", "missing"));
}

TEST(career_event_styles_name_map_activities)
{
    CHECK_EQ(careerEventActivityType(1), std::string_view("race"));
    CHECK_EQ(careerEventActivityType(2), std::string_view("streetrace"));
    CHECK_EQ(careerEventActivityType(3), std::string_view("exhibition"));
    CHECK_EQ(careerEventActivityType(4), std::string_view("race"));
    CHECK_EQ(careerEventActivityType(5), std::string_view("nemesisrace"));
    CHECK_EQ(careerEventActivityType(6), std::string_view("race"));
    CHECK(careerEventActivityType(0).empty());
}

TEST(minimap_camera_follows_the_car)
{
    SatNavTuning tuning;
    tuning.carSpeedMin = 2.0F;
    tuning.carSpeedMax = 50.0F;
    tuning.distanceMin = 500.0F;
    tuning.distanceMax = 700.0F;
    tuning.elevationMinDegrees = 70.0F;
    tuning.elevationMaxDegrees = 70.0F;
    tuning.fovMinDegrees = 55.0F;
    tuning.fovMaxDegrees = 55.0F;
    tuning.playerViewportY = -0.3F;
    const glm::vec2 car(1000.0F, -200.0F);
    const auto project = [](const MinimapCamera& camera, glm::vec3 p) {
        const glm::vec3 d = p - camera.eye;
        const float depth = glm::dot(d, camera.forward);
        return glm::vec2(
            glm::dot(d, camera.right) * camera.scaleX / depth, glm::dot(d, camera.up) * camera.scaleY / depth);
    };
    for (const float heading : {0.0F, 1.0F, -2.5F}) {
        const MinimapCamera slow = MinimapCamera::follow(tuning, car, heading, 0.0F, 1.0F);
        CHECK_NEAR(slow.distance, 500.0, 1e-3);
        // The elevation is measured from straight down.
        CHECK_NEAR(slow.forward.y, -std::cos(glm::radians(70.0F)), 1e-5);
        const glm::vec2 onScreen = project(slow, glm::vec3(car.x, 0.0F, car.y));
        CHECK_NEAR(onScreen.x, 0.0, 1e-4);
        CHECK(onScreen.y < -0.2F && onScreen.y > -0.4F);
        // Heading up: ground ahead of the car is higher on screen.
        const glm::vec3 ahead(car.x + 100.0F * std::sin(heading), 0.0F, car.y + 100.0F * std::cos(heading));
        CHECK(project(slow, ahead).y > onScreen.y + 0.05F);
        CHECK_NEAR(project(slow, ahead).x, 0.0, 1e-4);
    }
    CHECK_NEAR(MinimapCamera::follow(tuning, car, 0.0F, 26.0F, 1.0F).distance, 600.0, 1e-3);
    CHECK_NEAR(MinimapCamera::follow(tuning, car, 0.0F, 90.0F, 1.0F).distance, 700.0, 1e-3);
}

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

TEST(disc_every_scene_and_gauge_parses)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    std::vector<std::string> scenes;
    for (const std::string& path : vfs.list("media")) {
        if (fh::endsWithNoCase(path, ".bgf")) {
            scenes.push_back(path.substr(0, path.size() - 4));
        }
    }
    CHECK(scenes.size() > 200);
    std::size_t parsed = 0;
    std::size_t gauges = 0;
    std::size_t totalNamed = 0;
    std::size_t totalMatching = 0;
    for (const std::string& path : scenes) {
        if (!vfs.exists(path + ".bsg") || !vfs.exists(path + ".fbf")) {
            continue;
        }
        try {
            const SceneFiles files = SceneFiles::load(vfs, path);
            // Names in the render data hash to the presentation's name hashes;
            // images (named after their file) and repeated names (which the
            // exporter renames) are the exceptions.
            std::size_t named = 0;
            std::size_t matching = 0;
            for (const BgfElement& element : files.bgf.elements) {
                if (element.kind == ElementKind::Behavior || element.kind == ElementKind::Image) {
                    continue;
                }
                for (const FbfRecord& record : files.fbf.records) {
                    if (record.id == element.id) {
                        ++named;
                        matching += anarkHash(record.name) == element.nameHash ? 1 : 0;
                        break;
                    }
                }
            }
            CHECK(named > 0);
            totalNamed += named;
            totalMatching += matching;
            ++parsed;
            gauges += path.find("digitalgauge") != std::string::npos ? 1 : 0;
        } catch (const fh::DataError& e) {
            fh::test::fail(__FILE__, __LINE__, e.what());
        }
    }
    std::fprintf(stderr, "  %zu scenes parsed, %zu of them digital gauges; %zu of %zu names match their hashes\n",
        parsed, gauges, totalMatching, totalNamed);
    CHECK(totalMatching * 10 > totalNamed * 9);
    CHECK(parsed > 200);
    CHECK(gauges > 50);
}

TEST(disc_uv_transform_rebuilds_every_stored_matrix)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    std::size_t images = 0;
    std::size_t matching = 0;
    std::size_t otherModes = 0;
    for (const std::string& path : vfs.list("media")) {
        if (!fh::endsWithNoCase(path, ".fbf")) {
            continue;
        }
        const std::optional<fh::Bytes> data = vfs.read(path);
        if (!data) {
            continue;
        }
        const FbfFile file = FbfFile::parse(*data, path);
        for (const FbfRecord& record : file.records) {
            if (record.kind != FbfKind::Image) {
                continue;
            }
            ++images;
            if (record.image.mappingMode != 0) {
                ++otherModes;
                CHECK(path.find("947_hud") == std::string::npos);
                continue;
            }
            glm::vec3 row0;
            glm::vec3 row1;
            record.image.uv.matrix(row0, row1);
            // The stored matrix maps (u, v, 0, 1) as a row vector:
            // u' = u m0 + v m4 + m12, v' = u m1 + v m5 + m13.
            const std::array<float, 16>& m = record.image.storedMatrix;
            const auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-3F * std::max(1.0F, std::fabs(b)); };
            const bool same = near(m[0], row0.x) && near(m[4], row0.y) && near(m[12], row0.z) && near(m[1], row1.x)
                && near(m[5], row1.y) && near(m[13], row1.z);
            if (same) {
                ++matching;
            } else {
                std::fprintf(stderr, "  %s %s: the stored matrix differs\n", path.c_str(), record.name.c_str());
            }
        }
    }
    std::fprintf(stderr, "  %zu of %zu images match; %zu use another mapping mode\n", matching, images, otherModes);
    CHECK(images > 1000);
    CHECK_EQ(matching + otherModes, images);
}

TEST(disc_fonts_cover_latin1)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    FontLibrary fonts(vfs, "en");
    for (const char* face : {"horizon_a", "horizon_b", "horizon_d", "horizon_e"}) {
        const VectorFont* font = fonts.font(face);
        CHECK(font != nullptr);
        if (!font) {
            continue;
        }
        CHECK_EQ(font->glyphs().size(), std::size_t{237});
        for (char32_t c = U'!'; c < 0x7F; ++c) {
            CHECK(font->glyph(c) != nullptr);
        }
        CHECK(font->glyph(0xE9) != nullptr);
        CHECK(font->glyph(0x20AC) != nullptr);
        CHECK(font->capHeight() > 0.5F && font->capHeight() < 1.0F);
        for (const auto& [code, glyph] : font->glyphs()) {
            CHECK(glyph.triangles.size() % 3 == 0);
        }
    }
    const VectorFont* digits = fonts.font("DG2_lcd");
    CHECK(digits != nullptr && digits->glyph(U'7') != nullptr);
}

// The render data keeps each text's width as the editor measured it. Our
// measure at 96/72 of the size must agree for SUPER_STACKER, which grows its
// bars by the difference, to leave the authored strings where they were.
TEST(disc_hud_authored_text_widths)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const Presentation hud(SceneFiles::load(vfs, "media/ui/scenes/ui4/947_HUD"));
    FontLibrary fonts(vfs, "en");
    std::vector<float> ratios;
    for (int element = 0; element < static_cast<int>(hud.elementCount()); ++element) {
        const FbfRecord* record = hud.record(element);
        const std::string shown = hud.text(element);
        if (!record || record->kind != FbfKind::Text || record->text.authoredWidth <= 0.0F || record->text.size <= 0
            || shown.find_first_not_of(' ') == std::string::npos) {
            continue;
        }
        const FbfText& text = record->text;
        const VectorFont* font = fonts.font(text.font);
        if (!font) {
            continue;
        }
        const auto size = static_cast<float>(text.size);
        const float measured = font->measure(shown, text.tracking / size) * size * (96.0F / 72.0F);
        ratios.push_back(measured / text.authoredWidth);
        if (shown == "ARTIST NAME" || shown == "RADIO OFF") {
            std::fprintf(stderr, "  \"%s\" %s %d: authored %.1f, measured %.1f\n", shown.c_str(), text.font.c_str(),
                text.size, static_cast<double>(text.authoredWidth), static_cast<double>(measured));
            CHECK_NEAR(measured, text.authoredWidth, 0.05 * text.authoredWidth);
        }
    }
    CHECK(ratios.size() > 20);
    if (ratios.empty()) {
        return;
    }
    std::ranges::sort(ratios);
    const float median = ratios[ratios.size() / 2];
    std::fprintf(stderr, "  %zu texts, measured over authored: median %.3f, from %.3f to %.3f\n", ratios.size(),
        static_cast<double>(median), static_cast<double>(ratios.front()), static_cast<double>(ratios.back()));
    CHECK_NEAR(median, 1.0, 0.05);
}

TEST(disc_hud_scene_structure)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    Presentation hud(SceneFiles::load(vfs, "media/ui/scenes/ui4/947_HUD"));
    CHECK_EQ(hud.files().bgf.width, 1280);
    const int root = hud.find("Layer/HUD");
    CHECK(root >= 0);
    for (const char* name : {"GAUGE_ANALOG", "TEXT_GEAR_ANALOG", "_P_Tach_Needle", "Redline", "EndConditionBlock",
             "PlaceBlock", "HUDTimes", "PositionIndicators", "ActivationPrompt", "MiniMap"}) {
        CHECK(hud.findBelow(root, name) >= 0);
    }
    const int countdown = hud.find("Layer/I_321_Go");
    CHECK(countdown >= 0);
    CHECK(hud.goToSlide(countdown, "Three"));
    CHECK_EQ(hud.text(hud.findBelow(countdown, "TEXT_COUNT")), std::string("3"));
    const int needle = hud.findBelow(root, "_P_Tach_Needle");
    const int arm = hud.findBelow(needle, "Group");
    // The tach sweeps 268 degrees: from +134 at 0 to -134 at the top of the scale.
    CHECK_NEAR(hud.trackValue(arm, attr::RotationZ, 0.0F).value_or(0.0F), 2.339, 1e-3);
    CHECK_NEAR(hud.trackValue(arm, attr::RotationZ, 1000.0F).value_or(0.0F), -2.339, 1e-3);
    const std::vector<DrawItem> items = hud.evaluate();
    CHECK(items.size() > 50);

    // The slides logic actions name exist on the components they target,
    // but for nine authoring leftovers (such as HIDE on ValueScale) that
    // the runtime can only ignore.
    std::size_t slideActions = 0;
    std::size_t slideMatches = 0;
    for (const LogicOwner& owner : hud.files().bgf.logic) {
        for (const LogicHandler& handler : owner.handlers) {
            for (const LogicAction& action : handler.actions) {
                if (action.command != command::GoToSlideName) {
                    continue;
                }
                ++slideActions;
                bool found = false;
                for (const std::string& name : hud.slideNames(static_cast<int>(action.target))) {
                    found = found || anarkHash(name) == (action.key & 0x07FFFFFFU);
                }
                slideMatches += found ? 1 : 0;
            }
        }
    }
    std::fprintf(stderr, "  %zu of %zu slide changes name a slide of their component\n", slideMatches, slideActions);
    CHECK(slideActions > 100);
    CHECK_EQ(slideActions - slideMatches, std::size_t{9});

    // Contract parameters resolve to the elements the game binds.
    const int hudContract = hud.contract(root, "HUD");
    CHECK(hudContract >= 0);
    CHECK_EQ(hud.contractParameter(hudContract, "TEXT_GEAR_ANALOG"), hud.findBelow(root, "TEXT_GEAR_ANALOG"));
    CHECK_EQ(hud.contractParameter(hudContract, "COMPONENT_SAT_NAV"), hud.findBelow(root, "MiniMap"));
    const int timers = hud.findBelow(root, "HUDTimes");
    const int timersContract = hud.contract(timers, "HUD_TIMERS");
    CHECK(timersContract >= 0);
    const int line1 = hud.contractParameter(timersContract, "TEXT_LINE_1");
    CHECK(line1 >= 0 && hud.name(line1) == "TEXT_VALUE_1");

    // Property sets reach the element.
    const int placeBlock = hud.findBelow(root, "PlaceBlock");
    CHECK(hud.fireEvent(hudContract, "HIDE_PLACE"));
    CHECK_NEAR(hud.attribute(placeBlock, attr::Opacity).value_or(-1.0F), 0.0, 1e-6);

    // The countdown's contract switches its slides by event.
    const int countdownContract = hud.contract(countdown, "HUD_321GO");
    CHECK(countdownContract >= 0);
    CHECK(hud.fireEvent(countdownContract, "COUNTDOWN_TWO"));
    CHECK_EQ(hud.currentSlide(countdown), std::string("Two"));
    CHECK(hud.fireEvent(countdownContract, "COUNTDOWN_GO"));
    CHECK_EQ(hud.currentSlide(countdown), std::string("GO"));
    // GO plays once with the next-slide flag: its end moves on to DONE.
    hud.update(1.1F);
    CHECK_EQ(hud.currentSlide(countdown), std::string("DONE"));
    CHECK(!hud.fireEvent(countdownContract, "NOT_AN_EVENT"));

    // The radio panel: its slides fire START_HIDE and HIDE_COMPLETE at the
    // HUD_RADIO contract near their end.
    const int radio = hud.contractParameter(hudContract, "COMPONENT_RADIO");
    const int radioContract = hud.contract(radio, "HUD_RADIO");
    CHECK(radioContract >= 0);
    const int radioControl = hud.contractParameter(radioContract, "RADIO_CONTROL");
    CHECK(hud.fireEvent(radioContract, "SHOW_INFO"));
    CHECK_EQ(hud.currentSlide(radioControl), std::string("CHANGETRACK"));
    hud.takeFiredEvents();
    hud.update(2.8F);
    std::vector<std::string> radioEvents;
    for (const Presentation::FiredEvent& fired : hud.takeFiredEvents()) {
        if (fired.target == radioContract) {
            radioEvents.push_back(fired.event);
        }
    }
    CHECK_EQ(radioEvents.size(), std::size_t{2});
    if (radioEvents.size() == 2) {
        CHECK_EQ(radioEvents[0], std::string("START_HIDE"));
        CHECK_EQ(radioEvents[1], std::string("HIDE_COMPLETE"));
    }
    CHECK(hud.fireEvent(radioContract, "HIDE_INFO"));
    CHECK_EQ(hud.currentSlide(radioControl), std::string("OFF"));
    CHECK(hud.fireEvent(radioContract, "SHOW_STATION"));
    CHECK_EQ(hud.currentSlide(radioControl), std::string("CHANGESTATION"));
    const int icons = hud.contractParameter(radioContract, "COMPONENT_RADIO_ICONS");
    CHECK(hud.goToSlideIndex(icons, 4));
    CHECK_EQ(hud.currentSlide(icons), std::string("OFF"));

    const std::optional<fh::Bytes> table = vfs.read("media/stringtables/en/ingame.str");
    CHECK(table.has_value());
}

TEST(disc_minimap_data_loads)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const NavFile nav = NavFile::parse(vfs.readOrThrow("media/tracks/colorado/colorado.nav"), "colorado.nav");
    std::size_t roads = 0;
    for (const NavWay& way : nav.ways) {
        roads += way.tag("road_type") == "a" || way.tag("road_type") == "b" || way.tag("road_type") == "dirt"
                || way.tag("road_type") == "freeway"
            ? 1
            : 0;
    }
    CHECK_EQ(nav.ways.size(), std::size_t{459});
    CHECK_EQ(roads, std::size_t{385});
    const fh::Bytes xml = vfs.readOrThrow("media/ui/MapProfileMinimap.xml");
    const MapProfile profile = MapProfile::parse(
        std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()), "MapProfileMinimap.xml", "minimap");
    CHECK(profile.groups.size() > 20);
    const std::vector<MapMarker> activities = loadActivityMarkers(vfs, "colorado");
    const auto count = [&](std::string_view tag) {
        return std::count_if(
            activities.begin(), activities.end(), [&](const MapMarker& marker) { return marker.value == tag; });
    };
    for (const char* tag : {"workshop", "paintshop", "autoshow", "carclub", "dlccenter", "racecentral"}) {
        CHECK_EQ(count(tag), 1);
    }
    CHECK_EQ(count("streetrace"), 3);
    CHECK_EQ(count("barnfind"), 0);
    CHECK_EQ(count("race"), 0);
    const SatNavTuning tuning = SatNavTuning::load(vfs);
    CHECK_NEAR(tuning.distanceMin, 500.0, 1e-3);
    CHECK_NEAR(tuning.playerViewportY, -0.3, 1e-5);
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
