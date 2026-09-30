#include "ShadedTiles.h"

#include "Archive.h"
#include "WorldTiles.h"

#include <lzx/zip.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace openhorizon::world {

namespace {

/// Texture slot values at or above this are engine textures (0xFFFFFFFE in
/// the files); RenderMesh keeps them as -1.
constexpr int kEngineSlot = -1;

struct BatchKey {
    std::uint32_t shader = 0;
    std::array<std::uint32_t, ShadedMesh::kSamplers> textures{};
    std::vector<float> constants;
    bool backdrop = false;
    std::uint32_t chunk = ShadedMesh::kMergedChunks;
    /// The chunk of a batch kept in its model's space, which merges with
    /// no other chunk's.
    std::uint32_t modelChunk = ShadedMesh::kMergedChunks;

    bool operator<(const BatchKey& other) const
    {
        return std::tie(shader, textures, constants, backdrop, chunk, modelChunk)
            < std::tie(other.shader, other.textures, other.constants, other.backdrop, other.chunk, other.modelChunk);
    }
};

struct BatchData {
    ShadedMesh::Batch batch;
    Bytes vertices;
    std::vector<std::uint32_t> indices;
};

/// A material's constants laid out as ShadedMesh::Batch::constants.
std::vector<glm::vec4> batchConstants(const RenderMesh::MaterialInfo& info)
{
    std::vector<glm::vec4> constants(info.constants.begin(),
        info.constants.begin()
            + static_cast<std::ptrdiff_t>(std::min(info.constants.size(), ShadedMesh::kFirstVertexConstant)));
    if (info.vertexConstants.empty()) {
        return constants;
    }
    constants.resize(ShadedMesh::kFirstVertexConstant, glm::vec4(0.0F));
    const std::size_t vertexCount
        = std::min(info.vertexConstants.size(), ShadedMesh::kMaterialConstants - ShadedMesh::kFirstVertexConstant);
    constants.insert(constants.end(), info.vertexConstants.begin(),
        info.vertexConstants.begin() + static_cast<std::ptrdiff_t>(vertexCount));
    return constants;
}

float determinant(const std::array<float, 9>& r)
{
    return r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
}

void appendFloat(Bytes& out, float value)
{
    std::uint8_t bytes[4];
    std::memcpy(bytes, &value, sizeof bytes);
    out.insert(out.end(), bytes, bytes + 4);
}

/// A DEC3N direction as the GPU reads it: signed 10-bit x, y, z from the low
/// bits up, each divided by 511 and clamped to -1.
glm::vec3 decodeDec3N(std::uint32_t word)
{
    const auto component = [word](int shift) {
        std::int32_t v = static_cast<std::int32_t>((word >> shift) & 0x3FF);
        if (v >= 512) {
            v -= 1024;
        }
        return std::max(static_cast<float>(v) / 511.0F, -1.0F);
    };
    return {component(0), component(10), component(20)};
}

/// Maps model-file coordinates (Z as stored) of a chunk to world
/// coordinates, for points and for directions; or with `modelSpace`, for a
/// placed chunk, leaves them as they are.
class ChunkTransform {
public:
    ChunkTransform(const WorldChunk& chunk, bool modelSpace)
        : m_chunk(chunk)
        , m_origin(chunk.placed ? chunk.placement.apply(glm::vec3(0.0F)) : glm::vec3(0.0F))
        , m_modelSpace(modelSpace && chunk.placed)
    {
    }

