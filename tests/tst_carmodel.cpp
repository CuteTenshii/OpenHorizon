// Tests of the car model reader: the carbin parser and vertex decoding on
// files built here in the layout CarBin documents, the name rules, and, when
// FH1_GAME_DIR names an extracted disc, a few real cars.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "carmodel/CarBin.h"
#include "carmodel/CarMaterials.h"
#include "carmodel/CarModel.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using openhorizon::Bytes;
namespace carmodel = openhorizon::carmodel;
using carmodel::CarBin;

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
    void zeros(std::size_t count) { data.insert(data.end(), count, 0); }
    void string(std::string_view s)
    {
        u32(static_cast<std::uint32_t>(s.size()));
        data.insert(data.end(), s.begin(), s.end());
    }
};

/// One vertex of the 28-byte format: a unit direction and a length that,
/// times the part box's half diagonal, give the position; texture
/// coordinates; the identity tangent frame; lighting bytes.
struct TestVertex {
    std::int16_t dx = 0;
    std::int16_t dy = 0;
    std::int16_t dz = 0;
    std::int16_t length = 0;
    std::uint16_t u = 0;
    std::uint16_t v = 0;
};

void writeVertex(Writer& w, const TestVertex& vertex)
{
    w.s16(vertex.dx);
    w.s16(vertex.dy);
    w.s16(vertex.dz);
    w.s16(vertex.length);
    w.u16(vertex.u);
    w.u16(vertex.v);
    w.u32(0);
    // Quaternion (x, y, z, w) = (0, 0, 0, 1): the normal is +X.
    w.s16(0);
    w.s16(0);
    w.s16(0);
    w.s16(32767);
    w.u8(106);
    w.u8(127);
    w.u8(127);
    w.u8(127);
}

struct TestPart {
    std::string name = "rotorLF";
    std::string material = "rotor";
    std::vector<TestVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::uint32_t primitive = 6;
    /// Leave the data out, as the `stripped_` files do.
    bool stripped = false;
};

void writeVertexBuffer(Writer& w, std::uint32_t count, bool stripped, const std::vector<TestVertex>& vertices)
{
    w.u32(3);
    w.u32(count);
    w.u32(count > 0 ? 28 : 0);
    w.u32(count > 0 ? (stripped ? 3 : 1) : 0);
    if (count > 0) {
        w.u32(0x12345678);
        if (!stripped) {
            for (const TestVertex& vertex : vertices) {
                writeVertex(w, vertex);
            }
        }
    }
}

void writePart(Writer& w, const TestPart& part)
{
    w.u32(5);
    // Position (0, 1, 2), box half extents (1, 2, 2): half diagonal 3.
    w.f32(0.0F);
    w.f32(1.0F);
    w.f32(2.0F);
    w.f32(-1.0F);
    w.f32(-2.0F);
    w.f32(-2.0F);
    w.f32(1.0F);
    w.f32(2.0F);
    w.f32(2.0F);
    w.f32(1.0F);
    w.f32(0.0F);
    w.f32(0.0F);
    w.f32(0.0F);
    w.f32(0.0F);
    // Glass section: two empty arrays.
    w.u32(1);
    w.u32(1);
    w.u32(0);
    w.u32(1);
    w.u32(0);
    w.u8(0);
    w.u32(0);
    // Mesh group: name, an empty vertex buffer for LODs 1 and up, meshes.
    w.u32(1);
    w.string(part.name);
    writeVertexBuffer(w, 0, false, {});
    w.u32(1);
    w.u32(1);
    w.u32(3);
    w.u8(0);
    // Texture coordinate factors: u from 0.25 over 0.5, v from 0 over 1.
    w.f32(0.25F);
    w.f32(0.5F);
    w.f32(0.0F);
    w.f32(1.0F);
    w.zeros(12 * 4);
    w.u32(2);
    w.string(part.material);
    w.u32(0);
    w.u32(part.primitive);
    w.u32(0);
    w.u32(1);
    for (const float f : {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F}) {
        w.f32(f);
    }
    w.u32(4);
    w.u32(part.stripped ? 2 : 0);
    w.u32(static_cast<std::uint32_t>(part.indices.size()));
    w.u32(2);
    if (!part.stripped) {
        for (const std::uint16_t index : part.indices) {
            w.u16(index);
        }
    }
    w.u32(0);
    // LOD 0 vertices, then the morph section.
    writeVertexBuffer(w, static_cast<std::uint32_t>(part.vertices.size()), part.stripped, part.vertices);
    w.u32(1);
    w.u8(0);
    writeVertexBuffer(w, 0, false, {});
    writeVertexBuffer(w, 0, false, {});
}

