// Loads a track's crowd and reports on it: what was read, who stands near a
// point, a geometry export for inspection, and a check of the placements
// against the world's ground, roads and crowd areas.

#include "base/Log.h"
#include "spectators/CrowdSet.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/RenderMesh.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <glm/geometric.hpp>

#include <lzx/zip.h>
#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

namespace spectators = fh::spectators;
namespace world = fh::world;

constexpr float kFestivalRadius = 250.0F;
constexpr float kStartRadius = 150.0F;
constexpr float kGridCell = 8.0F;
/// How far above a person a surface may lie and still be the one they
/// stand on (the placements round positions to a few centimetres, and seat
/// heights sit above the stand's walking surface).
constexpr float kSurfaceSlack = 0.5F;
/// A racing line counts as the road beside a person within this distance
/// and height difference.
constexpr float kRoadSearch = 30.0F;
constexpr float kRoadHeightSlack = 5.0F;
constexpr std::size_t kOwtHeaderBytes = 32;
constexpr std::size_t kOwtPointBytes = 48;
constexpr std::size_t kOwtHalfWidthOffset = 24;
/// Half-width of a line whose points give none usable.
constexpr float kDefaultHalfWidth = 5.0F;
constexpr float kAreaHeightSlack = 2.0F;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1crowd DISC TRACK stats\n"
        "       fh1crowd DISC TRACK at X,Z RADIUS\n"
        "       fh1crowd DISC TRACK --obj OUT.obj X,Z RADIUS\n"
        "       fh1crowd DISC TRACK validate\n"
        "  DISC   the extracted disc (its root or its media folder)\n"
        "  TRACK  a folder of media/tracks, such as colorado\n");
    return 2;
}