    glm::vec3 point(const glm::vec3& file) const
    {
        if (m_modelSpace) {
            return file;
        }
        const glm::vec3 parsed(file.x, file.y, -file.z);
        return m_chunk.placed ? m_chunk.placement.apply(parsed) : parsed;
    }
    glm::vec3 direction(const glm::vec3& file) const
    {
        if (m_modelSpace) {
            return file;
        }
        const glm::vec3 parsed(file.x, file.y, -file.z);
        return m_chunk.placed ? m_chunk.placement.apply(parsed) - m_origin : parsed;
    }

private:
    const WorldChunk& m_chunk;
    glm::vec3 m_origin;
    bool m_modelSpace = false;
};

/// The matrix taking a placed chunk's model-file coordinates (Z as stored)
/// to world coordinates, as Placement::apply() does.
glm::mat4 placementMatrix(const Placement& placement)
{
    const std::array<float, 9>& r = placement.rows;
    return {glm::vec4(r[0], r[1], r[2], 0.0F), glm::vec4(r[3], r[4], r[5], 0.0F), glm::vec4(r[6], r[7], r[8], 0.0F),
        glm::vec4(placement.position, 1.0F)};
}

/// Appends vertex `v` of `part`, converted as ShadedMesh describes, to `out`.
void convertVertex(const RenderMesh::Part& part, const RenderMesh::Material& material, std::uint32_t v,
    const xenos::VertexDeclaration& declaration, const ChunkTransform& transform, Bytes& out)
{
    const std::uint8_t* vertex = part.vertexData.data() + static_cast<std::size_t>(v) * part.stride;
    for (const xenos::VertexDeclElement& element : declaration.elements) {
        const std::uint8_t* at = vertex + element.offset;
        switch (element.type) {
        case decltype360::kFloat3: {
            const glm::vec3 raw(loadBeF32(at), loadBeF32(at + 4), loadBeF32(at + 8));
            const glm::vec3 world = element.usage == static_cast<std::uint8_t>(xenos::DeclUsage::Position)
                ? transform.point(raw * material.positionScale + material.positionOffset)
                : transform.direction(raw);
            appendFloat(out, world.x);
            appendFloat(out, world.y);
            appendFloat(out, world.z);
            break;
        }
        case decltype360::kDec3N: {
            const glm::vec3 world = transform.direction(decodeDec3N(loadBe32(at)));
            appendFloat(out, world.x);
            appendFloat(out, world.y);
            appendFloat(out, world.z);
            break;
        }
        case decltype360::kUShort2N: {
            constexpr float kUnit = 1.0F / 65535.0F;
            const glm::vec4& t = material.uvOffsetScale;
            appendFloat(out, t.x + t.z * static_cast<float>(loadBe16(at)) * kUnit);
            appendFloat(out, t.y + t.w * static_cast<float>(loadBe16(at + 2)) * kUnit);
            break;
        }
        default:
            // Colours keep their bytes; the declaration reads them as stored.
            out.insert(out.end(), at, at + 4);
            break;
        }
    }
}

std::uint32_t renderStride(const xenos::VertexDeclaration& declaration)
{
    return declaration.stride(0);
}

} // namespace

