// Tests of the world formats: pure functions and parsers on data built here
// in the layouts the headers document, a small world written to a real
// archive, and, when FH1_GAME_DIR names an extracted disc, Colorado itself.

#include "TestHarness.h"

#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/PvsFile.h"
#include "world/RenderMesh.h"
#include "world/ScatterSet.h"
#include "world/ShadedTiles.h"
#include "world/TrackPlacements.h"
#include "world/TrackShaders.h"
#include "world/TrackTextures.h"
#include "world/TrackWorld.h"
#include "world/WorldIndex.h"
#include "world/WorldTiles.h"
#include "world/XboxTexture.h"
#include "world/ZoneGrid.h"

#include <lzx/zip.h>
#include <lzx/zip_writer.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using fh::Bytes;
namespace world = fh::world;

/// Builds big-endian test data.
struct Writer {
    Bytes data;

    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint16_t v)
    {
        u8(static_cast<std::uint8_t>(v >> 8));
        u8(static_cast<std::uint8_t>(v));
    }
    void u32(std::uint32_t v)
    {
        u16(static_cast<std::uint16_t>(v >> 16));
        u16(static_cast<std::uint16_t>(v));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(std::string_view s) { data.insert(data.end(), s.begin(), s.end()); }
    void fill(std::size_t count, std::uint8_t value) { data.insert(data.end(), count, value); }
    void padTo(std::size_t size) { fill(size > data.size() ? size - data.size() : 0, 0); }
    void alignTo(std::size_t alignment) { padTo((data.size() + alignment - 1) / alignment * alignment); }
    void patch32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i) {
            data[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        }
    }
};

/// IEEE 754 half-precision bits of `value`, for the values tests use (whole
/// numbers and simple fractions, which half floats hold exactly).
std::uint16_t halfBits(float value)
{
    if (value == 0.0F) {
        return 0;
    }
    const std::uint16_t sign = value < 0.0F ? 0x8000 : 0;
    int exponent = 0;
    const float fraction = std::frexp(std::abs(value), &exponent);
    const auto mantissa = static_cast<std::uint16_t>(std::lround((fraction * 2.0F - 1.0F) * 1024.0F));
    return static_cast<std::uint16_t>(sign | ((exponent - 1 + 15) << 10) | mantissa);
}

struct TestMaterial {
    std::vector<std::uint32_t> indices;
    std::uint32_t width = 2;
    std::uint32_t tableIndex = 0;
    /// Texture coordinate offset u, v and scale u, v.
    std::array<float, 4> uvOffsetScale{0.0F, 0.0F, 1.0F, 1.0F};
    /// Position offset and scale x, y, z.
    std::array<float, 3> positionOffset{0.0F, 0.0F, 0.0F};
    std::array<float, 3> positionScale{1.0F, 1.0F, 1.0F};
    std::string name = "Mat0";
};

struct TestPart {
    std::string name;
    /// Positions in file coordinates (Z not yet flipped).
    std::vector<glm::vec3> positions;
    /// Raw 16-bit texture coordinates written at byte 16 of each vertex,
    /// when the stride has room for them.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> texcoords;
    std::vector<TestMaterial> materials;
    std::uint32_t stride = 16;
};

/// An entry of the material table after the parts.
struct TestMaterialInfo {
    std::uint32_t shader = 0;
    /// -1 is written as an engine-supplied slot.
    std::vector<int> textureSlots;
    std::vector<glm::vec4> constants;
};

struct TestTail {
    std::vector<TestMaterialInfo> materials;
    std::vector<std::string> shaders;
};

/// A render model file in the layout RenderMesh documents; without a tail
/// it ends in bytes the parser cannot read as a material table.
Bytes renderModel(const std::vector<TestPart>& parts, const std::optional<TestTail>& tail = std::nullopt)
{
    glm::vec3 lo(1e9F);
    glm::vec3 hi(-1e9F);
    for (const TestPart& part : parts) {
        for (const glm::vec3& p : part.positions) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    }
    Writer w;
    w.u32(6);
    w.f32(lo.x);
    w.f32(lo.y);
    w.f32(lo.z);
    w.u32(0);
    w.f32(hi.x);
    w.f32(hi.y);
    w.f32(hi.z);
    w.u32(0);
    for (int i = 0; i < 16; ++i) {
        w.f32(i % 5 == 0 ? 1.0F : 0.0F);
    }
    w.u32(4);
    w.u32(4);
    w.u32(0);
    w.u32(1);
    w.u32(static_cast<std::uint32_t>(parts.size()));
    w.padTo(0xB0);
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const TestPart& part = parts[p];
        if (p > 0) {
            w.fill(48, 0);
            w.u32(1);
        }
        w.u32(static_cast<std::uint32_t>(part.name.size()));
        w.text(part.name);
        w.u32(3);
        w.u32(static_cast<std::uint32_t>(part.positions.size()));
        w.u32(part.stride);
        w.u32(0);
        for (std::size_t v = 0; v < part.positions.size(); ++v) {
            const std::size_t start = w.data.size();
            w.f32(part.positions[v].x);
            w.f32(part.positions[v].y);
            w.f32(part.positions[v].z);
            w.fill(part.stride - 12, 0x7F);
            if (v < part.texcoords.size() && part.stride >= 20) {
                Writer uv;
                uv.u16(part.texcoords[v].first);
                uv.u16(part.texcoords[v].second);
                std::copy(uv.data.begin(), uv.data.end(), w.data.begin() + static_cast<std::ptrdiff_t>(start + 16));
            }
        }
        w.u32(1);
        w.u32(static_cast<std::uint32_t>(part.materials.size()));
        w.u32(1);
        for (const TestMaterial& material : part.materials) {
            w.u32(2);
            w.u32(static_cast<std::uint32_t>(material.name.size()));
            w.text(material.name);
            w.u32(0);
            w.u32(6);
            w.u32(material.tableIndex);
            w.u32(1);
            for (const float f : material.positionOffset) {
                w.f32(f);
            }
            w.f32(0.0F);
            for (const float f : material.positionScale) {
                w.f32(f);
            }
            w.f32(0.0F);
            for (const float f : material.uvOffsetScale) {
                w.f32(f);
            }
            w.u32(4);
            w.u32(0);
            w.u32(static_cast<std::uint32_t>(material.indices.size()));
            w.u32(material.width);
            for (const std::uint32_t index : material.indices) {
                if (material.width == 2) {
                    w.u16(static_cast<std::uint16_t>(index));
                } else {
                    w.u32(index);
                }
            }
            w.u32(5);
            w.u32(1);
        }
    }
    if (!tail) {
        w.text("trailing shader data");
        return w.data;
    }
    w.u32(1);
    w.u32(1);
    w.u32(1);
    w.u32(static_cast<std::uint32_t>(tail->materials.size()));
    for (const TestMaterialInfo& info : tail->materials) {
        w.u32(3);
        w.u32(info.shader);
        w.u32(0);
        w.u32(1);
        w.u32(0);
        w.u32(1);
        if (info.constants.empty()) {
            w.u32(1);
            w.fill(16, 0);
        } else {
            w.u32(static_cast<std::uint32_t>(info.constants.size()));
            for (const glm::vec4& c : info.constants) {
                for (int k = 0; k < 4; ++k) {
                    w.f32(c[k]);
                }
            }
        }
        w.u32(1);
        w.u32(static_cast<std::uint32_t>(info.textureSlots.size()));
        for (const int slot : info.textureSlots) {
            w.u32(slot < 0 ? 0xFFFFFFFEU : static_cast<std::uint32_t>(slot));
        }
    }
    w.u32(1);
    w.u32(static_cast<std::uint32_t>(tail->shaders.size()));
    for (const std::string& shader : tail->shaders) {
        w.u32(static_cast<std::uint32_t>(shader.size() + 1));
        w.text(shader);
        w.u8(0);
    }
    w.text("compiled shader code");
    return w.data;
}

/// A flat quad at height `y` centred on (cx, cz) in world coordinates, as a
/// four-index strip.
TestPart quad(std::string name, float cx, float cz, float y, float half)
{
    TestPart part;
    part.name = std::move(name);
    // File Z is world Z negated.
    part.positions = {{cx - half, y, -(cz - half)}, {cx + half, y, -(cz - half)}, {cx - half, y, -(cz + half)},
        {cx + half, y, -(cz + half)}};
    TestMaterial material;
    material.indices = {0, 1, 2, 3};
    part.materials = {material};
    return part;
}