/// A brake file (the shortest preamble) holding `part`; `translucent` puts
/// its mesh in the translucent draw list.
Bytes brakeFile(const TestPart& part, bool translucent)
{
    Writer w;
    w.u32(1);
    // Header.
    w.u32(17);
    w.u32(16);
    w.u32(1);
    w.u32(0);
    w.u32(3);
    w.zeros(256);
    w.u32(0);
    w.u32(0);
    w.zeros(15 * 36);
    w.f32(-0.33F);
    w.f32(0.66F);
    w.u32(0);
    w.u32(1);
    w.u32(0);
    // Model and its draw lists.
    w.u32(4);
    w.u32(4);
    w.u32(1);
    w.u32(translucent ? 1 : 0);
    if (translucent) {
        w.u16(0);
        w.u16(0);
    }
    w.u32(translucent ? 0 : 1);
    if (!translucent) {
        w.u16(0);
        w.u16(0);
    }
    w.u32(1);
    w.u32(1);
    writePart(w, part);
    // Tail: three empty arrays.
    for (int i = 0; i < 3; ++i) {
        w.u32(1);
        w.u32(0);
    }
    return std::move(w.data);
}

TestPart quadPart()
{
    TestPart part;
    // Two corners along +X and -X at full length (3 · 1/3 = 1 from the
    // centre), one along +Y at a third of the length.
    part.vertices = {
        {32767, 0, 0, 10922, 0, 0},
        {0, 32767, 0, 10922, 65535, 0},
        {-32767, 0, 0, 10922, 0, 65535},
        {0, 0, 32767, 10922, 65535, 65535},
    };
    part.indices = {0, 1, 2, 3};
    return part;
}

TEST_CASE("appendTrianglesUnrollsStripsAndRestarts", "[carmodel]")
{
    std::vector<std::uint32_t> out;
    const std::vector<std::uint32_t> strip{0, 1, 2, 3, 0xFFFF, 4, 5, 6};
    carmodel::carbin::appendTriangles(strip, 6, 7, out);
    CHECK(out == (std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3, 4, 5, 6}));

    out.clear();
    const std::vector<std::uint32_t> list{0, 1, 2, 2, 2, 3};
    carmodel::carbin::appendTriangles(list, 4, 4, out);
    CHECK(out == (std::vector<std::uint32_t>{0, 1, 2}));
}

