#include "RenderMesh.h"

#include <charconv>
#include <limits>

namespace openhorizon::world {

namespace {

constexpr std::uint32_t kVersion = 6;
constexpr std::size_t kPartCountOffset = 0x74;
constexpr std::size_t kFirstPartOffset = 0xB0;
constexpr std::uint32_t kMaxNameLength = 256;
constexpr std::uint32_t kMaxParts = 4096;
constexpr std::uint32_t kMaxMaterials = 4096;
constexpr std::uint32_t kMinStride = 12;
constexpr std::uint32_t kMaxStride = 64;
constexpr std::size_t kPartSeparatorBytes = 48;
constexpr std::uint32_t kStripRestart16 = 0xFFFF;
constexpr std::uint32_t kStripRestart32 = 0xFFFFFFFF;

/// A length-prefixed name, without its trailing NULs.
std::string readName(BeReader& r)
{
    const std::uint32_t length = r.u32();
    if (length == 0 || length > kMaxNameLength) {
        r.seek(r.pos() - 4);
        r.fail(std::format("implausible name length {}", length));
    }
    return r.fixedString(length);
}

/// The bounding box at 0x04, with Z negated; negating swaps which corner
/// holds the minimum.
void readBounds(BeReader& r, glm::vec3& boundsMin, glm::vec3& boundsMax)
{
    const float minX = r.f32();
    const float minY = r.f32();
    const float minZ = r.f32();
    r.skip(4);
    const float maxX = r.f32();
    const float maxY = r.f32();
    const float maxZ = r.f32();
    boundsMin = glm::vec3(minX, minY, -maxZ);
    boundsMax = glm::vec3(maxX, maxY, -minZ);
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

/// Length of the "LOD<digits>" token at `at`, or 0 if there is none.
std::size_t lodTokenLength(std::string_view text, std::size_t at)
{
    if (at + 3 >= text.size() || !iequals(text.substr(at, 3), "lod") || !isDigit(text[at + 3])) {
        return 0;
    }
    std::size_t end = at + 3;
    while (end < text.size() && isDigit(text[end])) {
        ++end;
    }
    return end - at;
}

/// Reads the material table and shader paths after the last part. The tail
/// only matters for texturing, so a layout this parser does not know leaves
/// the table empty instead of rejecting the model.
void readMaterialTable(BeReader& r, RenderMesh& mesh)
{
    constexpr std::uint32_t kMaxEntries = 4096;
    constexpr std::uint32_t kMaxConstants = 256;
    constexpr std::uint32_t kMaxSlots = 64;
    constexpr std::uint32_t kEngineSlot = 0x80000000;
    try {
        r.expect(1, "material table tag");
        r.expect(1, "material table tag");
        r.expect(1, "material table tag");
        const std::uint32_t count = r.u32();
        if (count > kMaxEntries) {
            r.fail(std::format("implausible material table size {}", count));
        }
        std::vector<RenderMesh::MaterialInfo> table;
        table.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            RenderMesh::MaterialInfo info;
            r.expect(3, "material entry tag");
            info.shader = r.u32();
            r.expect(0, "material entry header");
            // Two groups of shader constants (float4 each): the vertex
            // shader's, then the pixel shader's.
            for (int group = 0; group < 2; ++group) {
                r.expect(1, "shader constant tag");
                const std::uint32_t constants = r.u32();
                if (constants > kMaxConstants) {
                    r.fail(std::format("implausible shader constant count {}", constants));
                }
                for (std::uint32_t k = 0; k < constants; ++k) {
                    const float x = r.f32();
                    const float y = r.f32();
                    const float z = r.f32();
                    const float w = r.f32();
                    (group == 0 ? info.vertexConstants : info.constants).emplace_back(x, y, z, w);
                }
            }
            r.expect(1, "texture slot tag");
            const std::uint32_t slotCount = r.u32();
            if (slotCount > kMaxSlots) {
                r.fail(std::format("implausible texture slot count {}", slotCount));
            }
            for (std::uint32_t s = 0; s < slotCount; ++s) {
                const std::uint32_t slot = r.u32();
                info.textureSlots.push_back(slot >= kEngineSlot ? -1 : static_cast<int>(slot));
            }
            table.push_back(std::move(info));
        }
        r.expect(1, "shader list tag");
        const std::uint32_t shaderCount = r.u32();
        if (shaderCount > kMaxEntries) {
            r.fail(std::format("implausible shader count {}", shaderCount));
        }
        std::vector<std::string> shaders;
        shaders.reserve(shaderCount);
        for (std::uint32_t i = 0; i < shaderCount; ++i) {
            shaders.push_back(readName(r));
        }
        mesh.materialTable = std::move(table);
        mesh.shaders = std::move(shaders);
    } catch (const DataError&) {
        mesh.materialTable.clear();
        mesh.shaders.clear();
    }
}

/// Where the 4 bytes at `offset` of vertex `vertex` start in the part's
/// vertex data, or nothing if they lie outside the vertex.
std::optional<std::size_t> vertexField(const RenderMesh::Part& part, std::uint32_t vertex, int offset)
{
    if (offset < 0 || static_cast<std::uint32_t>(offset) + 4 > part.stride) {
        return std::nullopt;
    }
    const std::size_t at = static_cast<std::size_t>(vertex) * part.stride + static_cast<std::size_t>(offset);
    if (at + 4 > part.vertexData.size()) {
        return std::nullopt;
    }
    return at;
}

} // namespace

glm::vec2 RenderMesh::texcoord(const Part& part, const Material& material, std::uint32_t vertex, int offset)
{
    if (!vertexField(part, vertex, offset)) {
        return glm::vec2(0.0F);
    }
    const glm::vec2 raw = rawTexcoord(part, vertex, offset);
    const glm::vec4& t = material.uvOffsetScale;
    return {t.x + t.z * raw.x, t.y + t.w * raw.y};
}

glm::vec2 RenderMesh::rawTexcoord(const Part& part, std::uint32_t vertex, int offset)
{
    const std::optional<std::size_t> at = vertexField(part, vertex, offset);
    if (!at) {
        return glm::vec2(0.0F);
    }
    constexpr float kUnit = 1.0F / 65535.0F;
    const std::uint8_t* p = part.vertexData.data() + *at;
    return {static_cast<float>(loadBe16(p)) * kUnit, static_cast<float>(loadBe16(p + 2)) * kUnit};
}

std::uint32_t RenderMesh::colour(const Part& part, std::uint32_t vertex, int offset)
{
    const std::optional<std::size_t> at = vertexField(part, vertex, offset);
    return at ? loadBe32(part.vertexData.data() + *at) : 0;
}

std::size_t RenderMesh::triangleCount() const
{
    std::size_t count = 0;
    for (const Part& part : parts) {
        for (const Material& material : part.materials) {
            count += material.triangles.size() / 3;
        }
    }
    return count;
}

namespace rendermesh {

void appendTriangles(std::span<const std::uint32_t> indices, std::uint32_t restart, std::vector<std::uint32_t>& out)
{
    bool hasRestart = false;
    for (const std::uint32_t index : indices) {
        if (index == restart) {
            hasRestart = true;
            break;
        }
    }
    const auto addTriangle = [&out](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a != b && b != c && a != c) {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
        }
    };
    if (!hasRestart && indices.size() % 3 == 0) {
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            addTriangle(indices[i], indices[i + 1], indices[i + 2]);
        }
        return;
    }
    std::size_t runStart = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] == restart) {
            runStart = i + 1;
            continue;
        }
        if (i >= runStart + 2) {
            if ((i - runStart - 2) % 2 == 0) {
                addTriangle(indices[i - 2], indices[i - 1], indices[i]);
            } else {
                addTriangle(indices[i - 1], indices[i - 2], indices[i]);
            }
        }
    }
}