std::optional<std::pair<float, float>> parsePoint(std::string_view text)
{
    const std::size_t comma = text.find(',');
    if (comma == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string x(text.substr(0, comma));
    const std::string z(text.substr(comma + 1));
    char* end = nullptr;
    const float px = std::strtof(x.c_str(), &end);
    if (x.empty() || *end != '\0') {
        return std::nullopt;
    }
    const float pz = std::strtof(z.c_str(), &end);
    if (z.empty() || *end != '\0') {
        return std::nullopt;
    }
    return std::pair {px, pz};
}

std::optional<float> parseNumber(std::string_view text)
{
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (copy.empty() || *end != '\0' || !(value >= 0.0F)) {
        return std::nullopt;
    }
    return value;
}

const char* formatName(world::TextureSurface::Format format)
{
    switch (format) {
    case world::TextureSurface::Format::Dxt1:
        return "DXT1";
    case world::TextureSurface::Format::Dxt3:
        return "DXT3";
    case world::TextureSurface::Format::Dxt5:
        return "DXT5";
    case world::TextureSurface::Format::Bc5:
        return "BC5";
    case world::TextureSurface::Format::Rgba8:
        return "RGBA8";
    }
    return "?";
}

std::string className(const spectators::CrowdSet& crowd, int crowdClass)
{
    return crowdClass >= 0 ? crowd.classes()[static_cast<std::size_t>(crowdClass)].name : std::string("?");
}

int stats(const spectators::CrowdSet& crowd)
{
    const spectators::CrowdLoadReport& report = crowd.report();
    std::printf("people: %zu in %zu crowd sets (%d without people)\n", crowd.spectators().size(), crowd.groups().size(),
        report.skippedEmptySets);
    if (report.legacyFile) {
        std::printf(".crowd file: version %u, %u entries\n", report.legacyFile->version, report.legacyFile->entryCount);
    }
    std::size_t always = 0;
    std::size_t tagged = 0;
    std::size_t routed = 0;
    std::size_t alwaysPeople = 0;
    for (const spectators::CrowdGroup& group : crowd.groups()) {
        if (group.alwaysShown()) {
            ++always;
            alwaysPeople += group.count;
        }
        tagged += group.eventTag.empty() ? 0 : 1;
        routed += group.routes.empty() ? 0 : 1;
    }
    std::printf("sets: %zu always shown (%zu people), %zu with an event tag, %zu listing routes\n", always,
        alwaysPeople, tagged, routed);

    std::map<int, std::size_t> perClass;
    std::size_t seated = 0;
    std::size_t withModel = 0;
    std::size_t withClip = 0;
    for (const spectators::Spectator& person : crowd.spectators()) {
        ++perClass[person.crowdClass];
        seated += person.seated ? 1 : 0;
        withModel += person.model >= 0 ? 1 : 0;
        withClip += person.animation >= 0 ? 1 : 0;
    }
    std::printf("seated %zu, with a model %zu, with an idle clip %zu\n", seated, withModel, withClip);
    std::printf("classes:\n");
    for (const auto& [index, count] : perClass) {
        std::printf("  %3d %-20s %zu\n", index >= 0 ? crowd.classes()[static_cast<std::size_t>(index)].id : -1,
            className(crowd, index).c_str(), count);
    }

    std::printf("models: %zu\n", crowd.models().size());
    for (const spectators::SpectatorModel& model : crowd.models()) {
        std::printf("  %-8s %-5s %5zu vertices, triangles per level", model.name.c_str(),
            model.seated ? "sit" : "stand", model.mesh.vertexCount());
        for (std::size_t lod = 0; lod < model.mesh.lods.size(); ++lod) {
            std::printf(" %zu", model.mesh.triangleCount(lod));
        }
        std::printf(
            ", %.2f m, texture %s\n", static_cast<double>(model.height), crowd.textures()[model.texture].name.c_str());
    }
    std::printf("skeletons:\n");
    for (const spectators::CrowdSkeleton& skeleton : crowd.skeletons()) {
        std::printf("  %-17s %s, %zu bones, %zu clips\n", skeleton.name.c_str(),
            skeleton.seated ? "seated" : "standing", skeleton.skeleton.bones.size(), skeleton.animations.size());
    }
    std::printf("clips: %zu\n", crowd.animations().size());
    for (const spectators::SpectatorAnimation& animation : crowd.animations()) {
        const spectators::AnimationClip& clip = animation.clip;
        std::printf("  %-18s %2zu bones %4d frames %6.2f s %2d animated %2d constant channels\n",
            animation.name.c_str(), clip.boneCount(), clip.frameCount, static_cast<double>(clip.duration),
            clip.animatedChannels, clip.constantChannels);
    }
    std::printf("textures: %zu\n", crowd.textures().size());
    for (const spectators::SpectatorTexture& texture : crowd.textures()) {
        std::printf("  %-10s %s %dx%d, %zu levels\n", texture.name.c_str(), formatName(texture.chain.format),
            texture.chain.width, texture.chain.height, texture.chain.levels.size());
    }
    const spectators::SpriteMetrics& sprites = crowd.spriteMetrics();
    std::printf("sprites: %d cells of 64x128, %.2f x %.2f m each, ground at row %.1f\n",
        crowd.spriteLayout().cellCount(), static_cast<double>(sprites.cellWidthMetres()),
        static_cast<double>(sprites.cellHeightMetres()), static_cast<double>(sprites.groundRow));
    std::printf("parse failures: %zu\n", report.failures.size());
    for (const std::string& failure : report.failures) {
        std::printf("  %s\n", failure.c_str());
    }
    for (const std::string& warning : report.warnings) {
        std::printf("  warning: %s\n", warning.c_str());
    }
    return 0;
}

int listNear(const spectators::CrowdSet& crowd, float x, float z, float radius)
{
    const std::vector<std::uint32_t> near = crowd.spectatorsNear(x, z, radius);
    std::printf("%zu people within %.0f m of %.1f, %.1f\n", near.size(), static_cast<double>(radius),
        static_cast<double>(x), static_cast<double>(z));
    for (const std::uint32_t i : near) {
        const spectators::Spectator& person = crowd.spectators()[i];
        const spectators::CrowdGroup& group = crowd.groups()[person.group];
        const std::string model
            = person.model >= 0 ? crowd.models()[static_cast<std::size_t>(person.model)].name : std::string("-");
        const std::string clip = person.animation >= 0
            ? crowd.animations()[static_cast<std::size_t>(person.animation)].name
            : std::string("-");
        std::printf("%7u  %9.2f %7.2f %9.2f  heading %5.1f  %-18s %-8s %-16s phase %5.2f  %s%s%s\n", i,
            static_cast<double>(person.position.x), static_cast<double>(person.position.y),
            static_cast<double>(person.position.z),
            static_cast<double>(person.heading * 180.0F / std::numbers::pi_v<float>),
            className(crowd, person.crowdClass).c_str(), model.c_str(), clip.c_str(), static_cast<double>(person.phase),
            group.name.c_str(), group.eventTag.empty() ? "" : " ", group.eventTag.c_str());
    }
    return 0;
}

bool writePng(const world::TextureMipChain& chain, const std::filesystem::path& path)
{
    const world::RgbaImage image = world::decodeSurface(chain.level(0));
    xds::Image out;
    out.width = static_cast<std::uint32_t>(image.width);
    out.height = static_cast<std::uint32_t>(image.height);
    out.rgba = image.rgba;
    const std::vector<std::uint8_t> png = xds::writePng(out);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(file);
}

int exportObj(const spectators::CrowdSet& crowd, const std::filesystem::path& objPath, float x, float z, float radius)
{
    const std::vector<std::uint32_t> near = crowd.spectatorsNear(x, z, radius);
    std::ofstream obj(objPath);
    const std::filesystem::path mtlPath = std::filesystem::path(objPath).replace_extension(".mtl");
    std::ofstream mtl(mtlPath);
    if (!obj || !mtl) {
        std::fprintf(stderr, "cannot write %s\n", objPath.string().c_str());
        return 1;
    }
    // OBJ files are right-handed; the world is left-handed, so Z is negated
    // on the way out, which also turns the clockwise fronts anticlockwise.
    obj << "# " << near.size() << " spectators around " << x << ", " << z << "; OBJ Z = -world Z\n";
    obj << "mtllib " << mtlPath.filename().string() << "\n";
    std::set<std::uint32_t> usedTextures;
    std::size_t vertexBase = 1;
    std::size_t skipped = 0;
    for (const std::uint32_t i : near) {
        const spectators::Spectator& person = crowd.spectators()[i];
        if (person.model < 0) {
            ++skipped;
            continue;
        }
        const auto modelIndex = static_cast<std::uint32_t>(person.model);
        const spectators::SpectatorModel& model = crowd.models()[modelIndex];
        std::vector<glm::vec3> positions;
        if (person.animation >= 0) {
            positions = crowd.skinnedPositions(modelIndex, static_cast<std::uint32_t>(person.animation), person.phase);
        } else {
            for (std::size_t v = 0; v < model.mesh.vertexCount(); ++v) {
                const float* f = &model.mesh.vertices[v * spectators::SpectatorMesh::kFloatsPerVertex];
                positions.emplace_back(f[0], f[1], f[2]);
            }
        }
        const glm::mat4 transform = person.transform();
        obj << "o spectator_" << i << "\n";
        for (std::size_t v = 0; v < positions.size(); ++v) {
            const glm::vec4 p = transform * glm::vec4(positions[v], 1.0F);
            const float* f = &model.mesh.vertices[v * spectators::SpectatorMesh::kFloatsPerVertex];
            const glm::vec4 n = transform
                * glm::vec4(f[spectators::SpectatorMesh::kNormal], f[spectators::SpectatorMesh::kNormal + 1],
                    f[spectators::SpectatorMesh::kNormal + 2], 0.0F);
            obj << "v " << p.x << ' ' << p.y << ' ' << -p.z << "\n";
            obj << "vt " << f[spectators::SpectatorMesh::kTexcoord] << ' '
                << 1.0F - f[spectators::SpectatorMesh::kTexcoord + 1] << "\n";
            obj << "vn " << n.x << ' ' << n.y << ' ' << -n.z << "\n";
        }
        const spectators::SpectatorMesh::Batch& batch = model.mesh.lods.front();
        usedTextures.insert(batch.texture);
        obj << "usemtl tex" << batch.texture << "\n";
        for (std::uint32_t k = 0; k < batch.indexCount; k += 3) {
            obj << 'f';
            for (std::uint32_t c = 0; c < 3; ++c) {
                const std::size_t index = vertexBase + model.mesh.indices[batch.firstIndex + k + c];
                obj << ' ' << index << '/' << index << '/' << index;
            }
            obj << "\n";
        }
        vertexBase += positions.size();
    }
    for (const std::uint32_t texture : usedTextures) {
        const std::string png = std::format("{}_tex{}.png", objPath.stem().string(), texture);
        if (!writePng(crowd.textures()[texture].chain, objPath.parent_path() / png)) {
            std::fprintf(stderr, "cannot write %s\n", png.c_str());
            return 1;
        }
        mtl << "newmtl tex" << texture << "\nKd 1 1 1\nmap_Kd " << png << "\n";
    }
    std::printf("wrote %zu spectators (%zu without a model skipped), %zu vertices, %zu textures to %s\n",
        near.size() - skipped, skipped, vertexBase - 1, usedTextures.size(), objPath.string().c_str());
    return 0;
}

/// Triangles of the world's finest geometry near the places checked, by
/// what they are.
class WorldSurfaces {
public:
    enum class Kind : std::uint8_t { Surface, CrowdArea };

    struct Triangle {
        glm::vec3 a;
        glm::vec3 b;
        glm::vec3 c;
        Kind kind;
        /// Index into names(): the part and material.
        std::uint32_t name;
    };

    void build(const world::TrackWorld& track, const std::vector<std::pair<glm::vec2, float>>& areas)
    {
        const std::vector<world::WorldChunk>& chunks = track.index.chunks();
        const auto overlaps = [&](const world::WorldChunk& chunk) {
            for (const auto& [centre, radius] : areas) {
                if (chunk.boundsMax.x >= centre.x - radius && chunk.boundsMin.x <= centre.x + radius
                    && chunk.boundsMax.z >= centre.y - radius && chunk.boundsMin.z <= centre.y + radius) {
                    return true;
                }
            }
            return false;
        };
        for (const world::WorldChunk& chunk : chunks) {
            if (chunk.backdrop || chunk.bandStart > 0.0F || !overlaps(chunk)) {
                continue;
            }
            const lzx::ZipEntry& entry = track.archive->entries()[chunk.entry];
            world::RenderMesh mesh;
            try {
                mesh = world::rendermesh::parse(world::readEntry(*track.archive, entry), entry.path());
            } catch (const std::exception&) {
                ++m_failedChunks;
                continue;
            }
            ++m_chunks;
            for (const world::RenderMesh::Part& part : mesh.parts) {
                for (const world::RenderMesh::Material& material : part.materials) {
                    Kind kind = Kind::Surface;
                    if (fh::iequals(material.name, "CrowdTERR")) {
                        kind = Kind::CrowdArea;
                        ++m_crowdAreaChunks;
                    } else if (world::isNotSurface(mesh, material)) {
                        continue;
                    }
                    const std::uint32_t name
                        = nameIndex(world::rendermesh::lodGroupKey(part.name) + " / " + material.name);
                    for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                        const auto at = [&](std::size_t k) {
                            const glm::vec3 p = part.positions[material.triangles[t + k]];
                            return chunk.placed ? chunk.placement.apply(p) : p;
                        };
                        add({at(0), at(1), at(2), kind, name});
                    }
                }
            }
        }
    }

    /// The highest surface under (x, z) no higher than `maxY`, and its name.
    std::optional<std::pair<float, std::uint32_t>> groundBelow(const glm::vec3& p, float maxY) const
    {
        std::optional<std::pair<float, std::uint32_t>> best;
        forEachAt(p.x, p.z, [&](const Triangle& t, float y) {
            if (t.kind != Kind::CrowdArea && y <= maxY && (!best || y > best->first)) {
                best = std::pair {y, t.name};
            }
        });
        return best;
    }

    const std::string& name(std::uint32_t index) const { return m_names[index]; }

    bool inside(const glm::vec3& p, Kind kind, float heightSlack) const
    {
        bool found = false;
        forEachAt(p.x, p.z,
            [&](const Triangle& t, float y) { found = found || (t.kind == kind && std::abs(y - p.y) <= heightSlack); });
        return found;
    }

    std::size_t triangles() const { return m_triangles.size(); }
    int chunks() const { return m_chunks; }
    int failedChunks() const { return m_failedChunks; }
    int crowdAreaMaterials() const { return m_crowdAreaChunks; }

private:
    static int cell(float v) { return static_cast<int>(std::floor(v / kGridCell)); }
    static std::uint64_t key(int x, int z)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
    }

    void add(const Triangle& t)
    {
        const auto index = static_cast<std::uint32_t>(m_triangles.size());
        m_triangles.push_back(t);
        const float minX = std::min({t.a.x, t.b.x, t.c.x});
        const float maxX = std::max({t.a.x, t.b.x, t.c.x});
        const float minZ = std::min({t.a.z, t.b.z, t.c.z});
        const float maxZ = std::max({t.a.z, t.b.z, t.c.z});
        for (int cx = cell(minX); cx <= cell(maxX); ++cx) {
            for (int cz = cell(minZ); cz <= cell(maxZ); ++cz) {
                m_grid[key(cx, cz)].push_back(index);
            }
        }
    }

    template <typename Visit> void forEachAt(float x, float z, Visit&& visit) const
    {
        const auto found = m_grid.find(key(cell(x), cell(z)));
        if (found == m_grid.end()) {
            return;
        }
        for (const std::uint32_t i : found->second) {
            const Triangle& t = m_triangles[i];
            const float d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
            if (std::abs(d) < 1e-9F) {
                continue;
            }
            const float u = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / d;
            const float v = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / d;
            const float w = 1.0F - u - v;
            if (u < 0.0F || v < 0.0F || w < 0.0F) {
                continue;
            }
            visit(t, u * t.a.y + v * t.b.y + w * t.c.y);
        }
    }

    std::uint32_t nameIndex(const std::string& name)
    {
        const auto [it, added] = m_nameIndex.emplace(name, static_cast<std::uint32_t>(m_names.size()));
        if (added) {
            m_names.push_back(name);
        }
        return it->second;
    }

    std::vector<Triangle> m_triangles;
    std::vector<std::string> m_names;
    std::unordered_map<std::string, std::uint32_t> m_nameIndex;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_grid;
    int m_chunks = 0;
    int m_failedChunks = 0;
    int m_crowdAreaChunks = 0;
};