/// A compiled shader whose vertex inputs are `inputs`, and whose pixel
/// shader constant table names `samplers` (register, name).
Bytes shaderObject(
    const std::vector<std::uint32_t>& inputs, const std::vector<std::pair<std::uint16_t, std::string>>& samplers = {})
{
    Writer w;
    w.text("constant table");
    w.text(std::string_view("vs_3_0", 7));
    w.text(std::string_view("2.0.21119.0", 12));
    w.alignTo(4);
    w.fill(20, 0);
    w.u32(0x290);
    w.u32(0x00100000U | static_cast<std::uint32_t>(inputs.size() + 1));
    for (const std::uint32_t input : inputs) {
        w.u32(input);
    }
    w.u32(0x7030);
    if (samplers.empty()) {
        return w.data;
    }
    w.fill(12, 0x55);
    const auto count = static_cast<std::uint32_t>(samplers.size());
    const std::uint32_t records = 28;
    const std::uint32_t strings = records + count * 20;
    std::string names;
    std::vector<std::uint32_t> nameOffsets;
    for (const auto& [reg, name] : samplers) {
        nameOffsets.push_back(strings + static_cast<std::uint32_t>(names.size()));
        names += name;
        names.push_back('\0');
    }
    const std::uint32_t target = strings + static_cast<std::uint32_t>(names.size());
    w.u32(28);
    w.u32(target);
    w.u32(0xFFFF0300);
    w.u32(count);
    w.u32(records);
    w.u32(0);
    w.u32(target);
    for (std::size_t i = 0; i < samplers.size(); ++i) {
        w.u32(nameOffsets[i]);
        w.u16(3);
        w.u16(samplers[i].first);
        w.u16(1);
        w.u16(0);
        w.u32(0);
        w.u32(0);
    }
    w.text(names);
    w.text(std::string_view("ps_3_0", 7));
    return w.data;
}

/// A PVS file whose texture records hold `textureIds`, whose objects list
/// record numbers, and whose draw records place `drawObjects`.
Bytes pvsFile(const std::vector<std::uint32_t>& textureIds, const std::vector<std::vector<std::uint32_t>>& objects,
    const std::vector<std::uint16_t>& drawObjects = {0x5555})
{
    Writer w;
    w.text("FPVS");
    w.u32(0x32);
    w.padTo(0x20);
    w.u32(2);
    w.u32(0x1234);
    w.u32(0x5678);
    // The real file has seven unexplained bytes here.
    for (const std::uint8_t b : {0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00}) {
        w.u8(b);
    }
    w.u32(static_cast<std::uint32_t>(textureIds.size()));
    for (std::size_t i = 0; i < textureIds.size(); ++i) {
        w.u32(textureIds[i]);
        w.u32(static_cast<std::uint32_t>(i));
        w.f32(1.0F);
        w.f32(1.0F);
        w.u32(0);
        w.u32(0);
        w.u32(0x104);
    }
    w.u32(1);
    w.u32(8);
    w.text("h_diff_1");
    w.u32(static_cast<std::uint32_t>(drawObjects.size()));
    for (const std::uint16_t object : drawObjects) {
        w.u16(object);
        w.fill(16, 0x55);
    }
    w.u32(static_cast<std::uint32_t>(objects.size()));
    for (const std::vector<std::uint32_t>& records : objects) {
        w.u32(static_cast<std::uint32_t>(records.size()));
        for (const std::uint32_t record : records) {
            w.u32(record);
        }
        w.u32(1);
        w.u32(0);
        for (int i = 0; i < 15; ++i) {
            w.f32(0.5F);
        }
    }
    w.text("more tables");
    return w.data;
}

struct TestPlacement {
    std::uint16_t draw = 0;
    glm::vec3 position{0.0F};
    std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> ranges{-1, -1, -1};
    /// The id of the record's extra block; 0xFFFFFFFF marks an event prop.
    std::uint32_t blockId = 0xA8;
    std::string blockName;
    /// The block's m bytes: the race routes an event prop is put out for.
    std::vector<std::uint8_t> blockBytes{'x', 'y'};
};

/// A zone file in the layout TrackPlacements documents, with a few entries
/// in the sections the reader skips.
Bytes zoneFile(const std::vector<TestPlacement>& placements)
{
    Writer w;
    const auto count = static_cast<std::uint32_t>(placements.size());
    w.u32(count);
    for (const TestPlacement& p : placements) {
        w.u32(0x80000000U | p.draw);
    }
    w.u32(2);
    w.u16(7);
    w.u16(8);
    w.u32(1);
    w.fill(16, 0x11);
    w.u32(1);
    w.u32(0x22222222);
    w.u32(1);
    w.u8(0x33);
    w.u32(1);
    w.u16(9);
    w.u32(1);
    w.u32(0x44444444);
    w.u32(1);
    w.u8(0x55);
    w.u32(0);
    w.u32(count);
    w.fill(count, 0x66);
    w.u32(count);
    for (const TestPlacement& p : placements) {
        for (const float range : p.ranges) {
            w.u16(halfBits(range));
        }
        w.f32(p.position.x);
        w.f32(p.position.y);
        w.f32(p.position.z);
        for (const float value : p.rows) {
            w.u16(halfBits(value));
        }
        w.fill(16, 0);
        w.u8(1);
        w.u32(p.blockId);
        w.u8(static_cast<std::uint8_t>(p.blockBytes.size()));
        w.data.insert(w.data.end(), p.blockBytes.begin(), p.blockBytes.end());
        std::string name = p.blockName;
        name.resize(32, '\0');
        w.text(name);
    }
    return w.data;
}

struct TestScatterMesh {
    /// Draw record per level of detail; empty for a mesh named by `path`.
    std::vector<std::uint32_t> draws;
    std::string path;
};

struct TestScatterInstance {
    glm::vec3 position{0.0F};
    /// X, Y and Z axes, scaled, as the set stores them.
    std::array<glm::vec3, 3> axes{glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
    /// The group's block of distances it takes (byte 95 holds twice it).
    std::uint8_t block = 0;
};

struct TestScatterGroup {
    std::uint32_t mesh = 0;
    std::vector<TestScatterInstance> instances;
    float drawDistance = 220.0F;
    /// The level ends E0, E1 and E2 of each block.
    std::vector<std::array<float, 3>> blocks;
};

/// Whether each batch's chunk runs cover its indices exactly, in order.
template <typename Batch> bool runsCoverBatches(const std::vector<Batch>& batches)
{
    for (const Batch& batch : batches) {
        std::uint32_t next = batch.firstIndex;
        for (const world::ChunkRun& run : batch.runs) {
            if (run.firstIndex != next || run.indexCount == 0) {
                return false;
            }
            next += run.indexCount;
        }
        if (next != batch.firstIndex + batch.indexCount) {
            return false;
        }
    }
    return true;
}

/// A procedural placement set in the layout ScatterSet documents.
Bytes scatterFile(const std::string& name, const std::vector<TestScatterMesh>& meshes,
    const std::vector<TestScatterGroup>& groups, std::uint32_t extraEntries = 2)
{
    Writer w;
    w.text("OEGP");
    w.u32(42);
    w.padTo(0x30);
    w.u32(7);
    w.f32(-1.0F);
    w.f32(2500.0F);
    w.u32(22);
    w.u32(extraEntries);
    w.u32(0);
    w.u32(0); // file size, patched below
    w.u32(0);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(meshes.size()));
    w.fill(8, 0);
    std::string paddedName = name;
    paddedName.resize(0x1C, '\0');
    w.text(paddedName);
    w.u32(0);
    for (const TestScatterMesh& mesh : meshes) {
        for (const float v : {-1.0F, 0.0F, -1.0F, 0.0F, 1.0F, 5.0F, 1.0F, 0.0F}) {
            w.f32(v);
        }
        w.u32(static_cast<std::uint32_t>(mesh.path.size()));
        for (std::size_t slot = 0; slot < 3; ++slot) {
            w.u32(slot < mesh.draws.size() ? 1 : 0);
            w.u32(0x376AD290);
        }
        w.u32(0x14756A8);
    }
    for (const TestScatterMesh& mesh : meshes) {
        for (const std::uint32_t draw : mesh.draws) {
            w.u32(draw);
        }
    }
    for (const TestScatterMesh& mesh : meshes) {
        w.text(mesh.path);
    }
    w.alignTo(4);
    for (std::uint32_t e = 0; e < extraEntries; ++e) {
        w.u32(0x4C80 + e);
        w.u32(0);
    }
    w.u32(0x376AD608);
    w.f32(220.0F);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(groups.size()));
    w.u32(0);
    for (const TestScatterGroup& group : groups) {
        w.u32(0);
        w.u32(0xFFFFFFFF);
        for (const float v : {0.5F, 1.0F, 0.5F, 0.0F, 6.0F, 6.77F, 1.0F}) {
            w.f32(v);
        }
        w.u32(0);
        w.u32(1);
        w.u32(group.mesh);
        w.u32(static_cast<std::uint32_t>(group.instances.size()));
        w.u32(3);
        w.u32(static_cast<std::uint32_t>(group.blocks.size()));
        for (const float v : {group.drawDistance, 150.0F, 300.0F, group.drawDistance}) {
            w.f32(v);
        }
        w.fill(16, 0);
    }
    w.alignTo(16);
    for (const TestScatterGroup& group : groups) {
        for (const TestScatterInstance& instance : group.instances) {
            for (const glm::vec3& axis : instance.axes) {
                w.f32(axis.x);
                w.f32(axis.y);
                w.f32(axis.z);
                w.f32(0.0F);
            }
            for (const float v : {instance.position.x, instance.position.y, instance.position.z, 1.0F, 0.5F, 0.5F, 0.5F,
                     0.0F, 0.0F, 1.0F, 0.0F}) {
                w.f32(v);
            }
            w.u8(0);
            w.u8(0);
            w.u8(0);
            w.u8(static_cast<std::uint8_t>(instance.block * 2));
        }
        for (const std::array<float, 3>& ends : group.blocks) {
            for (const float v : {ends[0], ends[1], ends[2], 150.0F, 300.0F, 2500.0F, ends[2], 5.0F}) {
                w.f32(v);
            }
        }
    }
    w.fill(32, 0x5A);
    w.patch32(0x48, static_cast<std::uint32_t>(w.data.size()));
    return w.data;
}

