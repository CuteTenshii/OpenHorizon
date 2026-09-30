// Tests of the spectator formats: parsers on data built here in the layouts
// the headers document, a small crowd loaded from a disc written to a
// temporary folder, and, when FH1_GAME_DIR names an extracted disc, the crowd
// of Colorado.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "spectators/CrowdDraw.h"
#include "spectators/CrowdSet.h"
#include "vfs/Vfs.h"

#include <lzx/zip_writer.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using fh::Bytes;
namespace sp = fh::spectators;

/// Builds big-endian test data.
struct Writer {
    Bytes data;

    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint16_t v)
    {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v));
    }
    void s16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void u32(std::uint32_t v)
    {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void vec3(glm::vec3 v)
    {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void text(std::string_view s) { data.insert(data.end(), s.begin(), s.end()); }
    void fill(std::size_t count, std::uint8_t value) { data.insert(data.end(), count, value); }
    void padTo(std::size_t size) { fill(size > data.size() ? size - data.size() : 0, 0); }
    void alignTo(std::size_t alignment) { padTo((data.size() + alignment - 1) / alignment * alignment); }
    void patch32(std::size_t at, std::uint32_t v)
    {
        data[at] = static_cast<std::uint8_t>(v >> 24);
        data[at + 1] = static_cast<std::uint8_t>(v >> 16);
        data[at + 2] = static_cast<std::uint8_t>(v >> 8);
        data[at + 3] = static_cast<std::uint8_t>(v);
    }
};

bool near(const glm::vec3& a, const glm::vec3& b, float tolerance)
{
    return glm::length(a - b) <= tolerance;
}

constexpr std::string_view kConfig = R"(<?xml version="1.0" encoding="utf-8"?>
<spectators>
    <models Version="2" numSpriteAngles="10" bSpritesBothSides="False">
        <model name="Male01" spritesPerTex="9"><texture name="Male01" /></model>
        <model name="Male02" spritesPerTex="9"><texture name="Female03" /></model>
    </models>
    <modelset name="Default">
        <model name="Male01" />
        <!-- <model name="Male03" /> -->
        <model name="Male02" />
    </modelset>
    <skeleton Version="2" name="Standing" suffix="_Stand" subdir="stand">
        <animation name="Stand_Idle_01" filename="Stand_Idle_01" />
        <modelset name="Default" />
    </skeleton>
    <crowdclass id="001" name="Stand">
        <skeleton name="Standing" />
        <modelset name="Default" />
        <idle name="Stand_Idle_01" weight="1.0"/>
        <cheer name="Stand_Cheer_99" weight="0.5"/>
    </crowdclass>
    <crowdclass id="002" name="Walkers" randomsize="false" offsetX="0.0" offsetY="0.25" offsetZ="0.0" BBoffsetY="-1.0">
        <skeleton name="Standing" />
        <modelset name="Default" />
        <idle name="Stand_Idle_01" weight="1.0"/>
    </crowdclass>
</spectators>
)";

/// A two-bone mesh: a quad from y = 0 to 1 on bone 0 and a vertex on bone 1,
/// with two levels of detail.
Bytes skinnedModel()
{
    struct Vertex {
        glm::vec3 position;
        std::uint16_t u;
        std::uint16_t v;
        std::uint8_t bone0;
        std::uint8_t bone1;
        std::uint8_t weight0;
    };
    const std::vector<Vertex> vertices = {
        {{0.5F, 0.0F, 0.0F}, 0, 65535, 0, 1, 255},
        {{-0.5F, 0.0F, 0.0F}, 65535, 65535, 0, 1, 255},
        {{0.5F, 1.0F, 0.0F}, 0, 0, 0, 1, 255},
        {{-0.5F, 1.0F, 0.0F}, 65535, 0, 1, 0, 128},
    };
    // Winding counter-clockwise seen from +Z in the file's right-handed
    // frame, the side the normals face.
    const std::vector<std::uint16_t> indices = {0, 2, 1, 1, 2, 3, 0, 2, 1};
    Writer w;
    w.u32(1);
    w.u32(2);
    w.u32(1);
    w.u32(2);
    w.u32(1);
    w.u16(0);
    w.u16(6);
    w.u32(1);
    w.u16(6);
    w.u16(3);
    w.u32(1);
    w.u32(1);
    w.u32(static_cast<std::uint32_t>(vertices.size()));
    for (const Vertex& v : vertices) {
        w.u32(1);
        w.vec3(v.position);
        w.vec3({0.0F, 0.0F, 1.0F});
        w.u16(v.u);
        w.u16(v.v);
        w.u8(v.bone0);
        w.u8(v.bone1);
        w.u8(v.weight0);
        w.u8(static_cast<std::uint8_t>(255 - v.weight0));
    }
    w.u32(1);
    w.u32(static_cast<std::uint32_t>(indices.size()));
    for (const std::uint16_t i : indices) {
        w.u16(i);
    }
    w.u32(static_cast<std::uint32_t>(vertices.size()));
    w.u32(static_cast<std::uint32_t>(indices.size()));
    return w.data;
}

/// Packs `(value, bits)` fields into the frame stream's layout: bit b of the
/// stream is bit b % 32 of the big-endian word b / 32. The stream keeps its
/// last word whole, as the files do.
Bytes packStream(const std::vector<std::pair<std::uint32_t, unsigned>>& fields, std::size_t totalBytes)
{
    std::vector<std::uint32_t> words((totalBytes + 3) / 4, 0);
    std::size_t bit = 0;
    for (const auto& [value, bits] : fields) {
        for (unsigned k = 0; k < bits; ++k, ++bit) {
            if ((value >> k) & 1U) {
                words[bit / 32] |= 1U << (bit % 32);
            }
        }
    }
    Writer w;
    for (const std::uint32_t word : words) {
        w.u32(word);
    }
    return w.data;
}

/// Stored rotation and translation values of the test clip's three frames.
constexpr std::array<std::uint32_t, 3> kRotationStored = {100, 1738, 3377};
constexpr std::array<std::uint32_t, 3> kTranslationStored = {0, 64, 127};
constexpr std::int16_t kRotationInitial = -100;
constexpr std::int16_t kTranslationInitial = 256;
constexpr std::int16_t kConstantInitial = -64;