/// The AI racing lines of the track's routes (`route_NNN.owt`, layout in
/// src/game/RaceData.h), as the reference for where the roads are: each
/// point carries the half-width of the drivable corridor.
class RacingLines {
public:
    void load(const fh::Vfs& vfs, std::string_view track)
    {
        const std::string folder = std::format("media/aiopenworld/{}/ribbon_00", fh::toLower(track));
        for (const std::string& path : vfs.list(folder)) {
            if (!fh::endsWithNoCase(path, ".owt")) {
                continue;
            }
            const std::optional<fh::Bytes> data = vfs.read(path);
            if (!data || data->size() < kOwtHeaderBytes || std::memcmp(data->data(), "OWTM", 4) != 0) {
                continue;
            }
            const std::uint32_t count = fh::loadBe32(data->data() + 16);
            if (kOwtHeaderBytes + static_cast<std::size_t>(count) * kOwtPointBytes > data->size()) {
                continue;
            }
            const auto first = static_cast<std::uint32_t>(m_points.size());
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint8_t* p = data->data() + kOwtHeaderBytes + static_cast<std::size_t>(i) * kOwtPointBytes;
                const float halfWidth = fh::loadBeF32(p + kOwtHalfWidthOffset);
                m_points.push_back({{fh::loadBeF32(p), fh::loadBeF32(p + 4), fh::loadBeF32(p + 8)},
                    std::isfinite(halfWidth) && halfWidth > 1.0F ? halfWidth : kDefaultHalfWidth, i + 1 < count});
            }
            for (std::uint32_t i = first; i < m_points.size(); ++i) {
                m_grid[key(cell(m_points[i].position.x), cell(m_points[i].position.z))].push_back(i);
            }
            ++m_lines;
        }
    }

    struct Nearest {
        float distance = 0.0F;
        float halfWidth = 0.0F;
        /// Unit horizontal direction from the person to the line.
        glm::vec2 toward {0.0F};
    };

    /// The nearest point of any line within kRoadSearch metres horizontally
    /// and kRoadHeightSlack vertically, on the segments between points.
    std::optional<Nearest> nearest(const glm::vec3& p) const
    {
        std::optional<Nearest> best;
        const glm::vec2 at(p.x, p.z);
        for (int cx = cell(p.x - kRoadSearch); cx <= cell(p.x + kRoadSearch); ++cx) {
            for (int cz = cell(p.z - kRoadSearch); cz <= cell(p.z + kRoadSearch); ++cz) {
                const auto found = m_grid.find(key(cx, cz));
                if (found == m_grid.end()) {
                    continue;
                }
                for (const std::uint32_t i : found->second) {
                    const Point& a = m_points[i];
                    const Point& b = a.continues ? m_points[i + 1] : a;
                    const glm::vec2 pa(a.position.x, a.position.z);
                    const glm::vec2 ab = glm::vec2(b.position.x, b.position.z) - pa;
                    const float length2 = glm::dot(ab, ab);
                    const float t = length2 > 0.0F ? std::clamp(glm::dot(at - pa, ab) / length2, 0.0F, 1.0F) : 0.0F;
                    const glm::vec2 closest = pa + ab * t;
                    const float height = a.position.y + (b.position.y - a.position.y) * t;
                    const float distance = glm::length(closest - at);
                    if (distance > kRoadSearch || std::abs(height - p.y) > kRoadHeightSlack
                        || (best && distance >= best->distance)) {
                        continue;
                    }
                    Nearest hit;
                    hit.distance = distance;
                    hit.halfWidth = a.halfWidth + (b.halfWidth - a.halfWidth) * t;
                    hit.toward = distance > 1e-3F ? (closest - at) / distance : glm::vec2(0.0F);
                    best = hit;
                }
            }
        }
        return best;
    }

    int lines() const { return m_lines; }