/// A zone grid file in the layout ZoneGrid documents, with the origin at
/// 0, 0 and cells of radius 100.
Bytes hexFile(std::uint32_t zones, std::uint32_t columns, std::uint32_t rows, const std::vector<std::uint32_t>& cells)
{
    Writer w;
    w.text("HEXY");
    w.u32(101);
    w.f32(100.0F);
    w.f32(0.0F);
    w.f32(0.0F);
    w.u32(zones);
    w.u32(columns);
    w.u32(rows);
    for (const std::uint32_t cell : cells) {
        w.u32(cell);
    }
    w.fill(static_cast<std::size_t>(zones) * 9, 0x01);
    return w.data;
}

/// A temporary folder removed with everything in it when the object goes.
class TempDir {
public:
    TempDir()
    {
        std::random_device random;
        m_path = std::filesystem::temp_directory_path() / std::format("tst_world_{:08x}{:08x}", random(), random());
        std::filesystem::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

bool nearVec(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-3F)
{
    return glm::length(a - b) <= tolerance;
}

} // namespace

TEST(lodNamesAndGroups)
{
    using world::rendermesh::lodGroupKey;
    using world::rendermesh::lodLevel;
    CHECK_EQ(lodLevel("Redstone_Area02_Road_LOD02_01"), 2);
    CHECK_EQ(lodLevel("LOD00_Thing"), 0);
    CHECK_EQ(lodLevel("thing_lod13"), 13);
    CHECK_EQ(lodLevel("cables_NOLOD_3"), -1);
    CHECK_EQ(lodLevel("UberLOD23_CAGE"), -1);
    CHECK_EQ(lodLevel("Plain"), -1);
    CHECK_EQ(lodGroupKey("Redstone_Area02_Road_LOD01_02"), std::string("redstone_area02_road"));
    CHECK_EQ(lodGroupKey("Redstone_Area02_Road_LOD02_11"), std::string("redstone_area02_road"));
    CHECK_EQ(lodGroupKey("GrandstandStraight_LOD02_"), std::string("grandstandstraight"));
    CHECK_EQ(lodGroupKey("TERR_UberLOD_Patch07_LOD00"), std::string("terr_uberlod_patch07"));
    CHECK_EQ(lodGroupKey("Area12_4_7"), std::string("area12"));
    CHECK_EQ(lodGroupKey("Rock"), std::string("rock"));
}

TEST(stripsBecomeTriangleLists)
{
    std::vector<std::uint32_t> out;
    // A strip flips every other triangle back to the first one's winding.
    world::rendermesh::appendTriangles(std::vector<std::uint32_t>{0, 1, 2, 3}, 0xFFFF, out);
    CHECK_EQ(out, (std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3}));
    // A restart starts a new strip; degenerate triangles are dropped.
    out.clear();
    world::rendermesh::appendTriangles(std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0xFFFF, 4, 5, 6}, 0xFFFF, out);
    CHECK_EQ(out, (std::vector<std::uint32_t>{0, 1, 2, 4, 5, 6}));
    // Without restarts, a multiple of three is a list.
    out.clear();
    world::rendermesh::appendTriangles(std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2}, 0xFFFF, out);
    CHECK_EQ(out, (std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2}));
    // ... and any other length a strip.
    out.clear();
    world::rendermesh::appendTriangles(std::vector<std::uint32_t>{5, 6, 7, 8, 9}, 0xFFFFFFFF, out);
    CHECK_EQ(out, (std::vector<std::uint32_t>{5, 6, 7, 7, 6, 8, 7, 8, 9}));
    // A list's degenerate triangles are dropped too.
    out.clear();
    world::rendermesh::appendTriangles(std::vector<std::uint32_t>{0, 0, 1, 1, 2, 3}, 0xFFFF, out);
    CHECK_EQ(out, (std::vector<std::uint32_t>{1, 2, 3}));
}

TEST(parsesRenderModels)
{
    TestPart strip = quad("Road_LOD01_02", 100.0F, 200.0F, 5.0F, 10.0F);
    strip.materials[0].indices = {0, 1, 2, 3, 0xFFFF, 0, 1, 2};
    TestPart list;
    list.name = "Road_LOD01_02";
    list.stride = 36;
    list.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {1, 0, -1}, {2, 0, 0}, {2, 0, -1}};
    TestMaterial wide;
    wide.indices = {0, 1, 2, 1, 3, 2};
    wide.width = 4;
    TestMaterial narrow;
    narrow.indices = {1, 4, 3, 4, 5, 3};
    list.materials = {wide, narrow};
    const Bytes data = renderModel({strip, list});

    const world::RenderMesh mesh = world::rendermesh::parse(data, "test");
    CHECK_EQ(mesh.parts.size(), std::size_t{2});
    // Strip: 0-1-2, 2-1-3 (winding flipped back), restart, 0-1-2 again.
    CHECK_EQ(mesh.parts[0].materials[0].triangles.size(), std::size_t{9});
    CHECK_EQ(mesh.parts[0].materials[0].triangles[3], 2U);
    CHECK_EQ(mesh.parts[0].materials[0].triangles[4], 1U);
    CHECK_EQ(mesh.parts[1].stride, 36U);
    CHECK_EQ(mesh.parts[1].materials.size(), std::size_t{2});
    CHECK_EQ(mesh.parts[1].materials[0].triangles.size(), std::size_t{6});
    CHECK_EQ(mesh.parts[1].vertexData.size(), std::size_t{6 * 36});
    CHECK_EQ(mesh.triangleCount(), std::size_t{7});
    // Z is flipped into world coordinates.
    CHECK(nearVec(mesh.parts[0].positions[0], glm::vec3(90.0F, 5.0F, 190.0F)));
    CHECK(mesh.boundsMin.z <= 190.0F && mesh.boundsMax.z >= 210.0F);
    CHECK(mesh.boundsMin.z <= mesh.boundsMax.z);

    const std::optional<world::RenderMeshHeader> header = world::rendermesh::readHeader(
        fh::ByteView(data).first(std::min(data.size(), world::rendermesh::kHeaderBytes)));
    CHECK(header.has_value());
    if (header) {
        CHECK_EQ(header->partCount, 2U);
        CHECK_EQ(header->firstPartName, std::string("Road_LOD01_02"));
        CHECK(nearVec(header->boundsMin, mesh.boundsMin));
        CHECK(nearVec(header->boundsMax, mesh.boundsMax));
    }
    // The tail is not a material table; the geometry is still read.
    CHECK(mesh.materialTable.empty());
    CHECK(mesh.shaders.empty());
}