RenderMesh parse(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    r.expect(kVersion, "version");
    RenderMesh mesh;
    readBounds(r, mesh.boundsMin, mesh.boundsMax);

    r.seek(kPartCountOffset);
    const std::uint32_t partCount = r.u32();
    if (partCount == 0 || partCount > kMaxParts) {
        r.fail(std::format("implausible part count {}", partCount));
    }
    r.seek(kFirstPartOffset);
    mesh.parts.reserve(partCount);
    std::vector<std::uint32_t> indices;
    for (std::uint32_t p = 0; p < partCount; ++p) {
        if (p > 0) {
            r.skip(kPartSeparatorBytes);
            r.expect(1, "part separator");
        }
        RenderMesh::Part part;
        part.name = readName(r);
        r.expect(3, "vertex block tag");
        const std::uint32_t vertexCount = r.u32();
        part.stride = r.u32();
        r.expect(0, "vertex block padding");
        if (part.stride < kMinStride || part.stride > kMaxStride) {
            r.fail(std::format("unsupported vertex stride {}", part.stride));
        }
        const ByteView vertices = r.bytes(static_cast<std::size_t>(vertexCount) * part.stride);
        part.vertexData.assign(vertices.begin(), vertices.end());
        part.positions.reserve(vertexCount);
        for (std::uint32_t v = 0; v < vertexCount; ++v) {
            const std::uint8_t* at = vertices.data() + static_cast<std::size_t>(v) * part.stride;
            part.positions.emplace_back(loadBeF32(at), loadBeF32(at + 4), -loadBeF32(at + 8));
        }

        r.expect(1, "material list tag");
        const std::uint32_t materialCount = r.u32();
        r.expect(1, "material list tag");
        if (materialCount > kMaxMaterials) {
            r.fail(std::format("implausible material count {}", materialCount));
        }
        part.materials.reserve(materialCount);
        for (std::uint32_t m = 0; m < materialCount; ++m) {
            r.expect(2, "material tag");
            RenderMesh::Material material;
            material.name = readName(r);
            r.expect(0, "material header");
            r.u32();
            material.tableIndex = r.u32();
            r.expect(1, "material header");
            const float xOffset = r.f32();
            const float yOffset = r.f32();
            const float zOffset = r.f32();
            r.skip(4);
            const float xScale = r.f32();
            const float yScale = r.f32();
            const float zScale = r.f32();
            r.skip(4);
            material.positionOffset = glm::vec3(xOffset, yOffset, zOffset);
            material.positionScale = glm::vec3(xScale, yScale, zScale);
            const float uOffset = r.f32();
            const float vOffset = r.f32();
            const float uScale = r.f32();
            const float vScale = r.f32();
            material.uvOffsetScale = glm::vec4(uOffset, vOffset, uScale, vScale);
            r.expect(4, "index buffer tag");
            r.expect(0, "index buffer header");
            const std::uint32_t indexCount = r.u32();
            const std::uint32_t width = r.u32();
            if (width != 2 && width != 4) {
                r.fail(std::format("unsupported index width {}", width));
            }
            const std::size_t indexStart = r.pos();
            const ByteView raw = r.bytes(static_cast<std::size_t>(indexCount) * width);
            const std::uint32_t restart = width == 2 ? kStripRestart16 : kStripRestart32;
            indices.resize(indexCount);
            for (std::uint32_t i = 0; i < indexCount; ++i) {
                const std::uint8_t* at = raw.data() + static_cast<std::size_t>(i) * width;
                indices[i] = width == 2 ? loadBe16(at) : loadBe32(at);
                if (indices[i] != restart && indices[i] >= vertexCount) {
                    r.seek(indexStart + static_cast<std::size_t>(i) * width);
                    r.fail(std::format("index {} is past the {} vertices", indices[i], vertexCount));
                }
            }
            appendTriangles(indices, restart, material.triangles);
            r.u32();
            r.expect(1, "material end");
            part.materials.push_back(std::move(material));
        }
        mesh.parts.push_back(std::move(part));
    }
    readMaterialTable(r, mesh);
    return mesh;
}