private:
    struct Point {
        glm::vec3 position;
        float halfWidth;
        /// False on a line's last point.
        bool continues;
    };

    static int cell(float v) { return static_cast<int>(std::floor(v / kRoadSearch)); }
    static std::uint64_t key(int x, int z)
    {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
    }

    std::vector<Point> m_points;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_grid;
    int m_lines = 0;
};

struct CheckResult {
    std::size_t people = 0;
    std::size_t withGround = 0;
    std::size_t within10cm = 0;
    std::size_t within30cm = 0;
    std::size_t within1m = 0;
    std::vector<float> offsets;
    std::size_t onRoad = 0;
    /// Least distance from a person to the edge of a racing corridor,
    /// negative inside it.
    float minRoadMargin = std::numeric_limits<float>::max();
    std::size_t inCrowdArea = 0;
    std::size_t nearRoad = 0;
    std::size_t facingRoad = 0;
    std::size_t facingAway = 0;
    double facingDot = 0.0;
    /// People by the surface they stand on.
    std::map<std::string, std::size_t> standingOn;
};

CheckResult check(const spectators::CrowdSet& crowd, const WorldSurfaces& surfaces, const RacingLines& lines,
    const std::vector<std::uint32_t>& people)
{
    CheckResult result;
    for (const std::uint32_t i : people) {
        const spectators::Spectator& person = crowd.spectators()[i];
        ++result.people;
        if (const auto ground = surfaces.groundBelow(person.position, person.position.y + kSurfaceSlack)) {
            ++result.withGround;
            ++result.standingOn[surfaces.name(ground->second)];
            const float offset = person.position.y - ground->first;
            result.offsets.push_back(offset);
            result.within10cm += std::abs(offset) <= 0.1F ? 1 : 0;
            result.within30cm += std::abs(offset) <= 0.3F ? 1 : 0;
            result.within1m += std::abs(offset) <= 1.0F ? 1 : 0;
        }
        result.inCrowdArea
            += surfaces.inside(person.position, WorldSurfaces::Kind::CrowdArea, kAreaHeightSlack) ? 1 : 0;
        const std::optional<RacingLines::Nearest> road = lines.nearest(person.position);
        if (!road) {
            continue;
        }
        result.onRoad += road->distance < road->halfWidth ? 1 : 0;
        result.minRoadMargin = std::min(result.minRoadMargin, road->distance - road->halfWidth);
        if (!person.seated && road->distance > 1e-3F) {
            ++result.nearRoad;
            const glm::vec3 facing = person.facing();
            const float dot = glm::dot(glm::vec2(facing.x, facing.z), road->toward);
            result.facingDot += dot;
            result.facingRoad += dot > 0.5F ? 1 : 0;
            result.facingAway += dot < -0.5F ? 1 : 0;
        }
    }
    return result;
}