TEST(parsesMaterialTablesAndTexcoords)
{
    TestPart part = quad("Sign_LOD00", 0, 0, 0, 1);
    part.stride = 24;
    part.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {32768, 16384}};
    part.materials[0].tableIndex = 1;
    part.materials[0].uvOffsetScale = {0.5F, -0.25F, 2.0F, 4.0F};
    part.materials[0].positionOffset = {1.0F, 2.0F, 3.0F};
    part.materials[0].positionScale = {0.5F, 0.25F, 2.0F};
    TestMaterialInfo first;
    first.textureSlots = {-1};
    TestMaterialInfo second;
    second.shader = 1;
    second.textureSlots = {3, -1, -1, -1, -1, 0};
    second.constants = {glm::vec4(4, 4, 6, 6), glm::vec4(3, 3, 1, 1)};
    TestTail tail;
    tail.materials = {first, second};
    tail.shaders = {"shaders\\track\\add_diff_opac_rgba.fx", "shaders\\track\\h_diff_1.fx"};
    const world::RenderMesh mesh = world::rendermesh::parse(renderModel({part}, tail), "test");

    CHECK_EQ(mesh.materialTable.size(), std::size_t{2});
    CHECK_EQ(mesh.materialTable[1].shader, 1U);
    CHECK_EQ(mesh.materialTable[1].textureSlots, (std::vector<int>{3, -1, -1, -1, -1, 0}));
    CHECK(mesh.materialTable[1].constants == (std::vector<glm::vec4>{glm::vec4(4, 4, 6, 6), glm::vec4(3, 3, 1, 1)}));
    CHECK_EQ(mesh.shaders.size(), std::size_t{2});
    CHECK_EQ(mesh.shaders[1], std::string("shaders\\track\\h_diff_1.fx"));

    const world::RenderMesh::Part& parsed = mesh.parts[0];
    const world::RenderMesh::Material& material = parsed.materials[0];
    CHECK_EQ(material.tableIndex, 1U);
    CHECK(material.positionOffset == glm::vec3(1.0F, 2.0F, 3.0F));
    CHECK(material.positionScale == glm::vec3(0.5F, 0.25F, 2.0F));
    CHECK_EQ(parsed.vertexData.size(), std::size_t{96});
    CHECK(world::RenderMesh::texcoord(parsed, material, 0, 16) == glm::vec2(0.5F, -0.25F));
    CHECK(world::RenderMesh::texcoord(parsed, material, 1, 16) == glm::vec2(2.5F, -0.25F));
    CHECK(world::RenderMesh::texcoord(parsed, material, 2, 16) == glm::vec2(0.5F, 3.75F));
    const glm::vec2 mid = world::RenderMesh::texcoord(parsed, material, 3, 16);
    CHECK_NEAR(mid.x, 1.5, 1e-3);
    CHECK_NEAR(mid.y, 0.75, 1e-3);
    // A pair that would reach past the vertex is not read.
    CHECK(world::RenderMesh::texcoord(parsed, material, 0, 22) == glm::vec2(0.0F));
    CHECK_EQ(world::RenderMesh::colour(parsed, 0, 20), 0x7F7F7F7FU);
    CHECK_EQ(world::RenderMesh::colour(parsed, 4, 20), 0U);
}

TEST(rejectsMalformedModels)
{
    const Bytes good = renderModel({quad("Road_LOD01", 0, 0, 0, 1)});
    Bytes badVersion = good;
    badVersion[3] = 7;
    CHECK_THROWS(world::rendermesh::parse(badVersion, "test"));
    CHECK(!world::rendermesh::readHeader(badVersion).has_value());
    CHECK_THROWS(world::rendermesh::parse(fh::ByteView(good).first(good.size() / 2), "test"));
    TestPart pastEnd = quad("Road_LOD01", 0, 0, 0, 1);
    pastEnd.materials[0].indices = {0, 1, 2, 4};
    CHECK_THROWS(world::rendermesh::parse(renderModel({pastEnd}), "test"));
    TestPart badStride = quad("Road_LOD01", 0, 0, 0, 1);
    badStride.stride = 68;
    CHECK_THROWS(world::rendermesh::parse(renderModel({badStride}), "test"));
    CHECK(!world::rendermesh::readHeader(Bytes(0x40, 0)).has_value());
}

TEST(looksUpZonesInTheHexGrid)
{
    // Three columns and two rows; the middle cell of the second row has no
    // zone. Odd columns sit half a row further along Z.
    constexpr std::uint32_t kNone = 0xFFFFFFFF;
    const world::ZoneGrid grid = world::ZoneGrid::read(hexFile(5, 3, 2, {0, 1, 2, 3, kNone, 4}));
    CHECK_EQ(grid.zoneCount(), 5);
    CHECK_EQ(grid.columns(), 3);
    CHECK_EQ(grid.rows(), 2);
    const float rowPitch = std::sqrt(3.0F) * 100.0F;
    CHECK_EQ(grid.zoneAt(100.0F, rowPitch / 2), 0);
    CHECK_EQ(grid.zoneAt(250.0F, rowPitch), 1);
    CHECK_EQ(grid.zoneAt(400.0F, rowPitch / 2), 2);
    CHECK_EQ(grid.zoneAt(100.0F, rowPitch * 1.5F), 3);
    CHECK_EQ(grid.zoneAt(250.0F, rowPitch * 2), -1);
    CHECK_EQ(grid.zoneAt(400.0F, rowPitch * 1.5F), 4);
    // Near the edge between cells 0 and 1, on cell 0's side.
    CHECK_EQ(grid.zoneAt(170.0F, 120.0F), 0);
    // Below the odd column's first cell is outside the grid.
    CHECK_EQ(grid.zoneAt(250.0F, 60.0F), -1);
    CHECK_EQ(grid.zoneAt(-500.0F, 0.0F), -1);
    CHECK_EQ(world::ZoneGrid().zoneAt(0.0F, 0.0F), -1);

    Bytes wrongMagic = hexFile(1, 1, 1, {0});
    wrongMagic[0] = 'X';
    CHECK_THROWS(world::ZoneGrid::read(wrongMagic));
    CHECK_THROWS(world::ZoneGrid::read(hexFile(1, 1, 1, {3})));
    const Bytes truncated = hexFile(1, 2, 2, {0});
    CHECK_THROWS(world::ZoneGrid::read(fh::ByteView(truncated).first(40)));
}

TEST(readsZonePlacements)
{
    TestPlacement first;
    first.draw = 3;
    first.position = glm::vec3(-1670.5F, 12.5F, -1207.75F);
    first.rows = {0.5F, 0, 1, 0, 1, 0, 1, 0, -0.5F};
    first.ranges = {150, 300, 500};
    TestPlacement second;
    second.draw = 7;
    second.position = glm::vec3(10, 20, 30);
    second.blockId = 0xFFFFFFFF;
    second.blockName = "FR04_NODE";
    second.blockBytes = {0x93, 0xCE};
    TestPlacement flyer;
    flyer.draw = 9;
    flyer.blockId = 0xA2;
    flyer.blockName = "flyer_007";
    flyer.blockBytes = {0x00, 0x15};
    const auto placements = world::TrackPlacements::readZone(zoneFile({first, second, flyer}));
    CHECK_EQ(placements.size(), std::size_t{3});
    CHECK_EQ(placements[0].first, 3U);
    CHECK_EQ(placements[1].first, 7U);
    const world::Placement& p = placements[0].second;
    CHECK(p.position == first.position);
    CHECK(p.rows == first.rows);
    CHECK(p.ranges == first.ranges);
    CHECK(!p.eventProp);
    CHECK(p.eventTag.empty());
    const world::Placement& event = placements[1].second;
    CHECK(event.ranges == (std::array<float, 3>{-1, -1, -1}));
    CHECK(event.eventProp);
    CHECK_EQ(event.eventTag, std::string("FR04_NODE"));
    CHECK(event.eventRoutes == (std::vector<std::uint8_t>{147, 206}));
    CHECK(event.belongsToEvent("fr04"));
    CHECK(!event.belongsToEvent("FR0"));
    CHECK(event.onRoute(206));
    CHECK(!event.onRoute(12));
    CHECK(event.gameplayId.empty());
    CHECK(p.gameplayId.empty());
    const world::Placement& gameplay = placements[2].second;
    CHECK(!gameplay.eventProp);
    CHECK(gameplay.eventTag.empty());
    CHECK(gameplay.eventRoutes.empty());
    CHECK_EQ(gameplay.gameplayId, std::string("flyer_007"));

    Bytes truncated = zoneFile({first, second});
    truncated.resize(truncated.size() - 10);
    CHECK_THROWS(world::TrackPlacements::readZone(truncated));
    Bytes mismatch = zoneFile({first});
    mismatch[3] = 2;
    CHECK_THROWS(world::TrackPlacements::readZone(mismatch));
}

TEST(appliesPlacements)
{
    // World-space models are placed by an identity with Z mirrored, which
    // reproduces the parser's own coordinates.
    world::Placement asStored;
    asStored.rows = {1, 0, 0, 0, 1, 0, 0, 0, -1};
    CHECK(asStored.apply(glm::vec3(3, 4, 5)) == glm::vec3(3, 4, 5));

    // A marker pole from CollObjs.xml: its XAxis (0.164, 0, 0.986) is row 0
    // and its ZAxis (-0.986, 0, 0.164), negated, row 2.
    world::Placement pole;
    pole.position = glm::vec3(-1670.525F, 12.5432F, -1207.935F);
    pole.rows = {0.164F, 0, 0.986F, 0, 1, 0, 0.986F, 0, -0.164F};
    CHECK(nearVec(pole.apply(glm::vec3(1, 0, 0)) - pole.position, glm::vec3(0.164F, 0, 0.986F)));
    // Row 2 is the negated Z axis, and the parser negated the file's Z, so
    // the parsed model's Z maps to the XML's ZAxis.
    CHECK(nearVec(pole.apply(glm::vec3(0, 0, 1)) - pole.position, glm::vec3(-0.986F, 0, 0.164F)));
    CHECK(nearVec(pole.apply(glm::vec3(0, 2, 0)), pole.position + glm::vec3(0, 2, 0)));
}

