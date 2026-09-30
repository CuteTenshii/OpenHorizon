#include "CarBin.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace openhorizon::carmodel {

namespace {

constexpr std::uint32_t kCarPreambleVersion = 5;
constexpr std::uint32_t kWheelPreambleVersion = 3;
constexpr std::uint32_t kBrakePreambleVersion = 1;
constexpr std::uint32_t kHeaderVersion = 17;
constexpr std::uint32_t kModelVersion = 4;
constexpr std::uint32_t kDrawListVersion = 4;
constexpr std::uint32_t kArrayVersion = 1;
constexpr std::uint32_t kNodeVersion = 2;
constexpr std::uint32_t kSteeringVersion = 3;
constexpr std::uint32_t kPartVersion = 5;
constexpr std::uint32_t kMeshGroupVersion = 1;
constexpr std::uint32_t kFactorsVersion = 3;
constexpr std::uint32_t kMeshVersion = 2;
constexpr std::uint32_t kTransformVersion = 1;
constexpr std::uint32_t kVertexBufferVersion = 3;
constexpr std::uint32_t kIndexBufferVersion = 4;
constexpr std::uint32_t kMorphVersion = 1;
constexpr std::uint32_t kGlassVersion = 1;

constexpr std::uint32_t kHasHash = 1;
constexpr std::uint32_t kDataLeftOut = 2;
constexpr std::uint32_t kTriangleList = 4;

constexpr std::size_t kCarPreambleBlock = 256;
constexpr std::size_t kCarPreambleMiddle = 48;
constexpr std::size_t kVec4Bytes = 16;
constexpr std::size_t kLightTailBytes = 14;
constexpr std::size_t kHeaderRecords = 15;
constexpr std::size_t kHeaderRecordBytes = 36;
constexpr std::size_t kTransformFloats = 12;
constexpr std::size_t kGlassEntryBytes = 16;
constexpr std::size_t kGlassIndexBytes = 2;
constexpr std::size_t kTailEntryBytes = 16;

constexpr std::uint32_t kMaxCount = 1U << 24;
constexpr std::uint32_t kMaxStride = 256;
constexpr std::uint32_t kMaxNameLength = 1024;

constexpr float kS16 = 1.0F / 32767.0F;
constexpr float kU16 = 1.0F / 65535.0F;

std::uint32_t readCount(BeReader& r, const char* what)
{
    const std::uint32_t count = r.u32();
    if (count > kMaxCount) {
        r.seek(r.pos() - 4);
        r.fail(std::format("implausible {} count {}", what, count));
    }
    return count;
}

std::string readString(BeReader& r)
{
    const std::uint32_t length = r.u32();
    if (length > kMaxNameLength) {
        r.seek(r.pos() - 4);
        r.fail(std::format("implausible string length {}", length));
    }
    return r.fixedString(length);
}

glm::vec3 readVec3(BeReader& r)
{
    const float x = r.f32();
    const float y = r.f32();
    const float z = r.f32();
    return {x, y, z};
}

glm::mat4 readMatrix(BeReader& r)
{
    glm::mat4 m(1.0F);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            m[column][row] = r.f32();
        }
    }
    return m;
}

void readPreamble(BeReader& r, CarBin& model)
{
    switch (model.kind) {
    case CarBin::Kind::Car: {
        r.expect(kCarPreambleVersion, "car preamble version");
        r.skip(kCarPreambleBlock + kVec4Bytes + kCarPreambleMiddle + kVec4Bytes);
        const std::uint32_t lights = readCount(r, "light");
        model.lights.reserve(lights);
        for (std::uint32_t i = 0; i < lights; ++i) {
            CarBin::Light light;
            light.position = readVec3(r);
            const float dx = static_cast<float>(r.s16()) * kS16;
            const float dy = static_cast<float>(r.s16()) * kS16;
            const float dz = static_cast<float>(r.s16()) * kS16;
            light.direction = glm::vec3(dx, dy, dz);
            const ByteView tail = r.bytes(kLightTailBytes);
            std::copy(tail.begin(), tail.end(), light.data.begin());
            model.lights.push_back(light);
        }
        break;
    }
    case CarBin::Kind::Wheel:
        r.expect(kWheelPreambleVersion, "wheel preamble version");
        r.s32();
        for (float& f : model.wheelInfo) {
            f = r.f32();
        }
        break;
    case CarBin::Kind::Brake:
        r.expect(kBrakePreambleVersion, "brake preamble version");
        break;
    }
}