void printCheck(const char* label, const CheckResult& r)
{
    const auto percent = [](std::size_t part, std::size_t whole) {
        return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
    };
    std::vector<float> sorted = r.offsets;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.empty() ? 0.0 : static_cast<double>(sorted[sorted.size() / 2]);
    std::printf("%s: %zu people\n", label, r.people);
    std::printf("  ground below: %zu (%.1f%%); height above it: median %.3f m, |dy| <= 0.1 m %.1f%%, <= 0.3 m "
                "%.1f%%, <= 1 m %.1f%%\n",
        r.withGround, percent(r.withGround, r.people), median, percent(r.within10cm, r.withGround),
        percent(r.within30cm, r.withGround), percent(r.within1m, r.withGround));
    std::printf("  inside a racing line's corridor: %zu (%.1f%%), closest to a corridor edge %.2f m; over a "
                "CrowdTERR area: %zu (%.1f%%)\n",
        r.onRoad, percent(r.onRoad, r.people), static_cast<double>(r.minRoadMargin), r.inCrowdArea,
        percent(r.inCrowdArea, r.people));
    std::printf("  standing people with a racing line within %.0f m: %zu; facing it (cos > 0.5) %.1f%%, facing "
                "away %.1f%%, mean cos %.2f\n",
        static_cast<double>(kRoadSearch), r.nearRoad, percent(r.facingRoad, r.nearRoad),
        percent(r.facingAway, r.nearRoad), r.nearRoad > 0 ? r.facingDot / static_cast<double>(r.nearRoad) : 0.0);
    std::vector<std::pair<std::size_t, std::string>> surfaces;
    surfaces.reserve(r.standingOn.size());
    for (const auto& [name, count] : r.standingOn) {
        surfaces.emplace_back(count, name);
    }
    std::sort(surfaces.rbegin(), surfaces.rend());
    std::printf("  surfaces stood on most:\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(surfaces.size(), 12); ++i) {
        std::printf("    %6zu  %s\n", surfaces[i].first, surfaces[i].second.c_str());
    }
}