TEST(readsScatterSets)
{
    // A tree at two levels, whose copies take different blocks, a mesh named
    // by a path of odd length, and a bush at one level whose block ends
    // beyond its group's draw distance.
    TestScatterInstance turned;
    turned.position = glm::vec3(-167.5F, 36.25F, 854.0F);
    turned.axes = {glm::vec3(0, 0, -0.5F), glm::vec3(0, 0.5F, 0), glm::vec3(0.5F, 0, 0)};
    turned.block = 1;
    TestScatterInstance upright;
    upright.position = glm::vec3(10, 20, 30);
    TestScatterMesh tree;
    tree.draws = {61908, 61909};
    TestScatterMesh pathNamed;
    pathNamed.path = "Tracks\\Colorado\\Scene\\Vegetation\\VEG_Tree.max";
    TestScatterMesh bush;
    bush.draws = {61915};
    TestScatterGroup trees;
    trees.instances = {turned, upright};
    trees.blocks = {{30.0F, 40.0F, 60.0F}, {50.0F, 100.0F, 220.0F}};
    TestScatterGroup paths;
    paths.mesh = 1;
    paths.instances = {upright};
    paths.blocks = {{50.0F, 100.0F, 220.0F}};
    TestScatterGroup bushes;
    bushes.mesh = 2;
    bushes.instances = {upright};
    bushes.drawDistance = 100.0F;
    bushes.blocks = {{50.0F, 70.0F, 150.0F}};
    const Bytes data = scatterFile("Models_Ungrouped_1706", {tree, pathNamed, bush}, {trees, paths, bushes});

    const world::ScatterSet set = world::readScatterSet(data);
    CHECK_EQ(set.name, std::string("Models_Ungrouped_1706"));
    CHECK_EQ(set.meshDraws.size(), std::size_t{3});
    CHECK(set.meshDraws[0] == (std::vector<std::uint32_t>{61908, 61909}));
    CHECK(set.meshDraws[1].empty());
    CHECK(set.meshDraws[2] == (std::vector<std::uint32_t>{61915}));
    // The path-named mesh's instance is left out.
    CHECK_EQ(set.instances.size(), std::size_t{3});
    if (set.instances.size() == 3) {
        const world::ScatterSet::Instance& first = set.instances[0];
        CHECK_EQ(first.mesh, 0U);
        CHECK(first.placement.position == turned.position);
        // The stored Z axis, negated, maps the model's stored Z.
        CHECK(first.placement.rows == (std::array<float, 9>{0, 0, -0.5F, 0, 0.5F, 0, -0.5F, 0, 0}));
        // Each copy's own block gives its distances: the first level ends
        // at E0, the last at E2.
        CHECK(first.placement.ranges == (std::array<float, 3>{50.0F, 220.0F, -1.0F}));
        CHECK(set.instances[1].placement.position == upright.position);
        CHECK(set.instances[1].placement.ranges == (std::array<float, 3>{30.0F, 60.0F, -1.0F}));
        // One level, drawn to the group's distance, which comes before E2.
        CHECK_EQ(set.instances[2].mesh, 2U);
        CHECK(set.instances[2].placement.ranges == (std::array<float, 3>{100.0F, -1.0F, -1.0F}));
    }

    Bytes wrongSize = data;
    ++wrongSize[0x4B];
    CHECK_THROWS(world::readScatterSet(wrongSize));
    TestScatterGroup pastBlocks = bushes;
    pastBlocks.instances.front().block = 1;
    CHECK_THROWS(world::readScatterSet(
        scatterFile("Models_Ungrouped_1706", {tree, pathNamed, bush}, {trees, paths, pastBlocks})));
    Bytes empty(0x104, 0);
    std::copy_n("OEGP", 4, empty.begin());
    CHECK_THROWS(world::readScatterSet(empty));
    Bytes truncated(data.begin(), data.end() - 200);
    Writer patch{truncated};
    patch.patch32(0x48, static_cast<std::uint32_t>(truncated.size()));
    CHECK_THROWS(world::readScatterSet(patch.data));
}

TEST(readsEventGatedScatterSets)
{
    TestScatterInstance upright;
    upright.position = glm::vec3(10, 20, 30);
    TestScatterMesh tree;
    tree.draws = {61908, 61909};
    TestScatterGroup trees;
    trees.instances = {upright, upright};
    trees.blocks = {{50.0F, 100.0F, 220.0F}};
    const std::string name = "models_proc_clrd_trees_redstone_area02[freeroam]_0";
    const Bytes plain = scatterFile("Models_Ungrouped_1706", {tree}, {trees});
    CHECK(world::scatterSetKind(plain) == std::optional<std::uint32_t>(world::kModelSetKind));
    CHECK(!world::scatterSetKind(fh::ByteView(plain).first(0x20)).has_value());
    CHECK(!world::readScatterSet(plain).eventProp);

    // The event block and the longer name replace the plain set's 32-byte
    // name field; everything after it moves by a multiple of 16 bytes.
    Writer w;
    w.data.assign(plain.begin(), plain.begin() + 0x60);
    w.u32(0xFFFFFFFF);
    std::string tag = "FR04";
    tag.resize(32, '\0');
    w.text(tag);
    w.u8(2);
    w.u8(0);
    w.u8(21);
    w.padTo(0x60 + 56);
    w.text(name);
    w.u8(0);
    w.alignTo(16);
    w.data.insert(w.data.end(), plain.begin() + 0x80, plain.end());
    w.patch32(0x3C, static_cast<std::uint32_t>(name.size() + 1));
    w.patch32(0x48, static_cast<std::uint32_t>(w.data.size()));
    w.patch32(0x58, 0x0C92BCB0);
    const world::ScatterSet set = world::readScatterSet(w.data);
    CHECK_EQ(set.name, name);
    CHECK(set.eventProp);
    CHECK_EQ(set.eventTag, std::string("FR04"));
    CHECK(set.eventRoutes == (std::vector<std::uint8_t>{0, 21}));
    CHECK(set.meshDraws == (std::vector<std::vector<std::uint32_t>>{{61908, 61909}}));
    CHECK_EQ(set.instances.size(), std::size_t{2});
    for (const world::ScatterSet::Instance& instance : set.instances) {
        CHECK(instance.placement.position == upright.position);
        CHECK(instance.placement.eventProp);
        CHECK_EQ(instance.placement.eventTag, std::string("FR04"));
        CHECK(instance.placement.onRoute(0));
        CHECK(instance.placement.onRoute(21));
        CHECK(!instance.placement.onRoute(3));
    }

    Writer notABlock{w.data};
    notABlock.patch32(0x60, 0x000000A2);
    CHECK_THROWS(world::readScatterSet(notABlock.data));
    Writer headerEntries{w.data};
    headerEntries.patch32(0x44, 1);
    CHECK_THROWS(world::readScatterSet(headerEntries.data));
}

TEST(eventPropsFollowTheLoadedRoute)
{
    world::WorldChunk plain;
    plain.placed = true;
    world::WorldChunk freeRoam = plain;
    freeRoam.placement.eventProp = true;
    freeRoam.placement.eventRoutes = {0, 21};
    world::WorldChunk race = freeRoam;
    race.placement.eventRoutes = {147};
    world::WorldChunk everyRoute = freeRoam;
    everyRoute.placement.eventRoutes.clear();
    world::WorldChunk named = freeRoam;
    named.placement.eventTag = "FR04_01";

    const world::EventPropFilter none;
    CHECK(none.includes(plain));
    CHECK(!none.includes(freeRoam));
    const world::EventPropFilter onFreeRoam = world::EventPropFilter::route(0);
    CHECK(onFreeRoam.includes(plain));
    CHECK(onFreeRoam.includes(freeRoam));
    CHECK(!onFreeRoam.includes(race));
    CHECK(onFreeRoam.includes(everyRoute));
    CHECK(!onFreeRoam.includes(named));
    const world::EventPropFilter onRace = world::EventPropFilter::route(147);
    CHECK(!onRace.includes(freeRoam));
    CHECK(onRace.includes(race));
    CHECK(onRace.includes(everyRoute));
    CHECK(world::EventPropFilter::all().includes(named));
}