/// A CAFF animation: bone 0 with an animated rotation x (12 bits), an
/// animated translation x (7 bits) and a constant translation y; bone 1
/// without channels. Frames are 3 bytes, so they straddle stream words.
Bytes animationFile(std::uint16_t bones = 2)
{
    Writer d;
    d.text("animation");
    d.padTo(0x0A);
    d.text("00.00.00.0036");
    d.padTo(0x1C);
    d.f32(2.0F / 30.0F);
    d.u16(bones);
    d.u16(3);
    d.u32(0x50);
    d.u32(0);
    const std::size_t boneTablePointer = d.data.size();
    d.u32(0);
    d.u32(0);
    const std::size_t nameTablePointer = d.data.size();
    d.u32(0);
    d.padTo(0x48);
    d.u32(0);
    d.padTo(0x50);
    d.text("QUAT_BITSTREAM");
    d.padTo(0x50 + 22);
    d.u16(0);
    d.u32(0x6C);
    d.f32(1.0F);
    d.u32(0x94);
    d.u32(0xB8);
    d.u32(0xDC);
    const std::size_t pointers = d.data.size();
    d.u32(0);
    d.u32(0);
    d.u16(8);
    d.u16(4);
    d.u16(3);
    d.u16(9);
    d.u16(12);
    d.u16(bones);
    d.u16(3);
    d.u8(0);
    d.u8(0);
    d.padTo(0x94);
    for (int t = 0; t < 3; ++t) {
        d.f32(1.0F / 16383.0F);
    }
    for (int t = 0; t < 6; ++t) {
        d.f32(1.0F / 128.0F);
    }
    for (int t = 0; t < 6; ++t) {
        d.f32(0.0F);
    }
    for (int t = 0; t < 3; ++t) {
        d.f32(1.0F);
    }
    d.u8(0x80 | 0x02);
    d.u8(0x80 | 0x40 | 0x08);
    for (std::uint16_t b = 1; b < bones; ++b) {
        d.u8(0);
    }
    d.alignTo(4);
    const std::size_t initials = d.data.size();
    d.s16(kRotationInitial);
    d.s16(kTranslationInitial);
    d.s16(kConstantInitial);
    d.alignTo(4);
    const std::size_t widths = d.data.size();
    d.u8(static_cast<std::uint8_t>((6 << 4) | 11));
    d.alignTo(4);
    std::vector<std::pair<std::uint32_t, unsigned>> fields;
    for (std::size_t f = 0; f < 3; ++f) {
        fields.emplace_back(kRotationStored[f], 12U);
        fields.emplace_back(kTranslationStored[f], 7U);
        fields.emplace_back(0U, 5U);
    }
    const Bytes stream = packStream(fields, 9);
    d.data.insert(d.data.end(), stream.begin(), stream.end());
    d.alignTo(4);
    d.patch32(pointers, static_cast<std::uint32_t>(initials));
    d.patch32(pointers + 4, static_cast<std::uint32_t>(widths));

    const std::size_t boneTable = d.data.size();
    d.patch32(boneTablePointer, static_cast<std::uint32_t>(boneTable));
    for (std::uint16_t b = 0; b < bones; ++b) {
        d.vec3(b == 0 ? glm::vec3(0.0F, 1.0F, 0.0F) : glm::vec3(0.0F, 0.5F, 0.0F));
        d.vec3(glm::vec3(0.0F, 1.0F + 0.5F * static_cast<float>(b), 0.0F));
        d.fill(16, 0xCD);
        d.s16(static_cast<std::int16_t>(b == 0 ? -1 : b - 1));
        d.s16(static_cast<std::int16_t>(b + 1 < bones ? b + 1 : -1));
        d.s16(-1);
        d.s16(static_cast<std::int16_t>(b));
        d.s16(-1);
        d.s16(0);
    }
    const std::size_t nameTable = d.data.size();
    d.patch32(nameTablePointer, static_cast<std::uint32_t>(nameTable));
    d.u32(bones);
    d.u32(static_cast<std::uint32_t>(nameTable + 8));
    std::vector<std::size_t> namePointers;
    for (std::uint16_t b = 0; b < bones; ++b) {
        namePointers.push_back(d.data.size());
        d.u32(0);
        d.u32(0);
        d.u32(b);
    }
    for (std::uint16_t b = 0; b < bones; ++b) {
        d.patch32(namePointers[b], static_cast<std::uint32_t>(d.data.size()));
        d.text(b == 0 ? "Bip001" : "sk1_Bone_root");
        d.u8(0);
    }
    d.alignTo(16);

    Writer file;
    file.text("CAFF21.11.05.0034");
    file.padTo(0x48);
    file.u32(0x190);
    file.padTo(0x60);
    file.u32(static_cast<std::uint32_t>(d.data.size()));
    file.padTo(0x190);
    file.data.insert(file.data.end(), d.data.begin(), d.data.end());
    return file.data;
}

/// A crowd placement set with an event block and two people, or without the
/// block when `tag` is empty.
Bytes crowdSet(std::string_view name, std::string_view tag, const std::vector<std::uint8_t>& routes, glm::vec3 low,
    glm::vec3 high, const std::vector<std::array<std::uint16_t, 5>>& people)
{
    Writer w;
    w.text("OEGP");
    w.u32(42);
    w.f32(60.0F);
    w.f32(-1.0F);
    w.vec3(low + glm::vec3(1.0F, 1.0F, 1.0F));
    w.u32(0);
    w.vec3(high - glm::vec3(1.0F, 1.0F, 1.0F));
    w.u32(0);
    w.u32(3);
    w.f32(-1.0F);
    w.f32(-1.0F);
    w.u32(static_cast<std::uint32_t>(name.size() + 1));
    w.padTo(0x48);
    w.u32(0);
    w.padTo(0x58);
    w.u32(tag.empty() ? 0 : 0x0C92CA70);
    w.padTo(0x60);
    if (!tag.empty()) {
        w.u32(0xFFFFFFFF);
        w.text(tag);
        w.padTo(0x64 + 32);
        w.u8(static_cast<std::uint8_t>(routes.size()));
        for (const std::uint8_t route : routes) {
            w.u8(route);
        }
        w.padTo(0x98);
    }
    w.text(name);
    w.u8(0);
    w.alignTo(16);
    w.vec3(low);
    w.u32(0);
    w.vec3(high);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(people.size()));
    w.fill(28, 0xAB);
    for (const auto& p : people) {
        w.u16(p[0]);
        w.u16(p[1]);
        w.u16(p[2]);
        w.u8(static_cast<std::uint8_t>(p[3]));
        w.u8(static_cast<std::uint8_t>(p[4]));
        w.u8(1);
        w.u8(0);
        w.u8(0);
        w.u8(0);
    }
    w.patch32(0x48, static_cast<std::uint32_t>(w.data.size()));
    return w.data;
}

