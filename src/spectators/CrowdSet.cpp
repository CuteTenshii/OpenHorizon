#include "CrowdSet.h"

#include "base/Log.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"
#include "world/TrackWorld.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <lzx/zip.h>
#include <xds/xds.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>

namespace fh::spectators {

namespace {

constexpr std::string_view kRoot = "media/spectators";
constexpr float kCellSize = 32.0F;
/// Metres a second; the data gives no speed, so this is a brisk walk.
constexpr float kWalkingSpeed = 1.4F;
constexpr std::uint64_t kModelSalt = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t kAnimationSalt = 0xC2B2AE3D27D4EB4FULL;
constexpr std::uint64_t kPhaseSalt = 0x165667B19E3779F9ULL;
constexpr std::uint64_t kDensitySalt = 0x27D4EB2F165667C5ULL;
constexpr std::size_t kDdsHeaderBytes = 128;
constexpr std::size_t kDdsHeightOffset = 0x0C;
constexpr std::size_t kDdsWidthOffset = 0x10;
constexpr std::size_t kDdsFourCcOffset = 0x54;

std::uint64_t mix(std::uint64_t value)
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

/// A repeatable number in [0, 1) for person `key`.
double unitHash(std::uint64_t key, std::uint64_t salt)
{
    return static_cast<double>(mix(key ^ salt) >> 11) / static_cast<double>(1ULL << 53);
}

std::uint64_t cellKey(int x, int z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

int cellOf(float value)
{
    return static_cast<int>(std::floor(value / kCellSize));
}

bool isSeatedSuffix(std::string_view suffix)
{
    return iequals(suffix, "_sit");
}

/// Picks from weighted choices with `unit` in [0, 1).
int pickWeighted(const std::vector<WeightedChoice>& choices, double unit)
{
    double total = 0.0;
    for (const WeightedChoice& choice : choices) {
        total += std::max(choice.weight, 0.0F);
    }
    if (choices.empty()) {
        return -1;
    }
    if (!(total > 0.0)) {
        return static_cast<int>(choices[static_cast<std::size_t>(unit * static_cast<double>(choices.size()))].index);
    }
    double target = unit * total;
    for (const WeightedChoice& choice : choices) {
        target -= std::max(choice.weight, 0.0F);
        if (target < 0.0) {
            return static_cast<int>(choice.index);
        }
    }
    return static_cast<int>(choices.back().index);
}

world::TextureSurface::Format surfaceFormat(std::uint32_t format, bool& supported)
{
    supported = true;
    switch (static_cast<xds::Format>(format)) {
    case xds::Format::kDxt1:
    case xds::Format::kDxt1_As16_16_16_16:
        return world::TextureSurface::Format::Dxt1;
    case xds::Format::kDxt2_3:
    case xds::Format::kDxt2_3_As16_16_16_16:
        return world::TextureSurface::Format::Dxt3;
    case xds::Format::kDxt4_5:
    case xds::Format::kDxt4_5_As16_16_16_16:
        return world::TextureSurface::Format::Dxt5;
    case xds::Format::kDxn:
        return world::TextureSurface::Format::Bc5;
    default:
        supported = false;
        return world::TextureSurface::Format::Rgba8;
    }
}

int fullLevelCount(int width, int height)
{
    return static_cast<int>(std::bit_width(static_cast<unsigned>(std::max(std::max(width, height), 1))));
}

glm::mat4 mirrorX()
{
    glm::mat4 m(1.0F);
    m[0][0] = -1.0F;
    return m;
}

} // namespace

world::TextureMipChain loadXdsMipChain(ByteView data, const std::string& source)
{
    try {
        const xds::Texture texture = xds::Texture::parse(data);
        const xds::TextureInfo& info = texture.info();
        bool blocks = false;
        world::TextureSurface::Format format = surfaceFormat(info.format, blocks);
        const bool identity = info.swizzle
            == std::array<xds::Swizzle, 4> {xds::Swizzle::X, xds::Swizzle::Y, xds::Swizzle::Z, xds::Swizzle::W};
        blocks = blocks && identity;
        if (!blocks) {
            format = world::TextureSurface::Format::Rgba8;
        }
        const auto levelOf = [&](std::uint32_t level) {
            world::TextureSurface surface;
            surface.format = format;
            if (blocks) {
                xds::Surface raw = texture.extractLevel(level);
                surface.width = static_cast<int>(raw.width);
                surface.height = static_cast<int>(raw.height);
                surface.data = std::move(raw.data);
            } else {
                xds::Image image = texture.decodeLevel(level);
                surface.width = static_cast<int>(image.width);
                surface.height = static_cast<int>(image.height);
                surface.data = std::move(image.rgba);
            }
            if (surface.data.size() < surface.byteSize()) {
                throw DataError("{}: level {} is truncated", source, level);
            }
            surface.data.resize(surface.byteSize());
            return surface;
        };
        const world::TextureSurface top = levelOf(0);
        world::TextureMipChain chain;
        const int wanted = fullLevelCount(top.width, top.height);
        if (static_cast<int>(texture.levelCount()) >= wanted) {
            chain.format = format;
            chain.width = top.width;
            chain.height = top.height;
            chain.levels.push_back(top.data);
            for (int level = 1; level < wanted; ++level) {
                chain.levels.push_back(levelOf(static_cast<std::uint32_t>(level)).data);
            }
        } else {
            chain = world::buildMipChain(top);
        }
        chain.files = source;
        return chain;
    } catch (const xds::Error& e) {
        throw DataError("{}: {}", source, e.what());
    }
}

world::TextureMipChain loadDdsMipChain(ByteView data, const std::string& source)
{
    if (data.size() < kDdsHeaderBytes || std::memcmp(data.data(), "DDS ", 4) != 0) {
        throw DataError("{}: not a DDS file", source);
    }
    world::TextureSurface top;
    top.height = static_cast<int>(loadLe32(data.data() + kDdsHeightOffset));
    top.width = static_cast<int>(loadLe32(data.data() + kDdsWidthOffset));
    const std::string_view fourCc(reinterpret_cast<const char*>(data.data() + kDdsFourCcOffset), 4);
    if (fourCc == "DXT1") {
        top.format = world::TextureSurface::Format::Dxt1;
    } else if (fourCc == "DXT3") {
        top.format = world::TextureSurface::Format::Dxt3;
    } else if (fourCc == "DXT5") {
        top.format = world::TextureSurface::Format::Dxt5;
    } else {
        throw DataError("{}: unsupported DDS format", source);
    }
    if (top.width <= 0 || top.height <= 0 || top.byteSize() > data.size() - kDdsHeaderBytes) {
        throw DataError("{}: the image does not fit the file", source);
    }
    const auto first = data.begin() + static_cast<std::ptrdiff_t>(kDdsHeaderBytes);
    top.data.assign(first, first + static_cast<std::ptrdiff_t>(top.byteSize()));
    world::TextureMipChain chain = world::buildMipChain(top);
    chain.files = source;
    return chain;
}

SpectatorMesh buildSpectatorMesh(const SkinnedModel& model, std::uint32_t texture)
{
    SpectatorMesh mesh;
    mesh.vertices.reserve(model.vertices.size() * SpectatorMesh::kFloatsPerVertex);
    const float white = std::bit_cast<float>(0xFFFFFFFFU);
    for (const SkinnedModel::Vertex& v : model.vertices) {
        const float values[SpectatorMesh::kFloatsPerVertex]
            = {-v.position.x, v.position.y, v.position.z, -v.normal.x, v.normal.y, v.normal.z, v.texcoord.x,
                v.texcoord.y, white, static_cast<float>(v.bones[0]), static_cast<float>(v.bones[1]),
                static_cast<float>(v.weights[0]) / 255.0F, static_cast<float>(v.weights[1]) / 255.0F};
        mesh.vertices.insert(mesh.vertices.end(), std::begin(values), std::end(values));
    }

    // Mirroring reverses the winding; decide the order from the normals
    // rather than assume the files' convention, and keep it for every level.
    double facing = 0.0;
    const auto position = [&](std::uint16_t i) {
        const SkinnedModel::Vertex& v = model.vertices[i];
        return glm::vec3(-v.position.x, v.position.y, v.position.z);
    };
    for (std::size_t i = 0; i + 2 < model.indices.size(); i += 3) {
        const std::uint16_t a = model.indices[i];
        const std::uint16_t b = model.indices[i + 1];
        const std::uint16_t c = model.indices[i + 2];
        const glm::vec3 cross = glm::cross(position(b) - position(a), position(c) - position(a));
        const glm::vec3 n = model.vertices[a].normal + model.vertices[b].normal + model.vertices[c].normal;
        facing += glm::dot(cross, glm::vec3(-n.x, n.y, n.z));
    }
    const bool swap = facing < 0.0;

    mesh.indices.reserve(model.indices.size());
    for (const SkinnedModel::Lod& lod : model.lods) {
        SpectatorMesh::Batch batch;
        batch.texture = texture;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        for (std::uint32_t i = 0; i + 2 < lod.indexCount; i += 3) {
            const std::size_t at = lod.firstIndex + i;
            mesh.indices.push_back(model.indices[at]);
            mesh.indices.push_back(model.indices[at + (swap ? 2 : 1)]);
            mesh.indices.push_back(model.indices[at + (swap ? 1 : 2)]);
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        mesh.lods.push_back(batch);
    }
    return mesh;
}

void mirrorToSpectatorSpace(std::span<glm::mat4> matrices)
{
    const glm::mat4 m = mirrorX();
    for (glm::mat4& matrix : matrices) {
        matrix = m * matrix * m;
    }
}

bool CrowdGroup::belongsToEvent(std::string_view horizonEventId) const
{
    return !eventId.empty() && iequals(eventId, horizonEventId);
}

bool CrowdGroup::belongsToRace(std::string_view horizonEventId, int routeId) const
{
    if (belongsToEvent(horizonEventId)) {
        return true;
    }
    return routeId > 0 && std::find(routes.begin(), routes.end(), routeId) != routes.end();
}

bool CrowdGroup::listsFreeRoam() const
{
    return std::find(routes.begin(), routes.end(), std::uint8_t {0}) != routes.end();
}

glm::vec3 Spectator::facing() const
{
    return {std::sin(heading), 0.0F, std::cos(heading)};
}

glm::mat4 Spectator::transform() const
{
    const float s = std::sin(heading);
    const float c = std::cos(heading);
    glm::mat4 m(1.0F);
    m[0] = glm::vec4(c, 0.0F, -s, 0.0F);
    m[2] = glm::vec4(s, 0.0F, c, 0.0F);
    m[3] = glm::vec4(position, 1.0F);
    return m;
}

CrowdSession CrowdSession::freeRoam(std::vector<std::string> offeredEvents)
{
    CrowdSession session;
    session.offeredEvents = std::move(offeredEvents);
    return session;
}

CrowdSession CrowdSession::raceOn(std::string eventId, int routeId)
{
    CrowdSession session;
    session.race = true;
    session.eventId = std::move(eventId);
    session.routeId = routeId;
    return session;
}

bool CrowdSession::shows(const CrowdGroup& group) const
{
    if (group.alwaysShown()) {
        return true;
    }
    if (race) {
        return group.belongsToRace(eventId, routeId);
    }
    if (!group.listsFreeRoam()) {
        return false;
    }
    if (offeredEvents.empty() || group.eventId.empty()) {
        return true;
    }
    return std::any_of(
        offeredEvents.begin(), offeredEvents.end(), [&](const std::string& id) { return group.belongsToEvent(id); });
}

CrowdSet CrowdSet::load(const Vfs& vfs, std::string_view track)
{
    CrowdSet set;
    const std::string configPath = std::format("{}/spectators.xml", kRoot);
    const std::optional<std::string> text = vfs.readText(configPath);
    if (!text) {
        throw DataError("{} is missing", configPath);
    }
    set.m_config = SpectatorConfig::parse(*text, configPath);
    set.loadAnimations(vfs);
    set.loadModels(vfs);
    set.loadSprites(vfs);
    set.resolveClasses();
    set.loadPlacements(vfs, track);
    for (const std::string& failure : set.m_report.failures) {
        logWarning("crowd: {}", failure);
    }
    logInfo("crowd: {} people in {} sets, {} models, {} clips, {} textures", set.m_spectators.size(),
        set.m_groups.size(), set.m_models.size(), set.m_animations.size(), set.m_textures.size());
    return set;
}

void CrowdSet::loadAnimations(const Vfs& vfs)
{
    for (const SpectatorConfig::Skeleton& definition : m_config.skeletons) {
        CrowdSkeleton skeleton;
        skeleton.name = definition.name;
        skeleton.suffix = definition.suffix;
        skeleton.seated = isSeatedSuffix(definition.suffix);
        for (const SpectatorConfig::Animation& animation : definition.animations) {
            int index = findAnimation(animation.name);
            if (index < 0) {
                const std::string path = std::format("{}/anims/{}.anim.bin", kRoot, toLower(animation.file));
                const std::optional<Bytes> data = vfs.read(path);
                if (!data) {
                    m_report.failures.push_back(std::format("{} is missing", path));
                    continue;
                }
                try {
                    SpectatorAnimation loaded {animation.name, path, parseAnimation(*data, path)};
                    loaded.clip.name = animation.name;
                    m_animations.push_back(std::move(loaded));
                    index = static_cast<int>(m_animations.size() - 1);
                } catch (const DataError& e) {
                    m_report.failures.emplace_back(e.what());
                    continue;
                }
            }
            const Skeleton& bones = m_animations[static_cast<std::size_t>(index)].clip.skeleton;
            if (skeleton.skeleton.bones.empty()) {
                skeleton.skeleton = bones;
            } else if (!skeleton.skeleton.sameHierarchy(bones)) {
                m_report.failures.push_back(
                    std::format("clip {} does not share the bones of skeleton {}", animation.name, definition.name));
                continue;
            }
            skeleton.animations.push_back(static_cast<std::uint32_t>(index));
        }
        m_skeletons.push_back(std::move(skeleton));
    }
}

std::uint32_t CrowdSet::addTexture(const Vfs& vfs, const std::string& name, const std::string& path, bool xds)
{
    if (const auto found = m_textureByPath.find(path); found != m_textureByPath.end()) {
        return found->second;
    }
    const std::optional<Bytes> data = vfs.read(path);
    if (!data) {
        throw DataError("{} is missing", path);
    }
    SpectatorTexture texture {name, path, xds ? loadXdsMipChain(*data, path) : loadDdsMipChain(*data, path)};
    m_textures.push_back(std::move(texture));
    const auto index = static_cast<std::uint32_t>(m_textures.size() - 1);
    m_textureByPath.emplace(path, index);
    return index;
}

void CrowdSet::loadModels(const Vfs& vfs)
{
    std::vector<std::string> suffixes;
    for (const CrowdSkeleton& skeleton : m_skeletons) {
        if (std::none_of(
                suffixes.begin(), suffixes.end(), [&](const std::string& s) { return iequals(s, skeleton.suffix); })) {
            suffixes.push_back(skeleton.suffix);
        }
    }
    for (std::size_t m = 0; m < m_config.models.size(); ++m) {
        const SpectatorConfig::Model& definition = m_config.models[m];
        std::optional<std::uint32_t> texture;
        const std::string texturePath = std::format("{}/textures/{}.dds.xds", kRoot, toLower(definition.texture));
        try {
            texture = addTexture(vfs, definition.texture, texturePath, true);
        } catch (const DataError& e) {
            m_report.failures.emplace_back(e.what());
        }
        for (const std::string& suffix : suffixes) {
            const std::string path
                = std::format("{}/models/{}{}.skinbin", kRoot, toLower(definition.name), toLower(suffix));
            const std::optional<Bytes> data = vfs.read(path);
            if (!data) {
                m_report.failures.push_back(std::format("{} is missing", path));
                continue;
            }
            try {
                SpectatorModel model;
                model.name = definition.name;
                model.configModel = static_cast<int>(m);
                model.path = path;
                model.source = parseSkinnedModel(*data, path);
                model.seated = isSeatedSuffix(suffix);
                for (std::size_t s = 0; s < m_skeletons.size(); ++s) {
                    if (iequals(m_skeletons[s].suffix, suffix) && !m_skeletons[s].skeleton.bones.empty()) {
                        model.skeleton = static_cast<std::uint32_t>(s);
                        break;
                    }
                }
                const std::size_t bones = m_skeletons.empty() ? 0 : m_skeletons[model.skeleton].skeleton.bones.size();
                if (static_cast<std::size_t>(model.source.boneCount()) > bones) {
                    throw DataError("{}: uses {} bones, its skeleton has {}", path, model.source.boneCount(), bones);
                }
                model.height = model.source.height();
                model.texture = texture.value_or(0);
                if (!texture) {
                    m_report.warnings.push_back(std::format("{} has no texture", path));
                }
                model.mesh = buildSpectatorMesh(model.source, model.texture);
                m_models.push_back(std::move(model));
            } catch (const DataError& e) {
                m_report.failures.emplace_back(e.what());
            }
        }
    }
}

void CrowdSet::loadSprites(const Vfs& vfs)
{
    const std::string spritePath = std::format("{}/sprites.xds", kRoot);
    try {
        m_spriteTexture = static_cast<int>(addTexture(vfs, "sprites", spritePath, true));
        const world::TextureMipChain& chain = m_textures[static_cast<std::size_t>(m_spriteTexture)].chain;
        m_spriteLayout = {chain.width, chain.height};
        std::vector<float> heights(m_config.models.size(), 0.0F);
        for (const SpectatorModel& model : m_models) {
            if (!model.seated) {
                heights[static_cast<std::size_t>(model.configModel)] = model.height;
            }
        }
        m_spriteMetrics = measureSprites(world::decodeSurface(chain.level(0)), heights);
        const int needed = static_cast<int>(m_config.models.size()) * SpriteAtlasLayout::kCellsPerModel;
        if (m_spriteLayout.cellCount() < needed) {
            m_report.warnings.push_back(std::format("the sprite atlas has {} cells, {} models need {}",
                m_spriteLayout.cellCount(), m_config.models.size(), needed));
        }
    } catch (const DataError& e) {
        m_report.failures.emplace_back(e.what());
    }
    try {
        m_shadowTexture = static_cast<int>(addTexture(vfs, "shadow", std::format("{}/shadow.dds", kRoot), false));
    } catch (const DataError& e) {
        m_report.failures.emplace_back(e.what());
    }
}

void CrowdSet::resolveClasses()
{
    const auto resolveAnimations = [&](const std::vector<SpectatorConfig::WeightedAnimation>& names,
                                       const std::string& className) {
        std::vector<WeightedChoice> out;
        for (const SpectatorConfig::WeightedAnimation& animation : names) {
            const int index = findAnimation(animation.name);
            if (index < 0) {
                m_report.failures.push_back(
                    std::format("crowd class {} names clip {}, which no skeleton loads", className, animation.name));
                continue;
            }
            out.push_back({static_cast<std::uint32_t>(index), animation.weight});
        }
        return out;
    };
    for (const SpectatorConfig::CrowdClass& definition : m_config.classes) {
        CrowdClass crowdClass;
        crowdClass.id = definition.id;
        crowdClass.name = definition.name;
        crowdClass.skeleton = m_config.findSkeleton(definition.skeleton);
        crowdClass.offset = definition.offset;
        crowdClass.billboardOffsetY = definition.billboardOffsetY;
        crowdClass.randomSize = definition.randomSize;
        if (crowdClass.skeleton < 0) {
            m_report.failures.push_back(std::format(
                "crowd class {} names skeleton {}, which is not defined", definition.name, definition.skeleton));
        } else {
            const CrowdSkeleton& skeleton = m_skeletons[static_cast<std::size_t>(crowdClass.skeleton)];
            crowdClass.seated = skeleton.seated;
            const int modelSet = m_config.findModelSet(definition.modelSet);
            if (modelSet < 0) {
                m_report.failures.push_back(std::format(
                    "crowd class {} names model set {}, which is not defined", definition.name, definition.modelSet));
            } else {
                for (const std::string& name : m_config.modelSets[static_cast<std::size_t>(modelSet)].models) {
                    for (std::size_t m = 0; m < m_models.size(); ++m) {
                        if (iequals(m_models[m].name, name) && m_models[m].seated == skeleton.seated) {
                            crowdClass.models.push_back(static_cast<std::uint32_t>(m));
                        }
                    }
                }
            }
        }
        crowdClass.idles = resolveAnimations(definition.idles, definition.name);
        crowdClass.cheers = resolveAnimations(definition.cheers, definition.name);
        m_classes.push_back(std::move(crowdClass));
    }
}

void CrowdSet::loadPlacements(const Vfs& vfs, std::string_view track)
{
    const world::TrackPaths paths = world::TrackPaths::of(track);
    const std::string legacyPath
        = std::format("media/tracks/{}/ribbon_00/{}_track_00.crowd", toLower(track), toLower(track));
    if (const std::optional<Bytes> legacy = vfs.read(legacyPath)) {
        try {
            m_report.legacyFile = parseLegacyCrowdFile(*legacy, legacyPath);
            if (m_report.legacyFile->entryCount > 0) {
                m_report.failures.push_back(
                    std::format("{} lists {} entries in a layout no disc file shows; they are not placed", legacyPath,
                        m_report.legacyFile->entryCount));
            }
        } catch (const DataError& e) {
            m_report.failures.emplace_back(e.what());
        }
    }

    const std::shared_ptr<const lzx::ZipArchive> archive = vfs.archiveAt(paths.archive);
    if (!archive) {
        throw DataError("track {} has no archive at {}", track, paths.archive);
    }
    const std::vector<std::size_t> entries = world::distinctEntries(*archive, ".pgeo");
    std::vector<std::optional<CrowdPlacementSet>> sets(entries.size());
    std::mutex failureMutex;
    world::parallelFor(entries.size(), [&](std::size_t i) {
        const lzx::ZipEntry& entry = archive->entries()[entries[i]];
        try {
            const Bytes prefix = world::readEntryPrefix(*archive, entry, kPlacementSetPrefixBytes);
            if (placementSetType(prefix) != kCrowdSetType) {
                return;
            }
            sets[i] = parseCrowdPlacementSet(world::readEntry(*archive, entry), entry.path());
        } catch (const std::exception& e) {
            const std::lock_guard lock(failureMutex);
            m_report.failures.push_back(std::format("{}: {}", entry.path(), e.what()));
        }
    });
    std::vector<CrowdPlacementSet> found;
    std::vector<std::string> files;
    for (std::size_t i = 0; i < sets.size(); ++i) {
        if (sets[i]) {
            files.push_back(archive->entries()[entries[i]].path());
            found.push_back(std::move(*sets[i]));
        }
    }
    placeSpectators(found, files);
}

void CrowdSet::placeSpectators(const std::vector<CrowdPlacementSet>& sets, const std::vector<std::string>& files)
{
    std::unordered_map<int, int> classById;
    for (std::size_t c = 0; c < m_classes.size(); ++c) {
        classById.emplace(m_classes[c].id, static_cast<int>(c));
    }
    const auto place = [&](Spectator spectator, std::uint8_t storedClass, std::uint64_t key) {
        const auto found = classById.find(storedClass + 1);
        if (found == classById.end()) {
            ++m_report.unknownClassInstances;
        } else {
            spectator.crowdClass = found->second;
            const CrowdClass& crowdClass = m_classes[static_cast<std::size_t>(found->second)];
            spectator.seated = crowdClass.seated;
            spectator.position += crowdClass.offset;
            if (!crowdClass.models.empty()) {
                const auto pick = static_cast<std::size_t>(
                    unitHash(key, kModelSalt) * static_cast<double>(crowdClass.models.size()));
                spectator.model = static_cast<int>(crowdClass.models[std::min(pick, crowdClass.models.size() - 1)]);
            }
            spectator.animation = pickWeighted(crowdClass.idles, unitHash(key, kAnimationSalt));
            if (spectator.animation >= 0) {
                spectator.phase = static_cast<float>(unitHash(key, kPhaseSalt))
                    * m_animations[static_cast<std::size_t>(spectator.animation)].clip.duration;
            }
        }
        m_grid[cellKey(cellOf(spectator.position.x), cellOf(spectator.position.z))].push_back(
            static_cast<std::uint32_t>(m_spectators.size()));
        m_spectators.push_back(spectator);
    };

    for (std::size_t s = 0; s < sets.size(); ++s) {
        const CrowdPlacementSet& placement = sets[s];
        ++m_report.placementSets;
        if (placement.spectators.empty() && placement.walkPaths.empty()) {
            ++m_report.skippedEmptySets;
        }
        CrowdGroup group;
        group.name = placement.name;
        group.file = files[s];
        group.hasEventBlock = placement.hasEventBlock;
        group.eventTag = placement.eventTag;
        group.eventId = placement.eventId();
        group.routes = placement.routes;
        group.boundsMin = placement.boundsMin;
        group.boundsMax = placement.boundsMax;
        group.first = static_cast<std::uint32_t>(m_spectators.size());
        const auto groupIndex = static_cast<std::uint32_t>(m_groups.size());
        std::uint64_t key = static_cast<std::uint64_t>(groupIndex) << 32;
        for (const PlacedSpectator& placed : placement.spectators) {
            Spectator spectator;
            spectator.position = placed.position;
            spectator.heading = placed.headingRadians();
            spectator.group = groupIndex;
            spectator.flags = placed.flags;
            spectator.extra = placed.extra;
            place(spectator, placed.crowdClass, key++);
        }
        for (const WalkPath& path : placement.walkPaths) {
            Spectator spectator;
            spectator.group = groupIndex;
            spectator.walkPath = static_cast<int>(m_walkPaths.size());
            spectator.walkStart = static_cast<float>(unitHash(key, kPhaseSalt)) * path.loopLength();
            glm::vec3 direction(0.0F, 0.0F, 1.0F);
            spectator.position = path.pointAt(spectator.walkStart, &direction);
            spectator.heading = std::atan2(direction.x, direction.z);
            m_walkPaths.push_back({groupIndex, path});
            place(spectator, path.crowdClass, key++);
        }
        group.count = static_cast<std::uint32_t>(m_spectators.size()) - group.first;
        m_groups.push_back(std::move(group));
    }
    if (m_report.unknownClassInstances > 0) {
        m_report.failures.push_back(
            std::format("{} people name crowd classes spectators.xml lacks", m_report.unknownClassInstances));
    }
}

glm::vec3 CrowdSet::walkerAt(std::uint32_t spectator, float time, float& heading) const
{
    const Spectator& person = m_spectators.at(spectator);
    heading = person.heading;
    if (person.walkPath < 0) {
        return person.position;
    }
    const WalkPath& path = m_walkPaths[static_cast<std::size_t>(person.walkPath)].path;
    glm::vec3 direction(0.0F, 0.0F, 1.0F);
    glm::vec3 position = path.pointAt(person.walkStart + time * kWalkingSpeed, &direction);
    heading = std::atan2(direction.x, direction.z);
    if (person.crowdClass >= 0) {
        position += m_classes[static_cast<std::size_t>(person.crowdClass)].offset;
    }
    return position;
}

int CrowdSet::findAnimation(std::string_view name) const
{
    for (std::size_t i = 0; i < m_animations.size(); ++i) {
        if (iequals(m_animations[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::uint32_t> CrowdSet::spectatorsNear(float x, float z, float radius) const
{
    std::vector<std::pair<float, std::uint32_t>> hits;
    const float r2 = radius * radius;
    for (int cx = cellOf(x - radius); cx <= cellOf(x + radius); ++cx) {
        for (int cz = cellOf(z - radius); cz <= cellOf(z + radius); ++cz) {
            const auto cell = m_grid.find(cellKey(cx, cz));
            if (cell == m_grid.end()) {
                continue;
            }
            for (const std::uint32_t i : cell->second) {
                const float dx = m_spectators[i].position.x - x;
                const float dz = m_spectators[i].position.z - z;
                const float d2 = dx * dx + dz * dz;
                if (d2 <= r2) {
                    hits.emplace_back(d2, i);
                }
            }
        }
    }
    std::sort(hits.begin(), hits.end());
    std::vector<std::uint32_t> out;
    out.reserve(hits.size());
    for (const auto& hit : hits) {
        out.push_back(hit.second);
    }
    return out;
}

std::vector<std::uint32_t> CrowdSet::groupsFor(const CrowdSession& session) const
{
    std::vector<std::uint32_t> out;
    for (std::size_t g = 0; g < m_groups.size(); ++g) {
        if (session.shows(m_groups[g])) {
            out.push_back(static_cast<std::uint32_t>(g));
        }
    }
    return out;
}

std::vector<std::uint32_t> CrowdSet::spectatorsFor(const CrowdSession& session, float crowdSize) const
{
    const double keep = std::clamp(static_cast<double>(crowdSize), 0.0, 1.0);
    std::vector<std::uint32_t> out;
    for (const std::uint32_t g : groupsFor(session)) {
        const CrowdGroup& group = m_groups[g];
        for (std::uint32_t i = group.first; i < group.first + group.count; ++i) {
            if (keep >= 1.0 || unitHash(i, kDensitySalt) < keep) {
                out.push_back(i);
            }
        }
    }
    return out;
}

std::vector<glm::mat4> CrowdSet::skinMatrices(std::uint32_t model, std::uint32_t animation, float time) const
{
    if (model >= m_models.size() || animation >= m_animations.size()) {
        throw DataError("skinMatrices: model {} or clip {} out of range", model, animation);
    }
    const SpectatorModel& person = m_models[model];
    const AnimationClip& clip = m_animations[animation].clip;
    const Skeleton& skeleton = m_skeletons[person.skeleton].skeleton;
    if (!skeleton.sameHierarchy(clip.skeleton)) {
        throw DataError("clip {} does not fit the skeleton of {}", clip.name, person.path);
    }
    std::vector<BonePose> pose(clip.boneCount());
    samplePose(clip, time, pose);
    std::vector<glm::mat4> matrices(clip.boneCount());
    spectators::skinMatrices(clip.skeleton, skeleton, pose, matrices);
    mirrorToSpectatorSpace(matrices);
    return matrices;
}

std::vector<glm::vec3> CrowdSet::skinnedPositions(std::uint32_t model, std::uint32_t animation, float time) const
{
    const std::vector<glm::mat4> matrices = skinMatrices(model, animation, time);
    const SkinnedModel& source = m_models[model].source;
    std::vector<glm::vec3> out;
    out.reserve(source.vertices.size());
    for (const SkinnedModel::Vertex& v : source.vertices) {
        const glm::vec4 p(-v.position.x, v.position.y, v.position.z, 1.0F);
        glm::vec4 sum(0.0F);
        for (std::size_t k = 0; k < v.bones.size(); ++k) {
            if (v.weights[k] > 0) {
                sum += matrices[v.bones[k]] * p * (static_cast<float>(v.weights[k]) / 255.0F);
            }
        }
        out.emplace_back(sum);
    }
    return out;
}

SpriteBillboard CrowdSet::spriteFor(std::uint32_t spectator, const glm::vec3& camera) const
{
    const Spectator& person = m_spectators.at(spectator);
    const int configModel = person.model >= 0 ? m_models[static_cast<std::size_t>(person.model)].configModel : 0;
    SpriteBillboard billboard;
    billboard.frame
        = chooseSprite(m_spriteLayout, configModel, person.seated, person.facing(), person.position, camera);
    billboard.width = m_spriteMetrics.cellWidthMetres();
    billboard.height = m_spriteMetrics.cellHeightMetres();
    const float belowGround = (static_cast<float>(SpriteAtlasLayout::kCellHeight) - m_spriteMetrics.groundRow)
        * m_spriteMetrics.metresPerTexel;
    const float classOffset
        = person.crowdClass >= 0 ? m_classes[static_cast<std::size_t>(person.crowdClass)].billboardOffsetY : 0.0F;
    billboard.bottomCentre = person.position + glm::vec3(0.0F, classOffset - belowGround, 0.0F);
    return billboard;
}

} // namespace fh::spectators