TEST(readsShaderLayoutsAndPvsTables)
{
    // Normal, TEXCOORD0, TEXCOORD1 (flagged), as in h_diff_spec_ao_2.
    const std::optional<world::ShaderLayout> diffuse
        = world::readShaderLayout(shaderObject({0x3007, 0x5008, 0x215009}));
    CHECK(diffuse.has_value());
    if (diffuse) {
        CHECK_EQ(diffuse->texcoord0Offset, 16);
        CHECK_EQ(diffuse->vertexBytes, 24);
    }
    const std::optional<world::ShaderLayout> none = world::readShaderLayout(shaderObject({0x3007, 0xA008}));
    CHECK(none.has_value());
    if (none) {
        CHECK_EQ(none->texcoord0Offset, -1);
        CHECK_EQ(none->colourOffset, 16);
        CHECK(none->samplers.empty());
    }
    // Vertex-blended ground: three coordinate pairs and a colour, with its
    // samplers in the pixel shader's constant table.
    const std::optional<world::ShaderLayout> blend = world::readShaderLayout(shaderObject(
        {0x3007, 0x5008, 0x15009, 0x2500A, 0xA00B}, {{0, "Blend_ASampler"}, {1, "Blend_BSampler"}, {5, "AO_Sampler"}}));
    CHECK(blend.has_value());
    if (blend) {
        CHECK_EQ(blend->texcoord0Offset, 16);
        CHECK_EQ(blend->texcoord1Offset, 20);
        CHECK_EQ(blend->texcoord2Offset, 24);
        CHECK_EQ(blend->colourOffset, 28);
        CHECK_EQ(blend->samplers.size(), std::size_t{6});
        CHECK_EQ(blend->samplerRegister("AO_Sampler"), 5);
        CHECK_EQ(blend->samplerRegister("Blend_BSampler"), 1);
        CHECK_EQ(blend->samplerRegister("Splat_Sampler"), -1);
    }
    const std::string_view noShader = "no shader here";
    const fh::ByteView noShaderBytes(reinterpret_cast<const std::uint8_t*>(noShader.data()), noShader.size());
    CHECK(!world::readShaderLayout(noShaderBytes).has_value());

    const world::PvsTables tables = world::readPvs(pvsFile({0x10, 0x20, 0x2B40}, {{2, 0}, {}, {1}}, {4, 2}));
    CHECK(tables.textureIds == (std::vector<std::uint32_t>{0x10, 0x20, 0x2B40}));
    CHECK_EQ(tables.objectTextures.size(), std::size_t{3});
    CHECK(tables.objectTextures[0] == (std::vector<std::uint32_t>{0x2B40, 0x10}));
    CHECK(tables.objectTextures[1].empty());
    CHECK(tables.objectTextures[2] == (std::vector<std::uint32_t>{0x20}));
    CHECK(tables.drawObjects == (std::vector<std::uint16_t>{4, 2}));
    CHECK_THROWS(world::readPvs(pvsFile({0x10}, {{5}})));
    const std::string_view notPvs = "FPVS but nothing else";
    CHECK_THROWS(world::readPvs(fh::ByteView(reinterpret_cast<const std::uint8_t*>(notPvs.data()), notPvs.size())));

    CHECK(world::TrackTextures::objectNumber("coloradoout.06454.rmb.bin") == std::optional<std::uint32_t>(6454));
    CHECK(world::TrackTextures::objectNumber("COLORADO.7.RMB.BIN") == std::optional<std::uint32_t>(7));
    CHECK(!world::TrackTextures::objectNumber("_0x00002B40.bix").has_value());
    CHECK(!world::TrackTextures::objectNumber("colorado.rmb.bin").has_value());
    CHECK(!world::TrackTextures::objectNumber("colorado.99999999999.rmb.bin").has_value());
    CHECK(world::numberBefore("__R00Z01434.pvsz", 'z', ".pvsz") == std::optional<std::uint32_t>(1434));
    CHECK_EQ(world::TrackTextures::textureName(0x2B40), std::string("_0x00002B40"));

    Writer pack;
    pack.u32(1);
    pack.u32(7);
    pack.u32(4);
    pack.u32(4);
    pack.u32(3);
    pack.u32(0x52);
    pack.u32(0xFFFFFFFF);
    pack.u32(8);
    pack.fill(8, 0xAB);
    const std::vector<world::TrackTextures::BundleEntry> entries = world::TrackTextures::readBundle(pack.data);
    CHECK_EQ(entries.size(), std::size_t{1});
    if (!entries.empty()) {
        CHECK_EQ(entries[0].record, 7U);
        CHECK_EQ(entries[0].offset, std::size_t{32});
        CHECK_EQ(entries[0].size, std::size_t{8});
        CHECK_EQ(entries[0].formatWord, 0x52U);
    }
    Bytes badSeparator = pack.data;
    badSeparator[4 + 20] = 0;
    CHECK_THROWS(world::TrackTextures::readBundle(badSeparator));
    CHECK_THROWS(world::TrackTextures::readBundle(Bytes{0, 0, 0, 1}));
}

TEST(decodesAndEncodesTextures)
{
    // A 128x128 DXT1 texture of solid red, tiled as the GPU stores it with
    // 16-bit words swapped; every block is the same, so the tiling does not
    // change the data.
    constexpr std::uint32_t kDxt1Swapped = 18 | (1 << 6);
    Bytes tiled;
    for (int block = 0; block < 32 * 32; ++block) {
        for (const std::uint8_t b : {0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00}) {
            tiled.push_back(b);
        }
    }
    const world::TextureSurface surface = world::untileXboxSurface(tiled, kDxt1Swapped, 128, 128);
    CHECK(surface.format == world::TextureSurface::Format::Dxt1);
    CHECK_EQ(surface.data.size(), std::size_t{32 * 32 * 8});
    const world::RgbaImage image = world::decodeSurface(surface);
    CHECK_EQ(image.width, 128);
    CHECK_EQ(image.rgba.size(), std::size_t{128 * 128 * 4});
    CHECK_EQ(image.rgba[0], 255);
    CHECK_EQ(image.rgba[1], 0);
    CHECK_EQ(image.rgba[3], 255);
    CHECK_THROWS(world::untileXboxSurface(fh::ByteView(tiled).first(100), kDxt1Swapped, 128, 128));
    CHECK_THROWS(world::untileXboxSurface(tiled, 17, 128, 128));

    const world::TextureMipChain chain = world::buildMipChain(surface);
    CHECK(chain.format == world::TextureSurface::Format::Dxt1);
    CHECK_EQ(chain.levels.size(), std::size_t{8});
    CHECK(chain.levels[0] == surface.data);
    CHECK_EQ(chain.levels.back().size(), std::size_t{8});
    const world::RgbaImage smallest = world::decodeSurface(chain.level(7));
    CHECK_EQ(smallest.rgba[0], 255);
    CHECK_EQ(smallest.rgba[1], 0);

    // RGBA levels are filtered, weighting colour by alpha.
    world::TextureSurface rgba;
    rgba.width = 2;
    rgba.height = 1;
    rgba.data = {200, 100, 50, 255, 0, 0, 0, 0};
    const world::TextureMipChain rgbaChain = world::buildMipChain(rgba);
    CHECK_EQ(rgbaChain.levels.size(), std::size_t{2});
    CHECK(rgbaChain.levels[1] == (Bytes{200, 100, 50, 127}));

    // BC5 keeps two channels, red first.
    std::array<world::dxt::Rgba, 16> texels{};
    for (std::size_t i = 0; i < 16; ++i) {
        texels[i] = {static_cast<std::uint8_t>(i * 16), static_cast<std::uint8_t>(255 - i * 16), 0, 255};
    }
    world::TextureSurface normals;
    normals.format = world::TextureSurface::Format::Bc5;
    normals.width = 4;
    normals.height = 4;
    normals.data.resize(16);
    world::dxt::encodeBc5(texels.data(), normals.data.data());
    const world::RgbaImage decoded = world::decodeSurface(normals);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK_NEAR(decoded.rgba[i * 4], texels[i].r, 20);
        CHECK_NEAR(decoded.rgba[i * 4 + 1], texels[i].g, 20);
    }
    const world::TextureMipChain normalChain = world::buildMipChain(normals);
    CHECK(normalChain.format == world::TextureSurface::Format::Bc5);
    CHECK_EQ(normalChain.levels.size(), std::size_t{3});
    CHECK_EQ(normalChain.levels[2].size(), std::size_t{16});

    // DXT3 is converted to DXT5.
    world::TextureSurface dxt3;
    dxt3.format = world::TextureSurface::Format::Dxt3;
    dxt3.width = 4;
    dxt3.height = 4;
    dxt3.data = Bytes(16, 0xFF);
    CHECK(world::buildMipChain(dxt3).format == world::TextureSurface::Format::Dxt5);
}