TEST_CASE("parseReadsPartMeshAndVertices", "[carmodel]")
{
    const TestPart part = quadPart();
    const Bytes file = brakeFile(part, false);
    const CarBin model = carmodel::carbin::parse(file, "test", CarBin::Kind::Brake);
    CHECK(model.parts.size() == std::size_t{1});
    CHECK(model.lodCount == 1U);
    CHECK_THAT(model.lightingRange.x, WithinAbs(-0.33, 1e-6));
    CHECK_THAT(model.lightingRange.y, WithinAbs(0.66, 1e-6));
    if (model.parts.empty()) {
        return;
    }
    const CarBin::Part& p = model.parts.front();
    CHECK(p.name == std::string("rotorLF"));
    CHECK(p.lod0Vertices.count == 4U);
    CHECK(p.vertices.empty());
    CHECK(p.meshes.size() == std::size_t{1});
    const CarBin::Mesh& mesh = p.meshes.front();
    CHECK(mesh.name == std::string("rotor"));
    CHECK(!mesh.translucent);
    CHECK(mesh.triangles == (std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
    CHECK_THAT(mesh.uv0.x, WithinAbs(0.25, 1e-6));
    CHECK_THAT(mesh.uv0.y, WithinAbs(0.5, 1e-6));

    // Half diagonal |(1, 2, 2)| = 3; a third of it along +X from (0, 1, 2).
    const carmodel::Vertex v0 = carmodel::carbin::decodeVertex(p, p.verticesFor(mesh), 0);
    CHECK_THAT(v0.position.x, WithinAbs(1.0, 1e-3));
    CHECK_THAT(v0.position.y, WithinAbs(1.0, 1e-3));
    // Z is negated into car space.
    CHECK_THAT(v0.position.z, WithinAbs(-2.0, 1e-3));
    const carmodel::Vertex v3 = carmodel::carbin::decodeVertex(p, p.verticesFor(mesh), 3);
    CHECK_THAT(v3.position.z, WithinAbs(-3.0, 1e-3));
    CHECK_THAT(v0.normal.x, WithinAbs(1.0, 1e-4));
    CHECK_THAT(v0.tangent.y, WithinAbs(1.0, 1e-4));
    CHECK(v0.lighting[0] == std::uint8_t{106});

    const glm::vec2 uv = carmodel::carbin::meshUv0(mesh, carmodel::carbin::decodeVertex(p, p.verticesFor(mesh), 1));
    CHECK_THAT(uv.x, WithinAbs(0.75, 1e-5));
    CHECK_THAT(uv.y, WithinAbs(0.0, 1e-5));
}

TEST_CASE("parseMarksTranslucentMeshesFromDrawLists", "[carmodel]")
{
    const Bytes file = brakeFile(quadPart(), true);
    const CarBin model = carmodel::carbin::parse(file, "test", CarBin::Kind::Brake);
    CHECK((!model.parts.empty() && model.parts.front().meshes.front().translucent));
}

TEST_CASE("parseReadsStrippedFiles", "[carmodel]")
{
    TestPart part = quadPart();
    part.stripped = true;
    const Bytes file = brakeFile(part, false);
    const CarBin model = carmodel::carbin::parse(file, "stripped", CarBin::Kind::Brake);
    CHECK(!model.parts.empty());
    if (!model.parts.empty()) {
        const CarBin::Part& p = model.parts.front();
        CHECK(p.lod0Vertices.stripped);
        CHECK(p.lod0Vertices.count == 4U);
        CHECK(p.lod0Vertices.data.empty());
        CHECK(p.meshes.front().indicesStripped);
        CHECK(p.meshes.front().indexCount == 4U);
        CHECK(p.meshes.front().triangles.empty());
    }
}

TEST_CASE("parseRejectsDamagedFiles", "[carmodel]")
{
    const Bytes file = brakeFile(quadPart(), false);
    Bytes trailing = file;
    trailing.push_back(0);
    CHECK_THROWS_AS(carmodel::carbin::parse(trailing, "trailing", CarBin::Kind::Brake), std::exception);
    const Bytes truncated(file.begin(), file.end() - 5);
    CHECK_THROWS_AS(carmodel::carbin::parse(truncated, "truncated", CarBin::Kind::Brake), std::exception);
    // A brake file read as a wheel meets the wrong preamble version.
    CHECK_THROWS_AS(carmodel::carbin::parse(file, "kind", CarBin::Kind::Wheel), std::exception);
}

TEST_CASE("parseReadsCarPreambleLights", "[carmodel]")
{
    const Bytes brake = brakeFile(quadPart(), false);
    Writer w;
    w.u32(5);
    w.zeros(256 + 16 + 48 + 16);
    w.u32(1);
    w.f32(-0.5F);
    w.f32(0.25F);
    w.f32(2.0F);
    w.s16(0);
    w.s16(0);
    w.s16(32767);
    w.zeros(14);
    // The rest is the brake file after its one-word preamble.
    w.data.insert(w.data.end(), brake.begin() + 4, brake.end());
    const CarBin model = carmodel::carbin::parse(w.data, "car", CarBin::Kind::Car);
    CHECK(model.lights.size() == std::size_t{1});
    if (!model.lights.empty()) {
        CHECK_THAT(model.lights.front().position.z, WithinAbs(2.0, 1e-6));
        CHECK_THAT(model.lights.front().direction.z, WithinAbs(1.0, 1e-4));
    }
}

TEST_CASE("kindFromPathFollowsFolders", "[carmodel]")
{
    CHECK(carmodel::carbin::kindFromPath("media/wheels/alf_8c_08/alf_8c_08.carbin") == CarBin::Kind::Wheel);
    CHECK(carmodel::carbin::kindFromPath("media/cars/alf_8c_08/alf_8c_08_rotorlf_lod0.carbin") == CarBin::Kind::Brake);
    CHECK(carmodel::carbin::kindFromPath("media/brakes/10_rotor_steel.rmb.carbin") == CarBin::Kind::Brake);
    CHECK(carmodel::carbin::kindFromPath("media/cars/alf_8c_08/alf_8c_08_lod0.carbin") == CarBin::Kind::Car);
}

TEST_CASE("partVariantsFollowUpgradeNames", "[carmodel]")
{
    CHECK(carmodel::partVariant("body").stock);
    CHECK(carmodel::partVariant("bumperFa").stock);
    CHECK(carmodel::partVariant("bumperFa").slot == std::string("bumperF"));
    CHECK(!carmodel::partVariant("bumperFb").stock);
    CHECK(!carmodel::partVariant("bumperFrace").stock);
    CHECK(carmodel::partVariant("bumperFrace").variant == std::string("race"));
    CHECK(carmodel::partVariant("bumperFrameF").variant.empty());
    CHECK(carmodel::partVariant("winga_arm1").stock);
    CHECK(carmodel::partVariant("exhaustLb").slot == std::string("exhaustL"));
    CHECK(!carmodel::partVariant("cagerace").stock);
    CHECK(!carmodel::partVariant("lexanLHL").stock);
}

TEST_CASE("techniqueNamesFollowTheExecutable", "[carmodel]")
{
    CHECK(carmodel::techniqueName("body", "bumperFa") == std::string("body"));
    // Brake parts draw as rotor or caliper whatever their material.
    CHECK(carmodel::techniqueName("chrome", "rotorLF") == std::string("rotor"));
    CHECK(carmodel::techniqueName("metal", "caliperRR") == std::string("caliper"));
    CHECK(carmodel::techniqueName("brake_badge", "caliperRR") == std::string("brake_badge"));
    // Cockpit parts, matched without regard to case.
    CHECK(carmodel::techniqueName("leather", "interior") == std::string("cockpit_leather"));
    CHECK(carmodel::techniqueName("plastic", "doorcardLF") == std::string("cockpit_plastic"));
    CHECK(carmodel::techniqueName("leather", "headlightL") == std::string("leather"));
    // Tyres and rims get _V2 only with the builder's flag clear.
    CHECK(carmodel::techniqueName("rim", "wheel") == std::string("rim"));
    CHECK(carmodel::techniqueName("rim", "wheel", false) == std::string("rim_V2"));
    CHECK(carmodel::techniqueName("wheel_black", "wheel", false) == std::string("wheel_black_V2"));
    CHECK(carmodel::techniqueName("window", "glassF") == std::string("frontwindow"));
    CHECK(carmodel::techniqueName("window_2", "lexanF") == std::string("frontwindow_2"));
    CHECK(carmodel::techniqueName("window", "glassLF") == std::string("window"));
    CHECK(carmodel::missingTechnique("tire") == std::string("missingTire"));
    CHECK(carmodel::missingTechnique("tire", false) == std::string("missingTire_V2"));
    CHECK(carmodel::missingTechnique("body") == std::string("missing"));
    CHECK(carmodel::isTyreMaterial("sidewall"));
    CHECK(carmodel::isTyreMaterial("ghostTire_x"));
    CHECK(carmodel::isRimMaterial("blur_lip"));
    CHECK(!carmodel::isRimMaterial("chrome"));
    CHECK(carmodel::isMotionBlurMaterial("blur_rim"));
}

// The rear window's heating wires drew as opaque dark bands across the
// glass: their mesh covers the window in strips and its texture is clear
// (alpha 0) except for the wire, which the game keeps by alpha to mask.
TEST_CASE("defrostLinesDrawMaskedAndUnculled", "[carmodel]")
{
    const carmodel::MaterialDrawState defrost = carmodel::materialDrawState("defrost_lines");
    CHECK(defrost.alphaToMask);
    CHECK(!defrost.cull);
    CHECK(!defrost.blend);
    CHECK(carmodel::materialDrawState("Defrost_Lines_2").alphaToMask);
    const carmodel::MaterialDrawState window = carmodel::materialDrawState("window");
    CHECK(!window.alphaToMask);
    CHECK(window.cull);
    CHECK(window.blend);
    CHECK(carmodel::castsNoShadow("defrost_lines"));
    CHECK(carmodel::castsNoShadow("liverybadge"));
    CHECK(!carmodel::castsNoShadow("window"));
}

TEST_CASE("loadFlagsEvaluateTextureTableExpressions", "[carmodel]")
{
    const std::vector<std::string> set{"LOD0", "Cockpit"};
    CHECK(carmodel::loadFlagsMatch("", set));
    CHECK(carmodel::loadFlagsMatch("LOD0", set));
    CHECK(carmodel::loadFlagsMatch("Cockpit & !SuperLOD", set));
    CHECK(!carmodel::loadFlagsMatch("Cockpit & SuperLOD", set));
    CHECK(!carmodel::loadFlagsMatch("LOD15", set));
    CHECK(carmodel::loadFlagsMatch("!SuperLOD", set));
}

TEST_CASE("realCarsLoad", "[carmodel]")
{
    const char* gameDir = openhorizon::test::gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(gameDir);
    std::optional<openhorizon::Database> db;
    if (const std::optional<Bytes> image = vfs.read("media/db/gamedb.slt")) {
        db.emplace(*image);
    }

    // Every carbin of one car parses.
    for (const std::string& path : vfs.list("media/cars/alf_8c_08")) {
        if (path.ends_with(".carbin") && path.find("stripped_") == std::string::npos) {
            const CarBin bin = carmodel::loadCarBin(vfs, path, carmodel::carbin::kindFromPath(path));
            CHECK(!bin.parts.empty());
        }
    }

    const carmodel::CarModel alfa = carmodel::CarModel::load(vfs, db ? &*db : nullptr, "ALF_8C_08");
    CHECK(alfa.warnings.empty());
    for (const std::string& warning : alfa.warnings) {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
    CHECK(alfa.mesh.triangleCount() > 150000);
    CHECK(alfa.wheels.size() == std::size_t{4});
    CHECK(alfa.mesh.vertices.size() % carmodel::carbin::kVertexStride == std::size_t{0});
    // The collision box of maxdata.xml, less the door mirrors.
    CHECK_THAT(alfa.mesh.boundsMin.y, WithinAbs(-0.030, 0.02));
    CHECK_THAT(alfa.mesh.boundsMax.y, WithinAbs(1.228, 0.02));
    CHECK_THAT(alfa.mesh.boundsMin.z, WithinAbs(-2.167, 0.02));
    CHECK_THAT(alfa.mesh.boundsMax.z, WithinAbs(2.200, 0.02));
    CHECK(!alfa.textures.empty());
    for (const carmodel::CarModel::Texture& texture : alfa.textures) {
        CHECK(!texture.chain.levels.empty());
    }
    for (const carmodel::WheelMount& wheel : alfa.wheels) {
        CHECK_THAT(std::abs(wheel.centre.z), WithinAbs(2.654 / 2.0, 0.01));
        CHECK((wheel.radius > 0.3F && wheel.radius < 0.4F));
    }
    // Every batch draws with a technique of the car effect, and the
    // samplers it reads find their files.
    CHECK(alfa.effect != nullptr);
    for (const carmodel::CarMesh::Batch& batch : alfa.mesh.batches) {
        CHECK(!batch.technique.starts_with("missing"));
        if (alfa.effect) {
            CHECK(!alfa.effect->passes(batch.technique).empty());
        }
        if (batch.technique == "body") {
            CHECK(std::any_of(batch.textures.begin(), batch.textures.end(), [&](const auto& t) {
                return t.first == "baseSampler" && alfa.textures[t.second].path.ends_with("nodamage_lod0.xds");
            }));
        }
    }
    CHECK(alfa.settings.contains("Metallic"));
    const carmodel::ShaderSettings::Parameters* body = alfa.settings.at("Metallic").technique("body");
    CHECK(body != nullptr);
    if (body != nullptr) {
        // metallic.xml's PaintScale over normal.xml's.
        CHECK_THAT(body->at("PaintScale").x, WithinAbs(0.5, 1e-6));
    }

    carmodel::CarModelOptions geometryOnly;
    geometryOnly.textures = false;
    for (const char* name : {"VW_Beetle_68", "KEN_T440_12", "BUS_TOUR_12"}) {
        const carmodel::CarModel car = carmodel::CarModel::load(vfs, db ? &*db : nullptr, name, geometryOnly);
        CHECK(car.mesh.triangleCount() > 10000);
        CHECK(car.wheels.size() == std::size_t{4});
        // Nothing far below the ground once the wheelbase centre is the origin.
        CHECK(car.mesh.boundsMin.y > -0.4F);
    }
}

} // namespace