std::vector<std::uint32_t> ShadedMesh::Batch::textureIds() const
{
    std::vector<std::uint32_t> ids;
    for (const std::uint32_t id : textures) {
        if (id != kNoTexture && id != kEngineTexture && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::vector<std::uint32_t> ShadedMesh::textureIds() const
{
    std::vector<std::uint32_t> ids;
    for (const Batch& batch : batches) {
        for (const std::uint32_t id : batch.textureIds()) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    return ids;
}

ShadedMesh buildShadedMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackShaders& shaders, const TrackTextures* textures)
{
    ShadedMesh out;
    std::map<BatchKey, BatchData> batches;
    const auto& entries = archive.entries();
    std::unordered_map<std::uint32_t, std::shared_ptr<const RenderMesh>> meshes;
    std::unordered_map<std::uint32_t, std::string> failures;
    std::vector<std::uint32_t> remap;
    const auto fail = [&out](const std::string& error) {
        ++out.failedChunks;
        out.errors.push_back(error);
    };
    for (const std::uint32_t c : chunks) {
        if (c >= index.chunks().size()) {
            fail(std::format("chunk {} is not in the index", c));
            continue;
        }
        const WorldChunk& chunk = index.chunks()[c];
        if (chunk.entry >= entries.size()) {
            fail(std::format("chunk {} refers to a missing archive entry", c));
            continue;
        }
        const lzx::ZipEntry& entry = entries[chunk.entry];
        if (const auto failed = failures.find(chunk.entry); failed != failures.end()) {
            fail(failed->second);
            continue;
        }
        std::shared_ptr<const RenderMesh>& cached = meshes[chunk.entry];
        if (!cached) {
            try {
                cached = std::make_shared<const RenderMesh>(rendermesh::parse(readEntry(archive, entry), entry.name));
            } catch (const DataError& e) {
                meshes.erase(chunk.entry);
                fail(failures.emplace(chunk.entry, e.what()).first->second);
                continue;
            }
        }
        const RenderMesh& mesh = *cached;
        const std::vector<std::uint32_t>* objectTextures = nullptr;
        if (textures != nullptr) {
            if (const std::optional<std::uint32_t> object = TrackTextures::objectNumber(entry.name)) {
                objectTextures = textures->objectTextures(*object);
            }
        }
        // See buildTileMesh: with the parser's Z flip, a positive determinant
        // is the placement that mirrors, which turns triangles inside out.
        const bool mirrored = chunk.placed && determinant(chunk.placement.rows) > 0.0F;
        const ChunkTransform worldTransform(chunk, false);
        const ChunkTransform modelTransform(chunk, true);
        for (const RenderMesh::Part& part : mesh.parts) {
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material) || material.triangles.empty()) {
                    continue;
                }
                if (material.tableIndex >= mesh.materialTable.size()) {
                    ++out.skippedMaterials;
                    continue;
                }
                const RenderMesh::MaterialInfo& info = mesh.materialTable[material.tableIndex];
                const std::optional<std::uint32_t> shader
                    = info.shader < mesh.shaders.size() ? shaders.find(mesh.shaders[info.shader]) : std::nullopt;
                if (!shader) {
                    ++out.skippedMaterials;
                    continue;
                }
                const TrackShaders::Entry& effect = shaders.at(*shader);
                if (effect.fileDeclaration.stride(0) != part.stride) {
                    ++out.skippedMaterials;
                    continue;
                }
                BatchKey key;
                key.shader = *shader;
                key.textures.fill(ShadedMesh::kNoTexture);
                for (std::size_t k = 0; k < info.textureSlots.size() && k < ShadedMesh::kSamplers; ++k) {
                    const int slot = info.textureSlots[k];
                    if (slot == kEngineSlot) {
                        key.textures[k] = ShadedMesh::kEngineTexture;
                    } else if (objectTextures != nullptr && slot >= 0
                        && static_cast<std::size_t>(slot) < objectTextures->size()) {
                        key.textures[k] = (*objectTextures)[static_cast<std::size_t>(slot)];
                    }
                }
                std::vector<glm::vec4> constants = batchConstants(info);
                for (const glm::vec4& value : constants) {
                    key.constants.insert(key.constants.end(), {value.x, value.y, value.z, value.w});
                }
                key.backdrop = chunk.backdrop;
                key.chunk = chunk.backdrop ? c : ShadedMesh::kMergedChunks;
                const bool modelSpace = chunk.placed && effect.readsVertexMaterial;
                key.modelChunk = modelSpace ? c : ShadedMesh::kMergedChunks;
                const ChunkTransform& transform = modelSpace ? modelTransform : worldTransform;

                BatchData& data = batches[key];
                if (data.batch.stride == 0) {
                    data.batch.shader = key.shader;
                    data.batch.textures = key.textures;
                    data.batch.constants = std::move(constants);
                    data.batch.stride = renderStride(effect.renderDeclaration);
                    data.batch.backdrop = key.backdrop;
                    data.batch.chunk = key.chunk;
                    if (modelSpace) {
                        data.batch.model = placementMatrix(chunk.placement);
                    }
                }
                remap.assign(part.positions.size(), ShadedMesh::kNoTexture);
                const auto runStart = static_cast<std::uint32_t>(data.indices.size());
                for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                    const std::array<std::uint32_t, 3> corners = mirrored
                        ? std::array<std::uint32_t, 3>{material.triangles[t], material.triangles[t + 2],
                              material.triangles[t + 1]}
                        : std::array<std::uint32_t, 3>{
                              material.triangles[t], material.triangles[t + 1], material.triangles[t + 2]};
                    for (const std::uint32_t v : corners) {
                        if (remap[v] == ShadedMesh::kNoTexture) {
                            remap[v] = data.batch.vertexCount++;
                            convertVertex(part, material, v, effect.fileDeclaration, transform, data.vertices);
                        }
                        data.indices.push_back(remap[v]);
                    }
                }
                appendChunkRun(
                    data.batch.runs, c, runStart, static_cast<std::uint32_t>(data.indices.size()) - runStart);
            }
        }
    }
    for (auto& [key, data] : batches) {
        if (data.indices.empty()) {
            continue;
        }
        ShadedMesh::Batch batch = std::move(data.batch);
        const std::size_t aligned = (out.vertices.size() + ShadedMesh::kVertexAlignment - 1)
            / ShadedMesh::kVertexAlignment * ShadedMesh::kVertexAlignment;
        out.vertices.resize(aligned, 0);
        batch.vertexOffset = aligned;
        out.vertices.insert(out.vertices.end(), data.vertices.begin(), data.vertices.end());
        batch.firstIndex = static_cast<std::uint32_t>(out.indices.size());
        batch.indexCount = static_cast<std::uint32_t>(data.indices.size());
        for (ChunkRun& run : batch.runs) {
            run.firstIndex += batch.firstIndex;
        }
        out.indices.insert(out.indices.end(), data.indices.begin(), data.indices.end());
        out.batches.push_back(std::move(batch));
    }
    return out;
}

} // namespace openhorizon::world