TEST(convertsTrackVertexDeclarations)
{
    namespace dt = world::decltype360;
    fh::xenos::VertexDeclaration file;
    const auto element = [](std::uint16_t offset, std::uint32_t type, fh::xenos::DeclUsage usage, std::uint8_t index) {
        fh::xenos::VertexDeclElement e;
        e.offset = offset;
        e.type = type;
        e.usage = static_cast<std::uint8_t>(usage);
        e.usageIndex = index;
        return e;
    };
    file.elements = {element(0, dt::kFloat3, fh::xenos::DeclUsage::Position, 0),
        element(12, dt::kDec3N, fh::xenos::DeclUsage::Normal, 0),
        element(16, dt::kUShort2N, fh::xenos::DeclUsage::Texcoord, 0),
        element(20, dt::kColor, fh::xenos::DeclUsage::Color, 0)};
    const std::optional<fh::xenos::VertexDeclaration> render = world::TrackShaders::renderDeclaration(file);
    CHECK(render.has_value());
    if (render) {
        CHECK_EQ(render->elements.size(), std::size_t{4});
        CHECK_EQ(render->elements[1].offset, std::uint16_t{12});
        CHECK_EQ(render->elements[1].type, dt::kFloat3Native);
        CHECK_EQ(render->elements[2].offset, std::uint16_t{24});
        CHECK_EQ(render->elements[2].type, dt::kFloat2Native);
        CHECK_EQ(render->elements[3].offset, std::uint16_t{32});
        CHECK_EQ(render->elements[3].type, dt::kColor);
        CHECK_EQ(render->stride(0), 36U);
        // Same formats, no byte swap.
        CHECK(render->elements[0].format() == file.elements[0].format());
        CHECK_EQ(render->elements[0].endian(), 0U);
        CHECK(render->elements[2].format() == fh::xenos::VertexFormat::F32_32_Float);
    }
    fh::xenos::VertexDeclaration unknown;
    unknown.elements = {element(0, 0x2A2360, fh::xenos::DeclUsage::Position, 0)};
    CHECK(!world::TrackShaders::renderDeclaration(unknown).has_value());
}

TEST(buildsAWorldFromAnArchive)
{
    // Two levels of a world-space road, and a sign modelled at the origin
    // that two draw records place, one of them mirrored.
    TempDir dir;
    const std::filesystem::path archivePath = dir.path() / "bin.zip";
    TestPart sign = quad("Sign_LOD00", 0, 0, 0, 1);
    sign.stride = 24;
    sign.texcoords = {{0, 0}, {65535, 0}, {0, 65535}, {65535, 65535}};
    TestMaterialInfo signInfo;
    signInfo.textureSlots = {0};
    TestTail signTail;
    signTail.materials = {signInfo};
    signTail.shaders = {"shaders\\track\\h_diff_1.fx"};
    TestPlacement upright;
    upright.draw = 0;
    upright.position = glm::vec3(1005, 2, 1003);
    upright.rows = {1, 0, 0, 0, 1, 0, 0, 0, -1};
    upright.ranges = {50, -1, -1};
    TestPlacement mirrored;
    mirrored.draw = 1;
    mirrored.position = glm::vec3(990, 0, 990);
    mirrored.ranges = {50, -1, -1};
    const Bytes pvs = pvsFile({0x2B40}, {{}, {}, {0}}, {2, 2});
    {
        lzx::ZipWriter writer(archivePath);
        writer.add("tracks.00000.rmb.bin", renderModel({quad("Road_LOD01_02", 1000, 1000, 0, 10)}));
        writer.add("tracks.00001.rmb.bin", renderModel({quad("Road_LOD02_01", 1000, 1000, 0, 10)}));
        writer.add("tracks.00002.rmb.bin", renderModel({sign}, signTail));
        writer.add("TRACKS.00003.RMB.BIN", renderModel({quad("Unplaced_LOD00", 0, 0, 0, 1)}));
        writer.add("tracks.00004.rmb.bin", renderModel({quad("Road_ShadowCaster_LOD01", 1000, 1000, 0, 10)}));
        writer.add("shaders/track/h_diff_1.fxobj", shaderObject({0x3000, 0x5001}, {{0, "Diffuse_TextureSampler"}}));
        writer.add("__R00Z00000.pvsz", zoneFile({upright, mirrored}));
        writer.finish();
    }
    const lzx::ZipArchive archive = lzx::ZipArchive::open(archivePath);

    const std::optional<world::TrackPlacements> placements = world::TrackPlacements::load(pvs, archive);
    CHECK(placements.has_value());
    if (!placements) {
        return;
    }
    CHECK_EQ(placements->drawCount(), std::size_t{2});
    CHECK_EQ(placements->zoneCount(), 1);
    CHECK_EQ(placements->failedZones(), 0);
    CHECK(placements->placement(0) != nullptr);
    CHECK(placements->zonesListing(1) == (std::vector<std::uint16_t>{0}));
    const world::TrackTextures textures = world::TrackTextures::load(pvs, archive);
    CHECK_EQ(textures.shaderCount(), std::size_t{1});
    CHECK(textures.shader("shaders\\track\\H_DIFF_1.fx") != nullptr);

    std::size_t lastProgress = 0;
    const std::optional<world::WorldIndex> index = world::WorldIndex::build(
        archive, [&lastProgress](std::size_t done, std::size_t) { lastProgress = done; }, nullptr, &*placements);
    CHECK(index.has_value());
    if (!index) {
        return;
    }
    CHECK_EQ(lastProgress, std::size_t{5});
    CHECK_EQ(index->chunks().size(), std::size_t{4});
    CHECK_EQ(index->placedCount(), std::size_t{2});
    CHECK_EQ(index->localModelCount(), 1);
    for (const world::WorldChunk& chunk : index->chunks()) {
        if (!chunk.placed && chunk.lod == 1) {
            CHECK_EQ(chunk.bandStart, 0.0F);
            CHECK_EQ(chunk.bandEnd, world::WorldIndex::kLodStart[2]);
        } else if (!chunk.placed) {
            CHECK_EQ(chunk.lod, 2);
            CHECK_EQ(chunk.bandStart, world::WorldIndex::kLodStart[2]);
            CHECK(std::isinf(chunk.bandEnd));
        } else {
            CHECK_EQ(chunk.bandStart, 0.0F);
            CHECK_EQ(chunk.bandEnd, 50.0F);
        }
    }
    const world::Footprint footprint = index->footprint();
    CHECK(footprint.min.x <= 989.0F && footprint.max.x >= 1010.0F);
    // With the zone grid, the props take the zones whose files list their
    // draws, and the world-space models, which no draw record places here,
    // are drawn from everywhere.
    const world::ZoneGrid zoneGrid = world::ZoneGrid::read(hexFile(1, 1, 1, {0}));
    const std::optional<world::WorldIndex> zoned
        = world::WorldIndex::build(archive, {}, nullptr, &*placements, &zoneGrid);
    CHECK(zoned.has_value());
    if (zoned) {
        for (const world::WorldChunk& chunk : zoned->chunks()) {
            CHECK(chunk.zones == (chunk.placed ? std::vector<std::uint16_t>{0} : std::vector<std::uint16_t>{}));
        }
    }

    const world::WorldTileGrid grid(*index, 500.0F);
    const auto [cellX, cellZ] = grid.cellAt(1000, 1000);
    const std::optional<std::size_t> tile = grid.tileAt(cellX, cellZ);
    CHECK(tile.has_value());
    CHECK(!grid.tileAt(cellX + 5, cellZ).has_value());
    if (!tile) {
        return;
    }
    const std::vector<std::uint32_t> near = grid.chunksAt(grid.tiles()[*tile], 0.0F);
    CHECK_EQ(near.size(), std::size_t{3});
    CHECK_EQ(grid.chunksAt(grid.tiles()[*tile], 2000.0F).size(), std::size_t{1});
    // Near the tile every chunk may be drawn; past 50 m the prop's band has
    // ended, and past the first level's end so has that level's.
    CHECK_EQ(grid.chunksBeyond(grid.tiles()[*tile], 0.0F).size(), grid.tiles()[*tile].chunks.size());
    CHECK_EQ(grid.chunksBeyond(grid.tiles()[*tile], 60.0F).size(), std::size_t{2});
    CHECK_EQ(grid.chunksBeyond(grid.tiles()[*tile], 2000.0F).size(), std::size_t{1});

    const world::TileMesh mesh = world::buildTileMesh(archive, *index, near, &textures);
    CHECK_EQ(mesh.failedChunks, 0);
    CHECK_EQ(mesh.triangleCount(), std::size_t{6});
    CHECK_EQ(mesh.vertexCount(), std::size_t{12});
    CHECK(mesh.textures() == (std::vector<std::uint32_t>{0x2B40}));
    CHECK_EQ(mesh.batches.size(), std::size_t{2});
    CHECK(runsCoverBatches(mesh.batches));
    // Every triangle, the mirrored sign's included, winds clockwise seen
    // from the side its normals point to.
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const auto position = [&mesh](std::uint32_t v) {
            const float* f = mesh.vertices.data() + static_cast<std::size_t>(v) * world::TileMesh::kFloatsPerVertex;
            return glm::vec3(f[0], f[1], f[2]);
        };
        const auto normal = [&mesh](std::uint32_t v) {
            const float* f = mesh.vertices.data() + static_cast<std::size_t>(v) * world::TileMesh::kFloatsPerVertex
                + world::TileMesh::kNormal;
            return glm::vec3(f[0], f[1], f[2]);
        };
        const glm::vec3 a = position(mesh.indices[t]);
        const glm::vec3 b = position(mesh.indices[t + 1]);
        const glm::vec3 c = position(mesh.indices[t + 2]);
        CHECK(glm::dot(glm::cross(c - a, b - a), normal(mesh.indices[t])) > 0.0F);
    }
    // The sign's corners map through the material to the texture's corners.
    bool foundCorner = false;
    for (std::size_t v = 0; v < mesh.vertexCount(); ++v) {
        const float* f = mesh.vertices.data() + v * world::TileMesh::kFloatsPerVertex;
        if (nearVec(glm::vec3(f[0], f[1], f[2]), glm::vec3(1006, 2, 1004))) {
            foundCorner = true;
            CHECK_NEAR(f[world::TileMesh::kTexcoord], 1.0, 1e-4);
            CHECK_NEAR(f[world::TileMesh::kTexcoord + 1], 1.0, 1e-4);
        }
    }
    CHECK(foundCorner);

    const world::TileMesh broken = world::buildTileMesh(archive, *index, {99}, nullptr);
    CHECK_EQ(broken.failedChunks, 1);
    CHECK_EQ(broken.errors.size(), std::size_t{1});

    const Bytes prefix = world::readEntryPrefix(archive, *archive.find("tracks.00000.rmb.bin"), 16);
    CHECK_EQ(prefix.size(), std::size_t{16});
    CHECK_EQ(prefix[3], 6);
}