std::optional<glm::vec3> gameObjectPosition(const fh::Vfs& vfs, std::string_view track, std::string_view id)
{
    const std::optional<std::string> text
        = vfs.readText(std::format("media/tracks/{}/ribbon_00/gameobjs.xml", fh::toLower(track)));
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    for (const pugi::xml_node node : doc.child("GameObjs").children()) {
        if (fh::iequals(node.attribute("GameplayID").as_string(), id)) {
            const pugi::xml_node pos = node.child("Pos");
            return glm::vec3(
                pos.attribute("x").as_float(), pos.attribute("y").as_float(), pos.attribute("z").as_float());
        }
    }
    return std::nullopt;
}

std::optional<glm::vec3> routeStart(const fh::Vfs& vfs, std::string_view track, int route)
{
    const std::optional<std::string> text
        = vfs.readText(std::format("media/tracks/{}/ribbon_00/trackroute{:03}.xml", fh::toLower(track), route));
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    for (const pugi::xml_node node : doc.child("TrackRoute").child("NamedTransforms").children("NamedTransform")) {
        if (std::string_view(node.attribute("name").as_string()) == "start_location_00") {
            const pugi::xml_node t = node.child("Transform");
            return glm::vec3(
                t.attribute("pos.x").as_float(), t.attribute("pos.y").as_float(), t.attribute("pos.z").as_float());
        }
    }
    return std::nullopt;
}