/// A crowd placement set of one walking path through four points.
Bytes walkSet(std::string_view name)
{
    Writer w;
    w.text("OEGP");
    w.u32(42);
    w.f32(60.0F);
    w.f32(-1.0F);
    w.padTo(0x30);
    w.u32(3);
    w.f32(-1.0F);
    w.f32(-1.0F);
    w.u32(static_cast<std::uint32_t>(name.size() + 1));
    w.padTo(0x60);
    w.text(name);
    w.u8(0);
    w.alignTo(16);
    const std::size_t group = w.data.size();
    w.vec3(glm::vec3(3.4e38F));
    w.u32(0);
    w.vec3(glm::vec3(-3.4e38F));
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(1);
    w.padTo(group + 64);
    w.u32(0);
    w.u32(0);
    w.u32(1);
    w.alignTo(16);
    w.vec3(glm::vec3(0.0F));
    w.u32(0);
    w.vec3(glm::vec3(10.0F, 0.0F, 10.0F));
    w.u32(0);
    w.u32(0);
    w.u32(2);
    w.f32(40.0F);
    w.u32(0);
    // Stored order second, first, third: the path runs (0,0) (10,0) (10,10)
    // then (0,10) (0,5) (0,1).
    const std::array<std::array<glm::vec3, 3>, 2> segments = {{
        {glm::vec3(10.0F, 0.0F, 0.0F), glm::vec3(0.0F, 0.0F, 0.0F), glm::vec3(10.0F, 0.0F, 10.0F)},
        {glm::vec3(0.0F, 0.0F, 5.0F), glm::vec3(0.0F, 0.0F, 10.0F), glm::vec3(0.0F, 0.0F, 1.0F)},
    }};
    for (std::size_t s = 0; s < segments.size(); ++s) {
        for (const glm::vec3& point : segments[s]) {
            w.vec3(point);
            w.u32(0x0018F190);
        }
        w.fill(16, 0);
        for (int k = 0; k < 8; ++k) {
            w.f32(static_cast<float>(s * 20 + static_cast<std::size_t>(k) * 2));
        }
    }
    w.patch32(0x48, static_cast<std::uint32_t>(w.data.size()));
    return w.data;
}

void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> data)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    writeFile(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

} // namespace

TEST_CASE("configParsesModelsSetsSkeletonsAndClasses", "[spectators]")
{
    const sp::SpectatorConfig config = sp::SpectatorConfig::parse(kConfig, "test.xml");
    CHECK(config.version == 2);
    CHECK(config.spriteAngles == 10);
    CHECK(!config.spritesBothSides);
    CHECK(config.models.size() == std::size_t{2});
    CHECK(config.models[1].texture == std::string("Female03"));
    CHECK(config.models[1].spritesPerTexture == 9);
    CHECK(config.modelSets[0].models.size() == std::size_t{2});
    CHECK(config.skeletons[0].suffix == std::string("_Stand"));
    CHECK(config.skeletons[0].animations[0].file == std::string("Stand_Idle_01"));
    CHECK(config.classes.size() == std::size_t{2});
    CHECK(config.classes[0].id == 1);
    CHECK(config.classes[0].cheers[0].weight == 0.5F);
    CHECK(!config.classes[0].randomSize.has_value());
    CHECK((config.classes[1].randomSize.has_value() && !*config.classes[1].randomSize));
    CHECK(config.classes[1].offset.y == 0.25F);
    CHECK(config.classes[1].billboardOffsetY == -1.0F);
    CHECK(config.findModel("male02") == 1);
    CHECK(config.findModelSet("DEFAULT") == 0);
    CHECK(config.findSkeleton("Sitting") == -1);
    CHECK_THROWS_AS(sp::SpectatorConfig::parse("<spectators>", "broken.xml"), std::exception);
    CHECK_THROWS_AS(sp::SpectatorConfig::parse("<other/>", "other.xml"), std::exception);
}

TEST_CASE("skinnedModelParsesVerticesIndicesAndLevels", "[spectators]")
{
    const Bytes data = skinnedModel();
    const sp::SkinnedModel model = sp::parseSkinnedModel(data, "test.skinbin");
    CHECK(model.vertices.size() == std::size_t{4});
    CHECK(model.indices.size() == std::size_t{9});
    CHECK(model.lods.size() == std::size_t{2});
    CHECK(model.lods[1].firstIndex == 6U);
    CHECK(model.lods[1].indexCount == 3U);
    CHECK(near(model.vertices[2].position, {0.5F, 1.0F, 0.0F}, 1e-6F));
    CHECK_THAT(model.vertices[1].texcoord.x, WithinAbs(1.0, 1e-6));
    CHECK_THAT(model.vertices[1].texcoord.y, WithinAbs(1.0, 1e-6));
    CHECK(model.vertices[3].bones[0] == 1);
    CHECK(model.vertices[3].weights[1] == 127);
    CHECK(model.boneCount() == 2);
    CHECK_THAT(model.height(), WithinAbs(1.0, 1e-6));

    Bytes badMarker = data;
    badMarker[3] = 2;
    CHECK_THROWS_AS(sp::parseSkinnedModel(badMarker, "bad.skinbin"), std::exception);
    Bytes truncated(data.begin(), data.end() - 6);
    CHECK_THROWS_AS(sp::parseSkinnedModel(truncated, "short.skinbin"), std::exception);
    Bytes badIndex = data;
    const std::size_t firstIndex = data.size() - 8 - 9 * 2;
    badIndex[firstIndex + 1] = 9;
    CHECK_THROWS_AS(sp::parseSkinnedModel(badIndex, "index.skinbin"), std::exception);
}