TEST(loadsColorado)
{
    const char* gameDir = std::getenv("FH1_GAME_DIR");
    if (gameDir == nullptr || *gameDir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(gameDir);
    const std::optional<world::TrackWorld> loaded = world::TrackWorld::load(vfs, "colorado");
    CHECK(loaded.has_value());
    if (!loaded) {
        return;
    }
    const world::TrackWorld& colorado = *loaded;
    CHECK(colorado.warnings.empty());
    CHECK(colorado.zones.has_value());
    CHECK(colorado.placements.has_value());
    CHECK(colorado.textures.has_value());
    if (!colorado.zones || !colorado.placements || !colorado.textures) {
        return;
    }
    CHECK_EQ(colorado.zones->zoneCount(), 1434);
    CHECK_EQ(colorado.placements->zoneCount(), 1434);
    CHECK_EQ(colorado.placements->failedZones(), 0);
    CHECK_EQ(colorado.placements->drawCount(), std::size_t{62173});
    // Every model set: 1562 ungated and 469 behind an event block.
    CHECK_EQ(colorado.placements->scatterSets().size(), std::size_t{2031});
    CHECK_EQ(colorado.placements->failedScatterSets(), 0);
    CHECK_EQ(colorado.placements->scatterInstanceCount(), std::size_t{174802});
    // The map manager's counts: about 48,000 zone-placed props and 128,600
    // procedural copies outside event blocks.
    std::size_t ungatedCopies = 0;
    // The free-roam forests: 55 tree sets and a fence set for route 0.
    std::size_t freeRoamSets = 0;
    std::size_t freeRoamCopies = 0;
    for (const world::ScatterSet& set : colorado.placements->scatterSets()) {
        ungatedCopies += set.eventProp ? 0 : set.instances.size();
        const bool forest = set.name.starts_with("models_proc_clrd_trees_mountains_")
            || set.name.starts_with("models_proc_clrd_trees_redstone_")
            || set.name.starts_with("models_proc_clrd_trees_maintown_")
            || set.name.starts_with("models_proc_clrd_redrock_");
        if (forest && set.eventProp && set.eventTag.empty()
            && std::find(set.eventRoutes.begin(), set.eventRoutes.end(), 0) != set.eventRoutes.end()) {
            ++freeRoamSets;
            freeRoamCopies += set.instances.size();
        }
    }
    CHECK_NEAR(static_cast<double>(ungatedCopies), 128600.0, 500.0);
    CHECK_EQ(freeRoamSets, std::size_t{56});
    CHECK_EQ(freeRoamCopies, std::size_t{385});
    const world::WorldIndex& index = colorado.index;
    CHECK_NEAR(static_cast<double>(index.placedCount() - index.scatteredCount()), 48000.0, 1000.0);
    // The 100 discount signs, one placed chunk each.
    std::size_t flyers = 0;
    for (const world::WorldChunk& chunk : index.chunks()) {
        flyers += chunk.placed && chunk.placement.gameplayId.starts_with("flyer_") ? 1 : 0;
    }
    CHECK_EQ(flyers, std::size_t{100});
    std::size_t backdrop = 0;
    for (const world::WorldChunk& chunk : index.chunks()) {
        if (chunk.backdrop) {
            ++backdrop;
            CHECK(!chunk.zones.empty());
        }
    }
    CHECK(backdrop > 0);

    // Every effect the models name parses, with its Default pass's states.
    CHECK_EQ(colorado.shaders.size(), std::size_t{173});
    CHECK_EQ(colorado.shaders.failed(), 0);
    const auto stateOf = [&colorado](std::string_view name) {
        const std::optional<std::uint32_t> effect
            = colorado.shaders.find(std::string("shaders\\track\\") + std::string(name) + ".fx");
        CHECK(effect.has_value());
        return effect ? colorado.shaders.at(*effect).state : world::TrackShaders::RenderState{};
    };
    CHECK_EQ(stateOf("h_diff_1").cull, 2U);
    CHECK(!stateOf("h_diff_1").blend);
    CHECK_EQ(stateOf("tree_diff_opac_dirlight_2_2sd_noanim").cull, 0U);
    CHECK(stateOf("tree_diff_opac_dirlight_2_2sd_noanim").alphaToMask);
    CHECK_EQ(stateOf("tree_diff_opac_dirlight_2_2sd_noanim").alphaToMaskOffsets, 0xAAU);
    const world::TrackShaders::RenderState lake = stateOf("lake_anim_norm_opac_refl_3");
    CHECK(lake.blend);
    CHECK_EQ(lake.sourceBlend, 6U);
    CHECK_EQ(lake.destinationBlend, 7U);
    CHECK(stateOf("h_vblnd_decal_lm").slopeScaleDepthBias == 1.0F);
    CHECK(!stateOf("h_vblnd_decal_lm").depthWrite);

    // The marker pole the README names stands in a zone, and its tiles
    // build without failures.
    const world::WorldTileGrid grid(index, 500.0F);
    const auto [cellX, cellZ] = grid.cellAt(-1670.0F, -1208.0F);
    const std::optional<std::size_t> tile = grid.tileAt(cellX, cellZ);
    CHECK(tile.has_value());
    CHECK(colorado.zones->zoneAt(-1670.0F, -1208.0F) >= 0);
    if (tile) {
        const std::vector<std::uint32_t> chunks = grid.chunksAt(grid.tiles()[*tile], 0.0F);
        const world::TileMesh mesh = world::buildTileMesh(*colorado.archive, index, chunks, &*colorado.textures);
        CHECK_EQ(mesh.failedChunks, 0);
        CHECK(mesh.triangleCount() > 10000);
        // The effects' declarations fit every material's vertices, so the
        // shaded build keeps every triangle.
        const world::ShadedMesh shaded
            = world::buildShadedMesh(*colorado.archive, index, chunks, colorado.shaders, &*colorado.textures);
        CHECK_EQ(shaded.failedChunks, 0);
        CHECK_EQ(shaded.skippedMaterials, 0);
        CHECK_EQ(shaded.triangleCount(), mesh.triangleCount());
        CHECK(runsCoverBatches(mesh.batches));
        CHECK(runsCoverBatches(shaded.batches));
        for (const world::ShadedMesh::Batch& batch : shaded.batches) {
            CHECK_EQ(batch.vertexOffset % world::ShadedMesh::kVertexAlignment, std::size_t{0});
            CHECK_EQ(batch.stride, colorado.shaders.at(batch.shader).renderDeclaration.stride(0));
            CHECK(batch.vertexOffset + std::size_t{batch.vertexCount} * batch.stride <= shaded.vertices.size());
        }
        const std::vector<std::uint32_t> ids = mesh.textures();
        CHECK(!ids.empty());
        for (std::size_t i = 0; i < std::min<std::size_t>(ids.size(), 8); ++i) {
            const world::TextureMipChain chain = colorado.textures->loadTexture(*colorado.archive, ids[i]);
            CHECK(!chain.levels.empty());
            CHECK_EQ(chain.levels.size(),
                static_cast<std::size_t>(std::bit_width(static_cast<unsigned>(std::max(chain.width, chain.height)))));
        }
    }
}

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