std::optional<RenderMeshHeader> readHeader(ByteView prefix)
{
    try {
        BeReader r(prefix, "render model header");
        r.expect(kVersion, "version");
        RenderMeshHeader header;
        readBounds(r, header.boundsMin, header.boundsMax);
        r.seek(kPartCountOffset);
        header.partCount = r.u32();
        r.seek(kFirstPartOffset);
        header.firstPartName = readName(r);
        return header;
    } catch (const DataError&) {
        return std::nullopt;
    }
}

int lodLevel(std::string_view partName)
{
    for (std::size_t i = 0; i < partName.size(); ++i) {
        if (i > 0 && partName[i - 1] != '_') {
            continue;
        }
        const std::size_t length = lodTokenLength(partName, i);
        if (length == 0) {
            continue;
        }
        int level = 0;
        const char* first = partName.data() + i + 3;
        const auto [ptr, ec] = std::from_chars(first, partName.data() + i + length, level);
        return ec == std::errc() ? level : std::numeric_limits<int>::max();
    }
    return -1;
}

std::string lodGroupKey(std::string_view partName)
{
    std::string key;
    key.reserve(partName.size());
    for (std::size_t i = 0; i < partName.size();) {
        const std::size_t withUnderscore = partName[i] == '_' ? lodTokenLength(partName, i + 1) : 0;
        if (withUnderscore > 0) {
            i += 1 + withUnderscore;
            continue;
        }
        const std::size_t bare = lodTokenLength(partName, i);
        if (bare > 0) {
            i += bare;
            continue;
        }
        key.push_back(partName[i]);
        ++i;
    }
    // Trailing chunk numbers: the whole run of underscores and digits at the
    // end, from its first underscore on.
    std::size_t runStart = key.size();
    while (runStart > 0 && (key[runStart - 1] == '_' || isDigit(key[runStart - 1]))) {
        --runStart;
    }
    const std::size_t underscore = key.find('_', runStart);
    if (underscore != std::string::npos) {
        key.resize(underscore);
    }
    return toLower(key);
}

} // namespace rendermesh
} // namespace openhorizon::world
