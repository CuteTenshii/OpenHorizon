#include "SkinnedModel.h"

#include <algorithm>
#include <limits>

namespace fh::spectators {

namespace {

constexpr std::uint32_t kMarker = 1;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxLods = 16;
constexpr std::uint32_t kMaxVertices = 65536;
constexpr std::uint32_t kMaxIndices = 1U << 22;
constexpr float kTexcoordScale = 1.0F / 65535.0F;

glm::vec3 readVec3(BeReader& r)
{
    const float x = r.f32();
    const float y = r.f32();
    const float z = r.f32();
    return {x, y, z};
}

} // namespace

int SkinnedModel::boneCount() const
{
    int count = 0;
    for (const Vertex& vertex : vertices) {
        for (std::size_t i = 0; i < vertex.bones.size(); ++i) {
            if (vertex.weights[i] > 0) {
                count = std::max(count, vertex.bones[i] + 1);
            }
        }
    }
    return count;
}

float SkinnedModel::height() const
{
    if (vertices.empty()) {
        return 0.0F;
    }
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const Vertex& vertex : vertices) {
        low = std::min(low, vertex.position.y);
        high = std::max(high, vertex.position.y);
    }
    return high - low;
}

SkinnedModel parseSkinnedModel(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    r.expect(kVersion, "version");
    const std::uint32_t lodCount = r.u32();
    if (lodCount == 0 || lodCount > kMaxLods) {
        r.fail(std::format("implausible level count {}", lodCount));
    }
    r.expect(kMarker, "level array marker");
    r.expect(lodCount, "level array length");

    SkinnedModel model;
    model.lods.resize(lodCount);
    for (SkinnedModel::Lod& lod : model.lods) {
        r.expect(kMarker, "level marker");
        lod.firstIndex = r.u16();
        lod.indexCount = r.u16();
    }
    r.expect(kMarker, "vertex stream marker");
    r.expect(kMarker, "vertex stream count");
    const std::uint32_t vertexCount = r.u32();
    if (vertexCount > kMaxVertices) {
        r.fail(std::format("implausible vertex count {}", vertexCount));
    }
    model.vertices.resize(vertexCount);
    for (SkinnedModel::Vertex& vertex : model.vertices) {
        r.expect(kMarker, "vertex marker");
        vertex.position = readVec3(r);
        vertex.normal = readVec3(r);
        const float u = static_cast<float>(r.u16()) * kTexcoordScale;
        const float v = static_cast<float>(r.u16()) * kTexcoordScale;
        vertex.texcoord = {u, v};
        vertex.bones = {r.u8(), r.u8()};
        vertex.weights = {r.u8(), r.u8()};
    }
    r.expect(kMarker, "index marker");
    const std::uint32_t indexCount = r.u32();
    if (indexCount > kMaxIndices || indexCount % 3 != 0) {
        r.fail(std::format("implausible index count {}", indexCount));
    }
    model.indices.resize(indexCount);
    for (std::uint16_t& index : model.indices) {
        index = r.u16();
        if (index >= vertexCount) {
            r.fail(std::format("index {} past the {} vertices", index, vertexCount));
        }
    }
    r.expect(vertexCount, "trailing vertex count");
    r.expect(indexCount, "trailing index count");

    for (const SkinnedModel::Lod& lod : model.lods) {
        if (lod.firstIndex % 3 != 0 || lod.indexCount % 3 != 0
            || static_cast<std::size_t>(lod.firstIndex) + lod.indexCount > indexCount) {
            throw DataError(
                "{}: level {}+{} does not fit the {} indices", source, lod.firstIndex, lod.indexCount, indexCount);
        }
    }
    return model;
}

} // namespace fh::spectators