TEST_CASE("spectatorMeshMirrorsXAndKeepsFrontsClockwise", "[spectators]")
{
    const sp::SkinnedModel model = sp::parseSkinnedModel(skinnedModel(), "test.skinbin");
    const sp::SpectatorMesh mesh = sp::buildSpectatorMesh(model, 7);
    CHECK(mesh.vertexCount() == std::size_t{4});
    CHECK(mesh.lods.size() == std::size_t{2});
    CHECK(mesh.lods[0].texture == 7U);
    CHECK(mesh.triangleCount(0) == std::size_t{2});
    CHECK(mesh.triangleCount(1) == std::size_t{1});
    const float* v0 = &mesh.vertices[0];
    CHECK(v0[0] == -0.5F);
    CHECK(v0[sp::SpectatorMesh::kNormal + 2] == 1.0F);
    CHECK(std::bit_cast<std::uint32_t>(v0[sp::SpectatorMesh::kColour]) == 0xFFFFFFFFU);
    CHECK(v0[sp::SpectatorMesh::kBones + 1] == 1.0F);
    CHECK(v0[sp::SpectatorMesh::kWeights] == 1.0F);
    // Clockwise from the front in a left-handed frame: the numeric cross
    // product of the edges points along the normal.
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        const auto at = [&](std::uint32_t i) {
            const float* v = &mesh.vertices[i * sp::SpectatorMesh::kFloatsPerVertex];
            return glm::vec3(v[0], v[1], v[2]);
        };
        const glm::vec3 a = at(mesh.indices[t]);
        const glm::vec3 cross = glm::cross(at(mesh.indices[t + 1]) - a, at(mesh.indices[t + 2]) - a);
        CHECK(cross.z > 0.0F);
    }
}

TEST_CASE("animationDecodesBitStreamChannels", "[spectators]")
{
    const Bytes data = animationFile();
    const sp::AnimationClip clip = sp::parseAnimation(data, "test.anim.bin");
    CHECK(clip.frameCount == 3);
    CHECK(clip.boneCount() == std::size_t{2});
    CHECK_THAT(clip.framesPerSecond(), WithinAbs(30.0, 1e-3));
    CHECK(clip.animatedChannels == 2);
    CHECK(clip.constantChannels == 1);
    CHECK(clip.skeleton.bones[1].name == std::string("sk1_Bone_root"));
    CHECK(clip.skeleton.bones[1].parent == 0);
    CHECK(near(clip.skeleton.bones[1].modelPosition, {0.0F, 1.5F, 0.0F}, 1e-6F));
    for (int f = 0; f < 3; ++f) {
        const sp::BonePose& pose = clip.pose(f, 0);
        const float x
            = static_cast<float>(static_cast<int>(kRotationStored[static_cast<std::size_t>(f)]) + kRotationInitial)
            / 16383.0F;
        CHECK_THAT(pose.rotation.x, WithinAbs(x, 1e-6));
        CHECK_THAT(pose.rotation.w, WithinAbs(std::sqrt(1.0F - x * x), 1e-6));
        CHECK_THAT(pose.rotation.y, WithinAbs(0.0, 1e-9));
        const float tx = static_cast<float>(
                             static_cast<int>(kTranslationStored[static_cast<std::size_t>(f)]) + kTranslationInitial)
            / 128.0F;
        CHECK_THAT(pose.translation.x, WithinAbs(tx, 1e-6));
        CHECK_THAT(pose.translation.y, WithinAbs(-0.5, 1e-6));
        CHECK_THAT(pose.translation.z, WithinAbs(0.0, 1e-9));
        CHECK(near(pose.scale, glm::vec3(1.0F), 1e-6F));
        CHECK_THAT(clip.pose(f, 1).rotation.w, WithinAbs(1.0, 1e-9));
    }

    Bytes badVersion = data;
    badVersion[0x190 + 0x0A + 12] = '7';
    CHECK_THROWS_AS(sp::parseAnimation(badVersion, "version.anim.bin"), std::exception);
    Bytes notCaff = data;
    notCaff[0] = 'X';
    CHECK_THROWS_AS(sp::parseAnimation(notCaff, "magic.anim.bin"), std::exception);
    Bytes truncated(data.begin(), data.begin() + 0x190 + 0x40);
    CHECK_THROWS_AS(sp::parseAnimation(truncated, "short.anim.bin"), std::exception);
}

TEST_CASE("animationSamplesAndSkins", "[spectators]")
{
    const sp::AnimationClip clip = sp::parseAnimation(animationFile(), "test.anim.bin");
    std::vector<sp::BonePose> pose(clip.boneCount());
    sp::samplePose(clip, 0.5F / 30.0F, pose);
    CHECK_THAT(pose[0].translation.x, WithinAbs(2.25, 1e-5));
    // Looping: one duration later is the same pose.
    std::vector<sp::BonePose> later(clip.boneCount());
    sp::samplePose(clip, clip.duration + 0.5F / 30.0F, later);
    CHECK_THAT(later[0].translation.x, WithinAbs(pose[0].translation.x, 1e-4));

    sp::samplePose(clip, 0.0F, pose);
    std::vector<glm::mat4> matrices(clip.boneCount());
    sp::skinMatrices(clip.skeleton, clip.skeleton, pose, matrices);
    // Frame 0: no rotation, bone 0 moved by (2, -0.5, 0); bone 1 follows.
    const glm::vec4 onBone0 = matrices[0] * glm::vec4(0.0F, 1.0F, 0.0F, 1.0F);
    CHECK(near(glm::vec3(onBone0), {2.0F, 0.5F, 0.0F}, 1e-5F));
    const glm::vec4 onBone1 = matrices[1] * glm::vec4(0.0F, 1.5F, 0.0F, 1.0F);
    CHECK(near(glm::vec3(onBone1), {2.0F, 1.0F, 0.0F}, 1e-5F));

    // A quarter turn about Y on the root carries the child's offset along.
    sp::Skeleton skeleton = clip.skeleton;
    skeleton.bones[1].localOffset = {1.0F, 0.0F, 0.0F};
    skeleton.bones[1].modelPosition = {1.0F, 1.0F, 0.0F};
    std::vector<sp::BonePose> turn(2);
    turn[0].rotation = glm::angleAxis(std::numbers::pi_v<float> / 2.0F, glm::vec3(0.0F, 1.0F, 0.0F));
    sp::skinMatrices(skeleton, skeleton, turn, matrices);
    const glm::vec4 moved = matrices[1] * glm::vec4(1.0F, 1.0F, 0.0F, 1.0F);
    CHECK(near(glm::vec3(moved), {0.0F, 1.0F, -1.0F}, 1e-5F));

    const sp::AnimationClip three = sp::parseAnimation(animationFile(3), "three.anim.bin");
    CHECK_THROWS_AS(sp::skinMatrices(three.skeleton, clip.skeleton, pose, matrices), std::exception);
}

