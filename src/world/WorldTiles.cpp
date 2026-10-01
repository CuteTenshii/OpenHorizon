#include "WorldTiles.h"

#include "Archive.h"

#include <glm/geometric.hpp>

#include <lzx/zip.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>

namespace openhorizon::world {

namespace {

std::uint64_t cellKey(int x, int z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

/// A material's batch (less the index range) and where its vertices keep the inputs it reads.
struct SurfaceSource {
    TileMesh::Batch surface;
    int texcoord0Offset = -1;
    int texcoord1Offset = -1;
    int colourOffset = -1;
};

/// Orders batches that draw alike, so identical surfaces share one.
using BatchKey = std::tuple<bool, std::uint32_t, TileMesh::Shading, std::uint32_t,
    std::array<std::uint32_t, TileMesh::LayerCount>, std::array<float, 6>>;

BatchKey batchKey(const TileMesh::Batch& b)
{
    return {b.backdrop, b.chunk, b.shading, b.texture, b.layers, b.layerScales};
}

/// A layer's texture scale from a material constant, where it gives one.
float layerScale(float value)
{
    return value > 0.0F ? value : 1.0F;
}

SurfaceSource surfaceSource(const RenderMesh& mesh, const RenderMesh::Part& part, const RenderMesh::Material& material,
    const std::vector<std::uint32_t>* objectTextures, const TrackTextures& textures)
{
    if (objectTextures == nullptr || material.tableIndex >= mesh.materialTable.size()) {
        return {};
    }
    const RenderMesh::MaterialInfo& info = mesh.materialTable[material.tableIndex];
    if (info.shader >= mesh.shaders.size()) {
        return {};
    }
    const ShaderLayout* layout = textures.shader(mesh.shaders[info.shader]);
    const auto fits
        = [&part](int offset) { return offset >= 0 && static_cast<std::uint32_t>(offset) + 4 <= part.stride; };
    if (layout == nullptr || !fits(layout->texcoord0Offset)) {
        return {};
    }
    // Material slot k feeds sampler register k.
    const auto textureOf = [&](std::string_view sampler) -> std::uint32_t {
        const int reg = layout->samplerRegister(sampler);
        if (reg < 0 || static_cast<std::size_t>(reg) >= info.textureSlots.size()) {
            return TileMesh::kNoTexture;
        }
        const int slot = info.textureSlots[static_cast<std::size_t>(reg)];
        return slot >= 0 && static_cast<std::size_t>(slot) < objectTextures->size()
            ? (*objectTextures)[static_cast<std::size_t>(slot)]
            : TileMesh::kNoTexture;
    };
    SurfaceSource source;
    source.texcoord0Offset = layout->texcoord0Offset;
    TileMesh::Batch& surface = source.surface;

    const std::uint32_t normalMap = textureOf("NormalMapASampler");
    const std::uint32_t blendA = textureOf("Blend_ASampler");
    const std::uint32_t blendB = textureOf("Blend_BSampler");
    const std::uint32_t splat = textureOf("Splat_Sampler");
    if (normalMap != TileMesh::kNoTexture && layout->samplerRegister("CubeMapSampler") >= 0) {
        surface.shading = TileMesh::Shading::Water;
        surface.texture = normalMap;
        return source;
    }
    // Splat and occlusion maps span a patch once, on the last coordinate pair the shader reads.
    const int patchOffset = fits(layout->texcoord2Offset) ? layout->texcoord2Offset
        : fits(layout->texcoord1Offset)                   ? layout->texcoord1Offset
                                                          : -1;
    const bool splatGround = blendA != TileMesh::kNoTexture && blendB != TileMesh::kNoTexture
        && splat != TileMesh::kNoTexture && patchOffset >= 0;
    // Roads mix their Blend layers with noise and modulate maps, not a vertex colour; they stay plain.
    const bool roadLayers
        = layout->samplerRegister("Noise_Sampler") >= 0 || layout->samplerRegister("Modulate_Sampler") >= 0;
    const bool vertexGround = blendA != TileMesh::kNoTexture && blendB != TileMesh::kNoTexture
        && splat == TileMesh::kNoTexture && !roadLayers && fits(layout->colourOffset);
    if (splatGround || vertexGround) {
        surface.shading = splatGround ? TileMesh::Shading::Splat : TileMesh::Shading::VertexBlend;
        surface.texture = blendA;
        surface.layers[TileMesh::LayerB] = blendB;
        surface.layers[TileMesh::LayerC] = splatGround ? textureOf("Blend_CSampler") : TileMesh::kNoTexture;
        surface.layers[TileMesh::SplatMap] = splatGround ? splat : TileMesh::kNoTexture;
        if (patchOffset >= 0) {
            source.texcoord1Offset = patchOffset;
            surface.layers[TileMesh::OcclusionMap] = textureOf("AO_Sampler");
        }
        if (vertexGround) {
            source.colourOffset = layout->colourOffset;
        }
        // The first constant holds Blend_A's and Blend_B's scales, the second Blend_C's.
        if (!info.constants.empty()) {
            const glm::vec4& ab = info.constants[0];
            surface.layerScales[0] = layerScale(ab.x);
            surface.layerScales[1] = layerScale(ab.y);
            surface.layerScales[2] = layerScale(ab.z);
            surface.layerScales[3] = layerScale(ab.w);
        }
        if (info.constants.size() > 1) {
            surface.layerScales[4] = layerScale(info.constants[1].x);
            surface.layerScales[5] = layerScale(info.constants[1].y);
        }
        return source;
    }
    // Register 0 holds the diffuse texture of every other track shader.
    if (!info.textureSlots.empty() && info.textureSlots.front() >= 0
        && static_cast<std::size_t>(info.textureSlots.front()) < objectTextures->size()) {
        surface.texture = (*objectTextures)[static_cast<std::size_t>(info.textureSlots.front())];
    }
    return source;
}

/// The determinant of a placement's matrix: negative when it mirrors.
float determinant(const std::array<float, 9>& r)
{
    return r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
}

} // namespace

EventPropFilter EventPropFilter::all()
{
    EventPropFilter filter;
    filter.m_all = true;
    return filter;
}

EventPropFilter EventPropFilter::route(int routeId)
{
    EventPropFilter filter;
    filter.m_routeId = routeId;
    return filter;
}

bool EventPropFilter::includes(const WorldChunk& chunk) const
{
    if (!chunk.placed || !chunk.placement.eventProp || m_all) {
        return true;
    }
    return m_routeId >= 0 && chunk.placement.eventTag.empty() && chunk.placement.onRoute(m_routeId);
}

WorldTileGrid::WorldTileGrid(
    const WorldIndex& index, float tileSize, const EventPropFilter& eventProps, const std::vector<bool>& hidden)
    : m_index(index)
    , m_tileSize(tileSize > 0.0F ? tileSize : 1.0F)
    , m_origin(index.footprint().min)
{
    const auto& chunks = index.chunks();
    for (std::uint32_t i = 0; i < chunks.size(); ++i) {
        const WorldChunk& chunk = chunks[i];
        // Every chunk gets a tile, so a filter change only changes what tiles draw.
        const glm::vec3 centre = (chunk.boundsMin + chunk.boundsMax) / 2.0F;
        const auto [cx, cz] = cellAt(centre.x, centre.z);
        const auto [it, inserted] = m_tileByCell.try_emplace(cellKey(cx, cz), m_tiles.size());
        if (inserted) {
            Tile tile;
            tile.cellX = cx;
            tile.cellZ = cz;
            tile.boundsMin = chunk.boundsMin;
            tile.boundsMax = chunk.boundsMax;
            m_tiles.push_back(std::move(tile));
        }
        Tile& tile = m_tiles[it->second];
        tile.allChunks.push_back(i);
        tile.boundsMin = glm::min(tile.boundsMin, chunk.boundsMin);
        tile.boundsMax = glm::max(tile.boundsMax, chunk.boundsMax);
    }
    update(eventProps, hidden);
}

std::pair<int, int> WorldTileGrid::cellAt(float x, float z) const
{
    return {static_cast<int>(std::floor((x - m_origin.x) / m_tileSize)),
        static_cast<int>(std::floor((z - m_origin.y) / m_tileSize))};
}

std::optional<std::size_t> WorldTileGrid::tileAt(int cellX, int cellZ) const
{
    const auto it = m_tileByCell.find(cellKey(cellX, cellZ));
    return it == m_tileByCell.end() ? std::nullopt : std::optional<std::size_t>(it->second);
}

std::vector<std::size_t> WorldTileGrid::update(const EventPropFilter& eventProps, const std::vector<bool>& hidden)
{
    const auto& chunks = m_index.chunks();
    std::vector<std::size_t> changed;
    std::vector<std::uint32_t> drawn;
    for (std::size_t t = 0; t < m_tiles.size(); ++t) {
        Tile& tile = m_tiles[t];
        drawn.clear();
        for (const std::uint32_t c : tile.allChunks) {
            if (eventProps.includes(chunks[c]) && !(c < hidden.size() && hidden[c])) {
                drawn.push_back(c);
            }
        }
        if (m_built && drawn == tile.chunks) {
            continue;
        }
        tile.chunks = drawn;
        tile.edges.clear();
        for (const std::uint32_t c : tile.chunks) {
            tile.edges.push_back(chunks[c].bandStart);
            tile.edges.push_back(chunks[c].bandEnd);
        }
        std::sort(tile.edges.begin(), tile.edges.end());
        tile.edges.erase(std::unique(tile.edges.begin(), tile.edges.end()), tile.edges.end());
        changed.push_back(t);
    }
    m_built = true;
    return changed;
}

float WorldTileGrid::distanceTo(const Tile& tile, float x, float z) const
{
    const float dx = std::max({tile.boundsMin.x - x, 0.0F, x - tile.boundsMax.x});
    const float dz = std::max({tile.boundsMin.z - z, 0.0F, z - tile.boundsMax.z});
    return std::hypot(dx, dz);
}

int WorldTileGrid::stateAt(const Tile& tile, float distance) const
{
    return static_cast<int>(std::upper_bound(tile.edges.begin(), tile.edges.end(), distance) - tile.edges.begin());
}

std::vector<std::uint32_t> WorldTileGrid::chunksAt(const Tile& tile, float distance) const
{
    std::vector<std::uint32_t> result;
    const auto& chunks = m_index.chunks();
    for (const std::uint32_t c : tile.chunks) {
        if (distance >= chunks[c].bandStart && distance < chunks[c].bandEnd) {
            result.push_back(c);
        }
    }
    return result;
}

std::vector<std::uint32_t> WorldTileGrid::chunksBeyond(const Tile& tile, float distance) const
{
    std::vector<std::uint32_t> result;
    const auto& chunks = m_index.chunks();
    for (const std::uint32_t c : tile.chunks) {
        if (chunks[c].bandEnd > distance) {
            result.push_back(c);
        }
    }
    return result;
}

bool isNotSurface(const RenderMesh& mesh, const RenderMesh::Material& material)
{
    if (startsWithNoCase(material.name, "Placeholder") || iequals(material.name, "CrowdTERR")) {
        return true;
    }
    if (material.tableIndex >= mesh.materialTable.size()) {
        return false;
    }
    const std::uint32_t shader = mesh.materialTable[material.tableIndex].shader;
    return shader < mesh.shaders.size()
        && toLower(mesh.shaders[shader]).find("light_pollution") != std::string::npos;
}

TileMesh buildTileMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackTextures* textures)
{
    TileMesh out;
    // Concatenated into out.indices at the end; only backdrop is kept apart by chunk.
    std::map<BatchKey, std::pair<TileMesh::Batch, std::vector<std::uint32_t>>> batchIndices;
    const auto& entries = archive.entries();
    const auto fail = [&out](std::uint32_t chunk, const std::string& error) {
        ++out.failedChunks;
        out.errors.push_back(error);
        out.models.push_back({chunk, 0, {}, error});
    };
    // A prop placed many times in the tile is read and parsed once.
    std::unordered_map<std::uint32_t, std::shared_ptr<const RenderMesh>> meshes;
    std::unordered_map<std::uint32_t, std::string> failures;
    std::vector<glm::vec3> placedPositions;
    std::vector<glm::vec3> normals;
    std::vector<std::uint32_t> remap;
    for (const std::uint32_t c : chunks) {
        if (c >= index.chunks().size()) {
            fail(c, std::format("chunk {} is not in the index", c));
            continue;
        }
        const WorldChunk& chunk = index.chunks()[c];
        if (chunk.entry >= entries.size()) {
            fail(c, std::format("chunk {} refers to a missing archive entry", c));
            continue;
        }
        const lzx::ZipEntry& entry = entries[chunk.entry];
        if (const auto failed = failures.find(chunk.entry); failed != failures.end()) {
            fail(c, failed->second);
            continue;
        }
        std::shared_ptr<const RenderMesh>& cached = meshes[chunk.entry];
        if (!cached) {
            try {
                cached = std::make_shared<const RenderMesh>(rendermesh::parse(readEntry(archive, entry), entry.name));
            } catch (const DataError& e) {
                meshes.erase(chunk.entry);
                const std::string& error = failures.emplace(chunk.entry, e.what()).first->second;
                fail(c, error);
                continue;
            }
        }
        const RenderMesh& mesh = *cached;
        // The parsed model's Z flip already mirrors, so a positive determinant is the mirroring placement.
        const bool mirrored = chunk.placed && determinant(chunk.placement.rows) > 0.0F;
        const float handedness = mirrored ? -1.0F : 1.0F;
        TileMesh::Model model;
        model.chunk = c;
        model.triangles = static_cast<std::uint32_t>(mesh.triangleCount());
        const std::vector<std::uint32_t>* objectTextures = nullptr;
        if (textures != nullptr) {
            if (const std::optional<std::uint32_t> object = TrackTextures::objectNumber(entry.name)) {
                objectTextures = textures->objectTextures(*object);
            }
        }
        for (const RenderMesh::Part& part : mesh.parts) {
            if (chunk.placed) {
                placedPositions.clear();
                placedPositions.reserve(part.positions.size());
                for (const glm::vec3& p : part.positions) {
                    placedPositions.push_back(chunk.placement.apply(p));
                }
            }
            const std::vector<glm::vec3>& positions = chunk.placed ? placedPositions : part.positions;
            normals.assign(positions.size(), glm::vec3(0.0F));
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material)) {
                    continue;
                }
                for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                    const std::uint32_t a = material.triangles[t];
                    const std::uint32_t b = material.triangles[t + 1];
                    const std::uint32_t d = material.triangles[t + 2];
                    // Unnormalized to weigh by area; operands swapped for the Z flip's reversed winding.
                    const glm::vec3 n
                        = handedness * glm::cross(positions[d] - positions[a], positions[b] - positions[a]);
                    normals[a] += n;
                    normals[b] += n;
                    normals[d] += n;
                }
            }
            // Materials sharing a vertex can transform its UVs differently, so each gets its own copies.
            remap.resize(positions.size());
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material)) {
                    continue;
                }
                SurfaceSource source = textures != nullptr
                    ? surfaceSource(mesh, part, material, objectTextures, *textures)
                    : SurfaceSource{};
                source.surface.backdrop = chunk.backdrop;
                source.surface.chunk = chunk.backdrop ? c : TileMesh::kMergedChunks;
                std::fill(remap.begin(), remap.end(), TileMesh::kNoTexture);
                for (const std::uint32_t id : source.surface.textures()) {
                    if (std::find(model.textures.begin(), model.textures.end(), id) == model.textures.end()) {
                        model.textures.push_back(id);
                    }
                }
                auto& [batch, target] = batchIndices[batchKey(source.surface)];
                std::vector<ChunkRun> runs = std::move(batch.runs);
                batch = source.surface;
                batch.runs = std::move(runs);
                const auto runStart = static_cast<std::uint32_t>(target.size());
                for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                    // Mirrored placements get their winding turned back.
                    const std::array<std::uint32_t, 3> corners = mirrored
                        ? std::array<std::uint32_t, 3>{material.triangles[t], material.triangles[t + 2],
                              material.triangles[t + 1]}
                        : std::array<std::uint32_t, 3>{
                              material.triangles[t], material.triangles[t + 1], material.triangles[t + 2]};
                    for (const std::uint32_t v : corners) {
                        if (remap[v] == TileMesh::kNoTexture) {
                            remap[v] = static_cast<std::uint32_t>(out.vertexCount());
                            const glm::vec3& p = positions[v];
                            const float length = glm::length(normals[v]);
                            const glm::vec3 n = length > 0.0F ? normals[v] / length : glm::vec3(0.0F, 1.0F, 0.0F);
                            // The material's offset and scale decode both coordinate pairs.
                            const glm::vec2 uv = source.texcoord0Offset >= 0
                                ? RenderMesh::texcoord(part, material, v, source.texcoord0Offset)
                                : glm::vec2(0.0F);
                            const glm::vec2 uv1 = source.texcoord1Offset >= 0
                                ? RenderMesh::texcoord(part, material, v, source.texcoord1Offset)
                                : glm::vec2(0.0F);
                            // File-order bytes in a float; the renderer reads four normalized bytes.
                            std::uint32_t colourBytes = 0xFFFFFFFFU;
                            if (source.colourOffset >= 0) {
                                const std::uint32_t colour = RenderMesh::colour(part, v, source.colourOffset);
                                const std::array<std::uint8_t, 4> bytes{static_cast<std::uint8_t>(colour >> 24),
                                    static_cast<std::uint8_t>(colour >> 16), static_cast<std::uint8_t>(colour >> 8),
                                    static_cast<std::uint8_t>(colour)};
                                std::memcpy(&colourBytes, bytes.data(), sizeof colourBytes);
                            }
                            const float packed = std::bit_cast<float>(colourBytes);
                            out.vertices.insert(out.vertices.end(),
                                {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y, uv1.x, uv1.y, packed});
                        }
                        target.push_back(remap[v]);
                    }
                }
                appendChunkRun(batch.runs, c, runStart, static_cast<std::uint32_t>(target.size()) - runStart);
            }
        }
        out.models.push_back(std::move(model));
    }
    for (auto& [key, entry] : batchIndices) {
        auto& [batch, indices] = entry;
        if (indices.empty()) {
            continue;
        }
        batch.firstIndex = static_cast<std::uint32_t>(out.indices.size());
        batch.indexCount = static_cast<std::uint32_t>(indices.size());
        for (ChunkRun& run : batch.runs) {
            run.firstIndex += batch.firstIndex;
        }
        out.batches.push_back(batch);
        out.indices.insert(out.indices.end(), indices.begin(), indices.end());
    }
    return out;
}

std::vector<std::uint32_t> TileMesh::Batch::textures() const
{
    std::vector<std::uint32_t> ids;
    if (texture != kNoTexture) {
        ids.push_back(texture);
    }
    for (const std::uint32_t id : layers) {
        if (id != kNoTexture && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::vector<std::uint32_t> TileMesh::textures() const
{
    std::vector<std::uint32_t> ids;
    for (const Batch& batch : batches) {
        for (const std::uint32_t id : batch.textures()) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
    }
    return ids;
}

} // namespace openhorizon::world