void readHeader(BeReader& r, CarBin& model)
{
    r.expect(kHeaderVersion, "header version");
    r.u32();
    r.expect(kArrayVersion, "node array version");
    const std::uint32_t nodes = readCount(r, "node");
    model.nodes.reserve(nodes);
    for (std::uint32_t i = 0; i < nodes; ++i) {
        r.expect(kNodeVersion, "node version");
        CarBin::Node node;
        node.name = readString(r);
        node.index = r.s32();
        node.first = readMatrix(r);
        node.second = readMatrix(r);
        model.nodes.push_back(std::move(node));
    }
    r.expect(kSteeringVersion, "steering block version");
    for (float& f : model.steeringInfo) {
        f = r.f32();
    }
    r.s32();
    r.s32();
    r.skip(kHeaderRecords * kHeaderRecordBytes);
    const float minimum = r.f32();
    const float range = r.f32();
    model.lightingRange = glm::vec2(minimum, range);
    r.s32();
    r.expect(kArrayVersion, "header string array version");
    const std::uint32_t strings = readCount(r, "header string");
    for (std::uint32_t i = 0; i < strings; ++i) {
        readString(r);
    }
}

std::vector<CarBin::DrawItem> readDrawList(BeReader& r)
{
    const std::uint32_t count = readCount(r, "draw list");
    std::vector<CarBin::DrawItem> list(count);
    for (CarBin::DrawItem& item : list) {
        item.part = r.u16();
        item.mesh = r.u16();
    }
    return list;
}

void readDrawLists(BeReader& r, CarBin& model)
{
    r.expect(kDrawListVersion, "draw list version");
    model.lodCount = readCount(r, "LOD");
    model.translucentDrawLists.reserve(model.lodCount);
    for (std::uint32_t i = 0; i < model.lodCount; ++i) {
        model.translucentDrawLists.push_back(readDrawList(r));
    }
    model.opaqueDrawLists.reserve(model.lodCount);
    for (std::uint32_t i = 0; i < model.lodCount; ++i) {
        model.opaqueDrawLists.push_back(readDrawList(r));
    }
}

CarBin::VertexBuffer readVertexBuffer(BeReader& r)
{
    r.expect(kVertexBufferVersion, "vertex buffer version");
    CarBin::VertexBuffer buffer;
    buffer.count = readCount(r, "vertex");
    buffer.stride = r.u32();
    if (buffer.stride > kMaxStride) {
        r.seek(r.pos() - 4);
        r.fail(std::format("implausible vertex stride {}", buffer.stride));
    }
    const std::uint32_t flags = r.u32();
    if ((flags & kHasHash) != 0) {
        r.u32();
    }
    buffer.stripped = (flags & kDataLeftOut) != 0;
    if (buffer.count > 0 && !buffer.stripped) {
        const ByteView data = r.bytes(static_cast<std::size_t>(buffer.count) * buffer.stride);
        buffer.data.assign(data.begin(), data.end());
    }
    return buffer;
}

/// The index buffer as stored, widened to 32 bits; empty when left out.
std::vector<std::uint32_t> readIndexBuffer(BeReader& r, CarBin::Mesh& mesh)
{
    r.expect(kIndexBufferVersion, "index buffer version");
    const std::uint32_t flags = r.u32();
    if ((flags & kHasHash) != 0) {
        r.u32();
    }
    mesh.indexCount = readCount(r, "index");
    const std::uint32_t width = r.u32();
    if (width != 2 && width != 4) {
        r.seek(r.pos() - 4);
        r.fail(std::format("unsupported index width {}", width));
    }
    mesh.indicesStripped = (flags & kDataLeftOut) != 0;
    std::vector<std::uint32_t> indices;
    if (mesh.indicesStripped) {
        return indices;
    }
    const ByteView raw = r.bytes(static_cast<std::size_t>(mesh.indexCount) * width);
    indices.resize(mesh.indexCount);
    for (std::uint32_t i = 0; i < mesh.indexCount; ++i) {
        const std::uint8_t* at = raw.data() + static_cast<std::size_t>(i) * width;
        indices[i] = width == 2 ? loadBe16(at) : loadBe32(at);
    }
    return indices;
}

CarBin::Mesh readMesh(BeReader& r, std::vector<std::uint32_t>& indices)
{
    r.expect(kFactorsVersion, "texture factor version");
    CarBin::Mesh mesh;
    mesh.factorFlag = r.u8() != 0;
    std::array<float, 16> factors{};
    for (float& f : factors) {
        f = r.f32();
    }
    mesh.uv0 = glm::vec4(factors[0], factors[1], factors[2], factors[3]);
    mesh.uv1 = glm::vec4(factors[4], factors[5], factors[6], factors[7]);
    std::copy(factors.begin() + 8, factors.end(), mesh.extraFactors.begin());

    r.expect(kMeshVersion, "mesh version");
    mesh.name = readString(r);
    mesh.lod = r.u32();
    mesh.primitive = r.u32();
    r.u32();
    r.expect(kTransformVersion, "mesh transform version");
    r.skip(kTransformFloats * 4);
    indices = readIndexBuffer(r, mesh);
    r.u32();
    return mesh;
}