TEST_CASE("crowdPlacementSetsParseBothLayouts", "[spectators]")
{
    const Bytes data = crowdSet("crowd_proc_test_fr04_1", "FR04_15", {0, 7}, {0.0F, 0.0F, 0.0F},
        {100.0F, 10.0F, 200.0F}, {{{0, 0, 0, 64, 1}}, {{65535, 32768, 65535, 192, 8}}});
    CHECK(sp::placementSetType(data).value_or(0) == sp::kCrowdSetType);
    const sp::CrowdPlacementSet set = sp::parseCrowdPlacementSet(data, "set.pgeo");
    CHECK(set.name == std::string("crowd_proc_test_fr04_1"));
    CHECK(set.hasEventBlock);
    CHECK(set.eventTag == std::string("FR04_15"));
    CHECK(set.eventId() == std::string("FR04"));
    CHECK(set.routes.size() == std::size_t{2});
    CHECK(set.routes[1] == 7);
    CHECK(set.spectators.size() == std::size_t{2});
    CHECK(near(set.spectators[0].position, {0.0F, 0.0F, 0.0F}, 1e-4F));
    CHECK(near(set.spectators[1].position, {100.0F, 5.0F, 200.0F}, 1e-3F));
    // b / 255 of a turn, as the game's decoder reads it.
    CHECK_THAT(set.spectators[0].headingRadians(), WithinAbs(64.0 / 255.0 * 2.0 * std::numbers::pi, 1e-6));
    CHECK_THAT(set.spectators[1].headingRadians(), WithinAbs(192.0 / 255.0 * 2.0 * std::numbers::pi, 1e-6));
    CHECK(set.spectators[1].crowdClass == 8);
    CHECK(set.spectators[1].flags == 1);
    CHECK(near(set.boundsMin, {1.0F, 1.0F, 1.0F}, 1e-6F));

    const Bytes plain = crowdSet("crowd_festival", "", {}, {-5.0F, 0.0F, -5.0F}, {5.0F, 3.0F, 5.0F}, {});
    const sp::CrowdPlacementSet empty = sp::parseCrowdPlacementSet(plain, "plain.pgeo");
    CHECK(!empty.hasEventBlock);
    CHECK(empty.name == std::string("crowd_festival"));
    CHECK(empty.spectators.empty());

    const sp::CrowdPlacementSet walk = sp::parseCrowdPlacementSet(walkSet("crowd_walk_1"), "walk.pgeo");
    CHECK(walk.walkPaths.size() == std::size_t{1});
    const sp::WalkPath& path = walk.walkPaths[0];
    CHECK(path.crowdClass == 1);
    CHECK(path.points.size() == std::size_t{6});
    CHECK(near(path.points[0], {0.0F, 0.0F, 0.0F}, 1e-6F));
    CHECK(near(path.points[3], {0.0F, 0.0F, 10.0F}, 1e-6F));
    CHECK(path.distances.size() == std::size_t{16});
    CHECK_THAT(path.length, WithinAbs(40.0, 1e-6));
    CHECK_THAT(path.loopLength(), WithinAbs(40.0, 1e-5));
    glm::vec3 direction(0.0F);
    CHECK(near(path.pointAt(15.0F, &direction), {10.0F, 0.0F, 5.0F}, 1e-5F));
    CHECK(near(direction, {0.0F, 0.0F, 1.0F}, 1e-6F));
    CHECK(near(path.pointAt(55.0F), {10.0F, 0.0F, 5.0F}, 1e-4F));

    Bytes wrongSize = data;
    wrongSize.push_back(0);
    CHECK_THROWS_AS(sp::parseCrowdPlacementSet(wrongSize, "size.pgeo"), std::exception);
    Bytes otherType = data;
    otherType[0x33] = 7;
    CHECK(sp::placementSetType(otherType) == std::optional<std::uint32_t>(7));
    CHECK_THROWS_AS(sp::parseCrowdPlacementSet(otherType, "type.pgeo"), std::exception);
    CHECK(!sp::placementSetType(Bytes(0x40, 0)).has_value());

    Writer legacy;
    legacy.u32(2);
    legacy.u32(0);
    legacy.u32(0);
    const sp::LegacyCrowdFile file = sp::parseLegacyCrowdFile(legacy.data, "test.crowd");
    CHECK(file.version == 2U);
    CHECK(file.entryCount == 0U);
    CHECK_THROWS_AS(sp::parseLegacyCrowdFile(Bytes(8, 0), "short.crowd"), std::exception);
}

TEST_CASE("spritesChooseCellsByViewAngle", "[spectators]")
{
    const sp::SpriteAtlasLayout layout {2048, 768};
    CHECK(layout.columns() == 32);
    CHECK(layout.cellCount() == 192);
    const glm::vec3 facing(0.0F, 0.0F, 1.0F);
    const glm::vec3 origin(0.0F);
    // Model 2's cells start at 18; standing people take three views.
    sp::SpriteFrame frame = sp::chooseSprite(layout, 18, false, facing, origin, {0.0F, 1.0F, 10.0F});
    CHECK(frame.view == sp::SpriteView::StandingFront);
    CHECK(frame.cell == 19);
    CHECK_THAT(frame.uvMin.x, WithinAbs(19.0 * 64.0 / 2048.0, 1e-6));
    CHECK_THAT(frame.uvMax.y, WithinAbs(128.0 / 768.0, 1e-6));
    CHECK(
        sp::chooseSprite(layout, 0, false, facing, origin, {0.0F, 0.0F, -10.0F}).view == sp::SpriteView::StandingBack);
    frame = sp::chooseSprite(layout, 0, false, facing, origin, {10.0F, 0.0F, 0.0F});
    CHECK(frame.view == sp::SpriteView::StandingSide);
    CHECK(!frame.mirrored);
    frame = sp::chooseSprite(layout, 0, false, facing, origin, {-10.0F, 0.0F, 0.0F});
    CHECK(frame.view == sp::SpriteView::StandingSide);
    CHECK(frame.mirrored);
    // The first class's six views start three cells in (model 1: 9 + 3).
    frame = sp::chooseSprite(layout, 12, true, facing, origin, {10.0F, 0.0F, 10.0F});
    CHECK(frame.view == sp::SpriteView::SeatedTurned45);
    CHECK(frame.cell == 9 + 6);
    CHECK(sp::chooseSprite(layout, 12, true, facing, origin, {10.0F, 0.0F, -1.0F}).view == sp::SpriteView::SeatedSide);
    CHECK(sp::chooseSprite(layout, 12, true, facing, origin, {0.0F, 0.0F, -10.0F}).view == sp::SpriteView::SeatedBack);
    CHECK(sp::chooseSprite(layout, 12, true, facing, origin, {0.0F, 0.0F, 10.0F}).view == sp::SpriteView::SeatedFront);
    // Between 90 and 135 degrees the six views keep the side cell.
    CHECK(sp::chooseSprite(layout, 12, true, facing, origin, {10.0F, 0.0F, -9.0F}).view == sp::SpriteView::SeatedSide);

    fh::world::RgbaImage atlas;
    atlas.width = 128;
    atlas.height = 128;
    atlas.rgba.assign(static_cast<std::size_t>(atlas.width * atlas.height) * 4, 0);
    // Model 0's standing front cell is cell 1; an opaque column from row 20
    // to row 109.
    for (int y = 20; y < 110; ++y) {
        atlas.rgba[(static_cast<std::size_t>(y) * 128 + 64 + 30) * 4 + 3] = 255;
    }
    const sp::SpriteMetrics metrics = sp::measureSprites(atlas, {1.8F});
    CHECK_THAT(metrics.metresPerTexel, WithinAbs(1.8 / 90.0, 1e-6));
    CHECK_THAT(metrics.groundRow, WithinAbs(110.0, 1e-6));
    const sp::SpriteMetrics none = sp::measureSprites(atlas, {0.0F});
    CHECK_THAT(none.groundRow, WithinAbs(128.0, 1e-6));
}