int validate(const fh::Vfs& vfs, std::string_view track, const spectators::CrowdSet& crowd)
{
    std::vector<std::pair<glm::vec2, float>> areas;
    std::vector<std::uint32_t> festivalPeople;
    const std::optional<glm::vec3> festival = gameObjectPosition(vfs, track, "FESTIVAL_FIRST");
    if (festival) {
        std::printf("FESTIVAL_FIRST at %.1f, %.1f, %.1f\n", static_cast<double>(festival->x),
            static_cast<double>(festival->y), static_cast<double>(festival->z));
        areas.emplace_back(glm::vec2(festival->x, festival->z), kFestivalRadius);
        festivalPeople = crowd.spectatorsNear(festival->x, festival->z, kFestivalRadius);
    } else {
        std::printf("no FESTIVAL_FIRST object\n");
    }

    std::map<int, std::vector<std::uint32_t>> routeGroups;
    for (std::size_t g = 0; g < crowd.groups().size(); ++g) {
        for (const std::uint8_t route : crowd.groups()[g].routes) {
            if (route > 0) {
                routeGroups[route].push_back(static_cast<std::uint32_t>(g));
            }
        }
    }
    std::vector<std::uint32_t> startPeople;
    int startsWithCrowd = 0;
    int routesChecked = 0;
    for (const auto& [route, groups] : routeGroups) {
        const std::optional<glm::vec3> start = routeStart(vfs, track, route);
        if (!start) {
            continue;
        }
        ++routesChecked;
        std::set<std::uint32_t> routeGroupSet(groups.begin(), groups.end());
        int here = 0;
        for (const std::uint32_t i : crowd.spectatorsNear(start->x, start->z, kStartRadius)) {
            if (routeGroupSet.count(crowd.spectators()[i].group) > 0) {
                startPeople.push_back(i);
                ++here;
            }
        }
        startsWithCrowd += here > 0 ? 1 : 0;
        areas.emplace_back(glm::vec2(start->x, start->z), kStartRadius);
    }
    std::sort(startPeople.begin(), startPeople.end());
    startPeople.erase(std::unique(startPeople.begin(), startPeople.end()), startPeople.end());
    std::printf("routes listed by crowd sets: %zu, with a route file %d, with the route's crowd within %.0f m of "
                "the start %d\n",
        routeGroups.size(), routesChecked, static_cast<double>(kStartRadius), startsWithCrowd);

    const std::optional<world::TrackWorld> loaded = world::TrackWorld::load(vfs, track);
    if (!loaded) {
        std::fprintf(stderr, "the world could not be loaded\n");
        return 1;
    }
    WorldSurfaces surfaces;
    surfaces.build(*loaded, areas);
    RacingLines lines;
    lines.load(vfs, track);
    std::printf("world: %d chunks (%d unreadable), %zu triangles, %d CrowdTERR materials around the checked areas; "
                "%d racing lines\n",
        surfaces.chunks(), surfaces.failedChunks(), surfaces.triangles(), surfaces.crowdAreaMaterials(), lines.lines());
    printCheck(std::format("festival (within {:.0f} m of FESTIVAL_FIRST)", kFestivalRadius).c_str(),
        check(crowd, surfaces, lines, festivalPeople));
    printCheck(std::format("race starts (route crowds within {:.0f} m of start_location_00)", kStartRadius).c_str(),
        check(crowd, surfaces, lines, startPeople));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        return usage();
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string track = argv[2];
        const std::string command = argv[3];
        if (command == "--obj" && argc != 7) {
            return usage();
        }
        const spectators::CrowdSet crowd = spectators::CrowdSet::load(vfs, track);
        if (command == "stats" && argc == 4) {
            return stats(crowd);
        }
        if (command == "validate" && argc == 4) {
            return validate(vfs, track, crowd);
        }
        if (command == "at" && argc == 6) {
            const std::optional<std::pair<float, float>> point = parsePoint(argv[4]);
            const std::optional<float> radius = parseNumber(argv[5]);
            if (!point || !radius) {
                return usage();
            }
            return listNear(crowd, point->first, point->second, *radius);
        }
        if (command == "--obj") {
            const std::optional<std::pair<float, float>> point = parsePoint(argv[5]);
            const std::optional<float> radius = parseNumber(argv[6]);
            if (!point || !radius) {
                return usage();
            }
            return exportObj(crowd, argv[4], point->first, point->second, *radius);
        }
        return usage();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