CarBin::Part readPart(BeReader& r)
{
    r.expect(kPartVersion, "part version");
    CarBin::Part part;
    part.position = readVec3(r);
    r.skip(12);
    part.halfExtent = readVec3(r);
    part.boxScale = r.f32();
    part.centroid = readVec3(r);
    part.unknownValue = r.f32();

    r.expect(kGlassVersion, "glass section version");
    r.expect(kArrayVersion, "glass entry array version");
    part.glassEntries = readCount(r, "glass entry");
    r.skip(static_cast<std::size_t>(part.glassEntries) * kGlassEntryBytes);
    r.expect(kArrayVersion, "glass index array version");
    part.glassIndices = readCount(r, "glass index");
    r.skip(static_cast<std::size_t>(part.glassIndices) * kGlassIndexBytes);
    part.flag = r.u8() != 0;
    r.s32();

    r.expect(kMeshGroupVersion, "mesh group version");
    part.name = readString(r);
    part.vertices = readVertexBuffer(r);
    r.expect(kArrayVersion, "mesh array version");
    const std::uint32_t meshCount = readCount(r, "mesh");
    part.meshes.reserve(meshCount);
    std::vector<std::vector<std::uint32_t>> indices(meshCount);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        part.meshes.push_back(readMesh(r, indices[m]));
    }
    part.lod0Vertices = readVertexBuffer(r);
    r.expect(kMorphVersion, "morph section version");
    part.hasMorphs = r.u8() != 0;
    part.morph = readVertexBuffer(r);
    part.morphSecondary = readVertexBuffer(r);

    // The vertex buffers come after the meshes, so indices are checked and
    // unrolled only now.
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        CarBin::Mesh& mesh = part.meshes[m];
        const CarBin::VertexBuffer& buffer = part.verticesFor(mesh);
        carbin::appendTriangles(indices[m], mesh.primitive, buffer.count, mesh.triangles);
    }
    return part;
}

void readTail(BeReader& r)
{
    r.expect(kArrayVersion, "model tail array version");
    const std::uint32_t objects = readCount(r, "model tail object");
    for (std::uint32_t i = 0; i < objects; ++i) {
        r.expect(kArrayVersion, "model tail object version");
        r.skip(kTailEntryBytes);
    }
    for (int list = 0; list < 2; ++list) {
        r.expect(kArrayVersion, "model tail string array version");
        const std::uint32_t strings = readCount(r, "model tail string");
        for (std::uint32_t i = 0; i < strings; ++i) {
            readString(r);
        }
    }
}

void markTranslucent(CarBin& model, BeReader& r)
{
    for (const std::vector<CarBin::DrawItem>& list : model.translucentDrawLists) {
        for (const CarBin::DrawItem& item : list) {
            if (item.part >= model.parts.size() || item.mesh >= model.parts[item.part].meshes.size()) {
                r.fail(std::format("draw list names mesh {} of part {}, which does not exist", item.mesh, item.part));
            }
            model.parts[item.part].meshes[item.mesh].translucent = true;
        }
    }
}

/// A quantised coordinate pair through (offset, range) factors. Stand-in
/// meshes of a few traffic vehicles store the unfilled minimum and range
/// (FLT_MAX, -infinity); their coordinates come out as 0.
glm::vec2 dequantise(const glm::vec4& factors, const glm::vec2& raw)
{
    const float u = factors.x + factors.y * raw.x;
    const float v = factors.z + factors.w * raw.y;
    return {std::isfinite(u) ? u : 0.0F, std::isfinite(v) ? v : 0.0F};
}

} // namespace

const CarBin::VertexBuffer& CarBin::Part::verticesFor(const Mesh& mesh) const
{
    const VertexBuffer& preferred = mesh.lod == 0 ? lod0Vertices : vertices;
    const VertexBuffer& other = mesh.lod == 0 ? vertices : lod0Vertices;
    return preferred.empty() && !other.empty() ? other : preferred;
}

std::size_t CarBin::triangleCount() const
{
    std::size_t count = 0;
    for (const Part& part : parts) {
        for (const Mesh& mesh : part.meshes) {
            count += mesh.triangles.size() / 3;
        }
    }
    return count;
}

std::size_t CarBin::vertexCount() const
{
    std::size_t count = 0;
    for (const Part& part : parts) {
        count += part.vertices.count + part.lod0Vertices.count;
    }
    return count;
}