TEST_CASE("crowdDrawFollowsTheGamesRules", "[spectators]")
{
    namespace draw = sp::draw;
    // Models are whole within 60 m and gone at 80 m.
    CHECK_THAT(draw::modelFade(10.0F, draw::kModelFade), WithinAbs(1.0, 1e-6));
    CHECK_THAT(draw::modelFade(70.0F, draw::kModelFade), WithinAbs(0.5, 1e-6));
    CHECK_THAT(draw::modelFade(80.0F, draw::kModelFade), WithinAbs(0.0, 1e-6));
    CHECK_THAT(draw::modelFade(190.0F, draw::kWalkerFade), WithinAbs(0.5, 1e-6));
    // Sprites come in from 40 to 60 m and go from 650 to 700 m; those of
    // billboard-only people show from the camera out.
    CHECK_THAT(draw::spriteFade(30.0F, false), WithinAbs(0.0, 1e-6));
    CHECK_THAT(draw::spriteFade(50.0F, false), WithinAbs(0.5, 1e-6));
    CHECK_THAT(draw::spriteFade(100.0F, false), WithinAbs(1.0, 1e-6));
    CHECK_THAT(draw::spriteFade(675.0F, false), WithinAbs(0.5, 1e-6));
    CHECK_THAT(draw::spriteFade(700.0F, false), WithinAbs(0.0, 1e-6));
    CHECK_THAT(draw::spriteFade(5.0F, true), WithinAbs(1.0, 1e-6));
    CHECK_THAT(draw::spriteFade(800.0F, true), WithinAbs(0.0, 1e-6));

    // Eight people within 10 m: the band's cap of five sends three on to the
    // next level; a person past 90 m takes the last band, clamped to the
    // model's four levels.
    std::vector<float> distances(8, 5.0F);
    distances.push_back(20.0F);
    distances.push_back(95.0F);
    const std::vector<int> lods = draw::modelLods(distances, 4);
    CHECK((lods == std::vector<int>{0, 0, 0, 0, 0, 1, 1, 1, 1, 3}));

    // The shader's arc cosine is within 1e-4 of the true one.
    for (const float c : {-1.0F, -0.7F, -0.2F, 0.0F, 0.3F, 0.707F, 0.95F, 1.0F}) {
        CHECK_THAT(draw::shaderArcCos(c), WithinAbs(std::acos(c), 1e-4));
    }
    // Three views: front within 45 degrees, back past 135, side between;
    // six views: a column every 22.5 degrees from the front.
    const glm::vec2 north(0.0F, 1.0F);
    CHECK(draw::spriteColumn(north, glm::normalize(glm::vec2(0.5F, 1.0F)), false).column == 1);
    CHECK(draw::spriteColumn(north, glm::vec2(1.0F, 0.0F), false).column == 2);
    CHECK(draw::spriteColumn(north, glm::normalize(glm::vec2(0.5F, -1.0F)), false).column == 0);
    for (int step = 0; step <= 4; ++step) {
        const float angle = static_cast<float>(step) * std::numbers::pi_v<float> / 8.0F;
        const glm::vec2 toCamera(std::sin(angle), std::cos(angle));
        CHECK(draw::spriteColumn(north, toCamera, true).column == step + 1);
    }
    // The front and back views are never mirrored.
    CHECK(!draw::spriteColumn(north, glm::vec2(-0.1F, 1.0F), true).mirrored);
    CHECK(draw::spriteColumn(north, glm::vec2(-1.0F, 0.0F), true).mirrored);

    // The crowd's generator and the C runtime's rand().
    draw::CrowdRandom random(0);
    CHECK(random.next() == 0U);
    CHECK(random.next() == 21468U);
    CHECK(random.next() == 9988U);
    draw::CrtRandom crt;
    CHECK(crt.next() == 41U);
    CHECK(crt.next() == 18467U);

    // Three clips of equal weight fill 17, 17 and 16 of the 50 slots.
    const std::vector<std::uint32_t> clips{7, 8, 9};
    const std::vector<float> weights{1.0F, 1.0F, 1.0F};
    const std::vector<std::uint32_t> table = draw::clipTable(clips, weights);
    CHECK(table.size() == draw::kClipSlots);
    CHECK(std::count(table.begin(), table.end(), 7U) == 17);
    CHECK(std::count(table.begin(), table.end(), 8U) == 17);
    CHECK(std::count(table.begin(), table.end(), 9U) == 16);
    CHECK(draw::clipTable({}, {}).empty());
}

TEST_CASE("sessionsChooseGroups", "[spectators]")
{
    sp::CrowdGroup ambient;
    sp::CrowdGroup start;
    start.hasEventBlock = true;
    start.eventTag = "FR04_15";
    start.eventId = "FR04";
    start.routes = {0};
    sp::CrowdGroup race;
    race.hasEventBlock = true;
    race.routes = {12, 40};
    CHECK(sp::CrowdSession::freeRoam().shows(ambient));
    CHECK(sp::CrowdSession::freeRoam().shows(start));
    CHECK(!sp::CrowdSession::freeRoam().shows(race));
    CHECK(sp::CrowdSession::freeRoam({"fr04"}).shows(start));
    CHECK(!sp::CrowdSession::freeRoam({"FR05"}).shows(start));
    CHECK(sp::CrowdSession::raceOn("NR01", 40).shows(race));
    CHECK(!sp::CrowdSession::raceOn("NR01", 41).shows(race));
    CHECK(sp::CrowdSession::raceOn("FR04", 3).shows(start));
    CHECK(sp::CrowdSession::raceOn("NR01", 40).shows(ambient));
    CHECK(!race.belongsToRace("FR04", 0));
}

TEST_CASE("crowdSetLoadsFromADisc", "[spectators]")
{
    const std::filesystem::path root
        = std::filesystem::temp_directory_path() / std::format("fh1_spectators_test_{}", std::rand());
    std::filesystem::remove_all(root);
    writeText(root / "media" / "zipmanifest.xml",
        R"(<ZipFiles><Zip version="1" path="game:\media\tracks\testtrack\bin.zip" priority="10" /></ZipFiles>)");
    writeText(root / "media" / "spectators" / "spectators.xml", kConfig);
    const Bytes model = skinnedModel();
    writeFile(root / "media" / "spectators" / "models" / "male01_stand.skinbin", model);
    writeFile(root / "media" / "spectators" / "models" / "male02_stand.skinbin", model);
    writeFile(root / "media" / "spectators" / "anims" / "stand_idle_01.anim.bin", animationFile());
    Writer legacy;
    legacy.u32(2);
    legacy.u32(0);
    legacy.u32(0);
    writeFile(root / "media" / "tracks" / "testtrack" / "ribbon_00" / "testtrack_track_00.crowd", legacy.data);
    {
        lzx::ZipWriter writer(root / "media" / "tracks" / "testtrack" / "bin.zip");
        writer.add("__R00G00001.pgeo",
            crowdSet("crowd_festival_1", "", {}, {0.0F, 0.0F, 0.0F}, {100.0F, 10.0F, 100.0F},
                {{{0, 0, 0, 0, 0}}, {{65535, 0, 65535, 0, 1}}, {{32768, 0, 32768, 0, 0}}}));
        writer.add("__R00G00002.pgeo",
            crowdSet("crowd_route_7", "NR07_15", {7}, {200.0F, 0.0F, 0.0F}, {210.0F, 10.0F, 10.0F},
                {{{0, 0, 0, 128, 0}}, {{0, 0, 0, 128, 5}}}));
        writer.add("__R00G00003.pgeo", walkSet("crowd_walk_1"));
        Writer grass;
        grass.text("OEGP");
        grass.padTo(0x30);
        grass.u32(2);
        grass.padTo(0x80);
        writer.add("__R00G00004.pgeo", grass.data);
        writer.finish();
    }

    // Held by pointer so it can close the archive before the folder goes:
    // Windows refuses to delete a file that is still open.
    auto vfs = std::make_unique<fh::Vfs>();
    vfs->mountDisc(root);
    const sp::CrowdSet crowd = sp::CrowdSet::load(*vfs, "testtrack");
    CHECK(crowd.groups().size() == std::size_t{3});
    CHECK(crowd.spectators().size() == std::size_t{3 + 2 + 1});
    CHECK(crowd.models().size() == std::size_t{2});
    CHECK(crowd.animations().size() == std::size_t{1});
    CHECK(crowd.classes().size() == std::size_t{2});
    CHECK(crowd.walkPaths().size() == std::size_t{1});
    CHECK(crowd.report().legacyFile.has_value());
    CHECK(crowd.report().unknownClassInstances == 1);
    // Missing: two textures, the sprites and the shadow; and the person of
    // an unknown class. The class's cheer names a clip its skeleton lacks,
    // which the game leaves out, as the load does.
    CHECK(crowd.report().failures.size() == std::size_t{5});
    CHECK(crowd.classes()[0].cheers.empty());
    CHECK(crowd.spriteTexture() == -1);

    const sp::Spectator& first = crowd.spectators()[0];
    CHECK(first.crowdClass == 0);
    CHECK((first.model == 0 || first.model == 1));
    CHECK(first.animation == 0);
    CHECK((first.startFrame >= 0 && first.startFrame < crowd.animations()[0].clip.frameCount));
    CHECK(near(crowd.spectators()[1].position, {100.0F, 0.25F, 100.0F}, 1e-3F));

    const std::vector<std::uint32_t> nearCentre = crowd.spectatorsNear(50.0F, 50.0F, 10.0F);
    CHECK(nearCentre.size() == std::size_t{1});
    // The two people of the first set and the walker, whose path runs
    // around the origin.
    const std::vector<std::uint32_t> nearOrigin = crowd.spectatorsNear(0.0F, 0.0F, 80.0F);
    CHECK(nearOrigin.size() == std::size_t{3});
    CHECK(std::find(nearOrigin.begin(), nearOrigin.end(), 0U) != nearOrigin.end());

    const sp::CrowdGroup& routeGroup = crowd.groups()[1];
    CHECK(routeGroup.eventId == std::string("NR07"));
    CHECK(crowd.groupsFor(sp::CrowdSession::freeRoam()).size() == std::size_t{2});
    CHECK(crowd.spectatorsFor(sp::CrowdSession::raceOn("NR07", 7)).size() == std::size_t{6});
    CHECK(crowd.spectatorsFor(sp::CrowdSession::raceOn("NR07", 7), 0.0F).size() == std::size_t{0});
    CHECK(crowd.spectatorsFor(sp::CrowdSession::raceOn("NR07", 7), 0.5F).size() <= std::size_t {6});

    const sp::Spectator& walker = crowd.spectators().back();
    CHECK(walker.walkPath == 0);
    float heading = 0.0F;
    const auto walkerIndex = static_cast<std::uint32_t>(crowd.spectators().size() - 1);
    CHECK(near(crowd.walkerAt(walkerIndex, 0.0F, heading), walker.position, 1e-4F));
    const glm::vec3 later = crowd.walkerAt(walkerIndex, 2.0F, heading);
    // 1 m/s along the path, lifted by the Walkers class's offset.
    CHECK(
        near(later, crowd.walkPaths()[0].path.pointAt(walker.walkStart + 2.0F) + glm::vec3(0.0F, 0.25F, 0.0F), 1e-3F));

    const std::vector<glm::mat4> matrices = crowd.skinMatrices(0, 0, 0.0F);
    CHECK(matrices.size() == std::size_t{2});
    const std::vector<glm::vec3> skinned = crowd.skinnedPositions(0, 0, 0.0F);
    CHECK(skinned.size() == std::size_t{4});
    // Spectator space mirrors X, so the clip's +2 m along X becomes -2 m.
    CHECK(near(skinned[0], {-0.5F - 2.0F, -0.5F, 0.0F}, 1e-4F));

    // The game gives the first class six views, from the model's fourth
    // cell on, whatever the class is called.
    const sp::SpriteBillboard billboard = crowd.spriteFor(0, {0.0F, 1.0F, 10.0F});
    CHECK(billboard.frame.view == sp::SpriteView::SeatedFront);
    CHECK(billboard.height > 0.0F);

    vfs.reset();
    std::filesystem::remove_all(root);
}