namespace carbin {

CarBin parse(ByteView data, const std::string& source, CarBin::Kind kind)
{
    BeReader r(data, source);
    CarBin model;
    model.kind = kind;
    readPreamble(r, model);
    readHeader(r, model);
    r.expect(kModelVersion, "model version");
    readDrawLists(r, model);
    r.expect(kArrayVersion, "part array version");
    const std::uint32_t partCount = readCount(r, "part");
    model.parts.reserve(partCount);
    for (std::uint32_t p = 0; p < partCount; ++p) {
        model.parts.push_back(readPart(r));
    }
    readTail(r);
    if (!r.atEnd()) {
        r.fail(std::format("{} bytes after the model", r.remaining()));
    }
    markTranslucent(model, r);
    return model;
}

CarBin::Kind kindFromPath(std::string_view path)
{
    const std::string lower = toLower(path);
    if (lower.find("wheels/") != std::string::npos || lower.find("wheels\\") != std::string::npos) {
        return CarBin::Kind::Wheel;
    }
    if (lower.find("brakes/") != std::string::npos || lower.find("brakes\\") != std::string::npos
        || lower.find("caliper") != std::string::npos || lower.find("rotor") != std::string::npos) {
        return CarBin::Kind::Brake;
    }
    return CarBin::Kind::Car;
}

Vertex decodeVertex(const CarBin::Part& part, const CarBin::VertexBuffer& buffer, std::uint32_t index)
{
    Vertex v;
    if (buffer.stride != kVertexStride || index >= buffer.count
        || buffer.data.size() < (static_cast<std::size_t>(index) + 1) * kVertexStride) {
        return v;
    }
    const std::uint8_t* p = buffer.data.data() + static_cast<std::size_t>(index) * kVertexStride;
    const auto s16
        = [p](int offset) { return static_cast<float>(static_cast<std::int16_t>(loadBe16(p + offset))) * kS16; };
    const auto u16 = [p](int offset) { return static_cast<float>(loadBe16(p + offset)) * kU16; };

    const glm::vec3 file = decodeFilePosition(p, glm::vec4(part.position, glm::length(part.halfExtent)));
    v.position = glm::vec3(file.x, file.y, -file.z);

    v.rawUv0 = glm::vec2(u16(8), u16(10));
    v.rawUv1 = glm::vec2(u16(12), u16(14));

    const glm::quat frame(s16(22), s16(16), s16(18), s16(20));
    const glm::vec3 normal = frame * glm::vec3(1.0F, 0.0F, 0.0F);
    const glm::vec3 tangent = frame * glm::vec3(0.0F, 1.0F, 0.0F);
    const auto toCar = [](glm::vec3 n) {
        const float length2 = glm::dot(n, n);
        if (length2 < 1e-12F) {
            return glm::vec3(0.0F, 1.0F, 0.0F);
        }
        n /= std::sqrt(length2);
        return glm::vec3(n.x, n.y, -n.z);
    };
    v.normal = toCar(normal);
    v.tangent = toCar(tangent);
    std::copy(p + 24, p + 28, v.lighting.begin());
    return v;
}

glm::vec3 decodeFilePosition(const std::uint8_t* vertex, const glm::vec4& packed)
{
    const auto s16 = [vertex](int offset) {
        return static_cast<float>(static_cast<std::int16_t>(loadBe16(vertex + offset))) * kS16;
    };
    const glm::vec3 direction(s16(0), s16(2), s16(4));
    return direction * (s16(6) * packed.w) + glm::vec3(packed);
}

glm::vec2 meshUv0(const CarBin::Mesh& mesh, const Vertex& vertex)
{
    return dequantise(mesh.uv0, vertex.rawUv0);
}

glm::vec2 meshUv1(const CarBin::Mesh& mesh, const Vertex& vertex)
{
    return dequantise(mesh.uv1, vertex.rawUv1);
}

void appendTriangles(std::span<const std::uint32_t> indices, std::uint32_t primitive, std::uint32_t vertexCount,
    std::vector<std::uint32_t>& out)
{
    const auto add = [&out](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a != b && b != c && a != c) {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    };
    if (primitive == kTriangleList) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            if (indices[i] < vertexCount && indices[i + 1] < vertexCount && indices[i + 2] < vertexCount) {
                add(indices[i], indices[i + 1], indices[i + 2]);
            }
        }
        return;
    }
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertexCount) {
            runStart = i + 1;
            continue;
        }
        if (i >= runStart + 2) {
            if ((i - runStart - 2) % 2 == 0) {
                add(indices[i - 2], indices[i - 1], indices[i]);
            } else {
                add(indices[i - 1], indices[i - 2], indices[i]);
            }
        }
    }
}

} // namespace carbin
} // namespace openhorizon::carmodel