TEST_CASE("coloradoCrowd", "[spectators]")
{
    const char* gameDir = fh::test::gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(gameDir);
    const sp::CrowdSet crowd = sp::CrowdSet::load(vfs, "colorado");
    CHECK(crowd.groups().size() == std::size_t{1783});
    CHECK(crowd.spectators().size() == std::size_t{149373});
    CHECK(crowd.walkPaths().size() == std::size_t{247});
    CHECK(crowd.models().size() == std::size_t{40});
    CHECK(crowd.animations().size() == std::size_t{56});
    CHECK(crowd.textures().size() == std::size_t{22});
    CHECK(crowd.skeletons().size() == std::size_t{3});
    CHECK(crowd.report().unknownClassInstances == 0);
    // Street_stand names two clips (Stand_Cheer_08 and _10) that its
    // skeleton does not list; the game leaves them out of the class, and
    // so does the load.
    CHECK(crowd.report().failures.size() == std::size_t{0});
    for (const sp::CrowdClass& crowdClass : crowd.classes()) {
        if (crowdClass.name == "Street_stand") {
            CHECK(crowdClass.cheers.size() == std::size_t{10});
        }
    }
    CHECK((crowd.report().legacyFile.has_value() && crowd.report().legacyFile->entryCount == 0));

    // The people as the set's init places them.
    std::size_t riders = 0;
    std::size_t billboardOnly = 0;
    for (const sp::Spectator& person : crowd.spectators()) {
        CHECK(person.animation >= 0);
        if (person.animation >= 0) {
            CHECK(person.startFrame < crowd.animations()[static_cast<std::size_t>(person.animation)].clip.frameCount);
        }
        CHECK((person.modelScale >= 0.95F && person.modelScale <= 1.05F));
        CHECK(person.spriteCell < 20 * 9);
        billboardOnly += person.billboardOnly ? 1 : 0;
        if (person.leader >= 0) {
            ++riders;
            CHECK(person.spriteCell < 0);
            const glm::vec3 carrier = crowd.spectators()[static_cast<std::size_t>(person.leader)].position;
            CHECK((carrier.x == person.position.x && carrier.z == person.position.z));
        }
    }
    // Every shoulder rider of the disc has a carrier under them.
    CHECK(riders == std::size_t{866});
    CHECK(billboardOnly > 0);
    for (const sp::CrowdClass& crowdClass : crowd.classes()) {
        CHECK(crowdClass.idleTable.size() == sp::draw::kClipSlots);
        CHECK(crowdClass.cheerTable.size() == (crowdClass.cheers.empty() ? std::size_t{0} : sp::draw::kClipSlots));
    }

    for (const sp::SpectatorAnimation& animation : crowd.animations()) {
        const sp::AnimationClip& clip = animation.clip;
        CHECK_THAT(clip.framesPerSecond(), WithinAbs(30.0, 0.05));
        // Bones turn at most about 55 degrees between frames (a fist pump's
        // elbow); a misread stream would jump anywhere.
        float largest = 0.0F;
        for (int f = 1; f < clip.frameCount; ++f) {
            for (std::size_t b = 0; b < clip.boneCount(); ++b) {
                largest = std::max(
                    largest, 1.0F - std::abs(glm::dot(clip.pose(f, b).rotation, clip.pose(f - 1, b).rotation)));
            }
        }
        CHECK(largest < 0.12F);
    }

    for (const sp::SpectatorModel& model : crowd.models()) {
        CHECK(model.mesh.lods.size() == std::size_t{4});
        CHECK((model.height > (model.seated ? 1.2F : 1.7F) && model.height < 1.9F));
        const sp::SpectatorTexture& texture = crowd.textures()[model.texture];
        CHECK(texture.chain.width == 256);
        CHECK(texture.chain.levels.size() == std::size_t{9});
    }
    const int sprites = crowd.spriteTexture();
    CHECK(sprites >= 0);
    if (sprites >= 0) {
        CHECK(crowd.textures()[static_cast<std::size_t>(sprites)].chain.width == 2048);
    }
    CHECK(crowd.shadowTexture() >= 0);
    CHECK((crowd.spriteMetrics().cellHeightMetres() > 2.0F && crowd.spriteMetrics().cellHeightMetres() < 3.5F));

    // The bind pose clip leaves the mesh where it is.
    const int idle = crowd.findAnimation("Stand_Idle_01");
    CHECK(idle >= 0);
    if (idle >= 0) {
        std::uint32_t standing = 0;
        while (crowd.models()[standing].seated) {
            ++standing;
        }
        const std::vector<glm::vec3> skinned = crowd.skinnedPositions(standing, static_cast<std::uint32_t>(idle), 0.0F);
        const float* v = crowd.models()[standing].mesh.vertices.data();
        CHECK(near(skinned[10], {v[10 * 13], v[10 * 13 + 1], v[10 * 13 + 2]}, 1e-4F));
    }

    // The festival's own crowd stands around FESTIVAL_FIRST.
    const std::vector<std::uint32_t> festival = crowd.spectatorsNear(-2088.7F, -50.5F, 250.0F);
    CHECK(festival.size() > 1000);
    std::size_t tagged = 0;
    for (const std::uint32_t i : festival) {
        tagged += crowd.groups()[crowd.spectators()[i].group].eventTag == "FESTIVAL_FIRST_15" ? 1 : 0;
    }
    CHECK(tagged > 0);
    CHECK(crowd.spectatorsFor(sp::CrowdSession::freeRoam()).size() < crowd.spectators().size());
}
