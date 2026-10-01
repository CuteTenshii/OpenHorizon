#include "CarModel.h"

#include "base/Log.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <future>
#include <limits>
#include <map>
#include <utility>

namespace openhorizon::carmodel {

namespace {

constexpr float kMillimetre = 0.001F;
constexpr float kInch = 0.0254F;
constexpr float kPercent = 0.01F;
constexpr std::uint32_t kUnused = std::numeric_limits<std::uint32_t>::max();
/// A `_lod0` exterior whose stock parts have fewer vertices than this is a
/// stand-in (real ones have tens of thousands).
constexpr std::size_t kPlaceholderVertices = 5000;
/// The effect in-race cars are drawn with.
constexpr const char* kCarEffect = "media/shaders/cars/shaders_v16.fxobj";
/// Wheel models with a smaller tyre radius are stand-ins, metres.
constexpr float kMinWheelRadius = 0.15F;

/// Where a part's vertices go: car = origin + rotation · (scale · (vertex -
/// centre)), with X negated before the rotation when mirrored, for a vertex
/// in car space (the file's Z negated).
struct Placement {
    glm::vec3 origin{0.0F};
    glm::vec3 centre{0.0F};
    glm::vec3 scale{1.0F};
    glm::mat3 rotation{1.0F};
    bool mirror = false;

    glm::vec3 point(const glm::vec3& p) const
    {
        glm::vec3 q = scale * (p - centre);
        if (mirror) {
            q.x = -q.x;
        }
        return origin + rotation * q;
    }

    /// File space to car space.
    glm::mat4 matrix() const
    {
        const glm::mat4 flipZ = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 1.0F, -1.0F));
        const glm::vec3 mirrorScale(mirror ? -scale.x : scale.x, scale.y, scale.z);
        return glm::translate(glm::mat4(1.0F), origin) * glm::mat4(rotation) * glm::scale(glm::mat4(1.0F), mirrorScale)
            * glm::translate(glm::mat4(1.0F), -centre) * flipZ;
    }
};

std::uint32_t lowestLod(const CarBin::Part& part)
{
    std::uint32_t lod = std::numeric_limits<std::uint32_t>::max();
    for (const CarBin::Mesh& mesh : part.meshes) {
        lod = std::min(lod, mesh.lod);
    }
    return lod;
}

bool isTyre(const CarBin::Mesh& mesh)
{
    return toLower(mesh.name).find("tire") != std::string::npos;
}

/// The meshes of a part to draw: those of its most detailed LOD, and for
/// wheels, whose LOD 0 has no tyre, the tyre of the most detailed LOD that
/// has one.
std::vector<std::size_t> meshesToDraw(const CarBin::Part& part, ModelSource source)
{
    std::vector<std::size_t> chosen;
    const std::uint32_t lod = lowestLod(part);
    bool hasTyre = false;
    for (std::size_t m = 0; m < part.meshes.size(); ++m) {
        if (part.meshes[m].lod == lod) {
            chosen.push_back(m);
            hasTyre = hasTyre || isTyre(part.meshes[m]);
        }
    }
    if (source != ModelSource::Wheel || hasTyre) {
        return chosen;
    }
    std::uint32_t tyreLod = std::numeric_limits<std::uint32_t>::max();
    for (const CarBin::Mesh& mesh : part.meshes) {
        if (isTyre(mesh)) {
            tyreLod = std::min(tyreLod, mesh.lod);
        }
    }
    for (std::size_t m = 0; m < part.meshes.size(); ++m) {
        if (part.meshes[m].lod == tyreLod && isTyre(part.meshes[m])) {
            chosen.push_back(m);
        }
    }
    return chosen;
}

bool isWheelPart(std::string_view name)
{
    return partVariant(name).slot == "wheel" || toLower(name) == "wheel";
}

/// Tyre size of a wheel model: radius and width of its tyre meshes, or of
/// its box when it has none; `centreX` is the middle of the tyre across.
struct WheelShape {
    float radius = 0.0F;
    float width = 0.0F;
    float centreX = 0.0F;
};

WheelShape measureWheel(const CarBin::Part& part, const std::vector<std::size_t>& meshes)
{
    float radius = 0.0F;
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    for (const std::size_t m : meshes) {
        const CarBin::Mesh& mesh = part.meshes[m];
        if (!isTyre(mesh)) {
            continue;
        }
        const CarBin::VertexBuffer& buffer = part.verticesFor(mesh);
        for (const std::uint32_t index : mesh.triangles) {
            const Vertex v = carbin::decodeVertex(part, buffer, index);
            radius = std::max(radius, std::hypot(v.position.y, v.position.z));
            minX = std::min(minX, v.position.x);
            maxX = std::max(maxX, v.position.x);
        }
    }
    if (radius <= 0.0F) {
        return {std::max(part.halfExtent.y, part.halfExtent.z), part.halfExtent.x * 2.0F, part.position.x};
    }
    return {radius, maxX - minX, (minX + maxX) * 0.5F};
}

/// Axle geometry from `physics/maxdata.xml`.
struct Chassis {
    float wheelbase = 2.6F;
    float frontTrack = 1.6F;
    float rearTrack = 1.6F;
    glm::vec3 wheelbaseCentre{0.0F};
    /// FrontStockRideHeight and RearStockRideHeight: how far the ground lies
    /// below `wheelbaseCentre` at each axle.
    float frontRideHeight = 0.0F;
    float rearRideHeight = 0.0F;
};

std::optional<Chassis> readChassis(const Vfs& vfs, const std::string& folder)
{
    const std::optional<std::string> text = vfs.readText(folder + "physics/maxdata.xml");
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    const pugi::xml_node misc = doc.child("MAXData").child("Misc");
    if (!misc) {
        return std::nullopt;
    }
    Chassis chassis;
    chassis.wheelbase = misc.attribute("Wheelbase").as_float(chassis.wheelbase);
    chassis.frontTrack = misc.attribute("FrontTrackOuter").as_float(chassis.frontTrack);
    chassis.rearTrack = misc.attribute("RearTrackOuter").as_float(chassis.rearTrack);
    chassis.frontRideHeight = misc.attribute("FrontStockRideHeight").as_float();
    chassis.rearRideHeight = misc.attribute("RearStockRideHeight").as_float();
    const pugi::xml_node centre = misc.child("BottomCenterWheelbasePos");
    chassis.wheelbaseCentre = glm::vec3(
        centre.attribute("x").as_float(), centre.attribute("y").as_float(), centre.attribute("z").as_float());
    return chassis;
}

struct TyreSize {
    float radius = 0.0F;
    float width = 0.0F;
};

/// Where the stock body stands (0x827051E0): the wheelbase's bottom centre and
/// the ride height down to the ground at each axle.
struct Stance {
    glm::vec3 bottomCentre{0.0F};
    float frontRideHeight = 0.0F;
    float rearRideHeight = 0.0F;
};

/// Stock wheel and tyre sizes, and the body's stance, from the database.
struct CarRow {
    std::string wheelMediaName;
    std::optional<TyreSize> front;
    std::optional<TyreSize> rear;
    std::optional<Stance> stance;
};

/// Data_CarBody's bottom centre (Z negated) and stock ride heights, the latter
/// replaced by the stock springs' DefRideHeight (0x82705A24-0x82705CA4).
std::optional<Stance> readStance(const Database& db, int carId)
{
    const std::optional<Row> body = db.queryOne(
        "SELECT DCB.* FROM List_UpgradeCarBody LUCB INNER JOIN Data_CarBody DCB ON (DCB.Id = LUCB.CarBodyID) "
        "WHERE LUCB.Ordinal = ? ORDER BY LUCB.IsStock DESC, LUCB.Level ASC LIMIT 1",
        {std::to_string(carId)});
    if (!body) {
        return std::nullopt;
    }
    Stance stance;
    stance.bottomCentre = glm::vec3(body->f("BottomCenterWheelbasePosx"), body->f("BottomCenterWheelbasePosy"),
        -body->f("BottomCenterWheelbasePosZ"));
    stance.frontRideHeight = body->f("ModelFrontStockRideHeight");
    stance.rearRideHeight = body->f("ModelRearStockRideHeight");
    const std::optional<Row> springs
        = db.queryOne("SELECT FrontSpringDamperPhysicsID, RearSpringDamperPhysicsID FROM List_UpgradeSpringDamper "
                      "WHERE Ordinal = ? ORDER BY IsStock DESC, Level ASC LIMIT 1",
            {std::to_string(carId)});
    if (springs) {
        const auto rideHeight = [&db](int id, float fallback) {
            const std::optional<Row> row
                = db.queryOne("SELECT DefRideHeight FROM List_SpringDamperPhysics WHERE SpringDamperPhysicsID = ?",
                    {std::to_string(id)});
            return row ? row->f("DefRideHeight", fallback) : fallback;
        };
        stance.frontRideHeight = rideHeight(springs->i("FrontSpringDamperPhysicsID"), stance.frontRideHeight);
        stance.rearRideHeight = rideHeight(springs->i("RearSpringDamperPhysicsID"), stance.rearRideHeight);
    }
    return stance;
}

CarRow readCarRow(const Database& db, std::string_view mediaName)
{
    CarRow row;
    const std::optional<Row> car
        = db.queryOne("SELECT * FROM Data_Car WHERE lower(MediaName) = lower(?)", {std::string(mediaName)});
    if (!car) {
        return row;
    }
    const auto tyre = [&car](const char* width, const char* aspect, const char* rim) -> std::optional<TyreSize> {
        const float w = car->f(width);
        const float a = car->f(aspect);
        const float d = car->f(rim);
        if (w <= 0.0F || a <= 0.0F || d <= 0.0F) {
            return std::nullopt;
        }
        return TyreSize{d * kInch * 0.5F + w * kMillimetre * a * kPercent, w * kMillimetre};
    };
    row.front = tyre("FrontTireWidthMM", "FrontTireAspect", "FrontWheelDiameterIN");
    row.rear = tyre("RearTireWidthMM", "RearTireAspect", "RearWheelDiameterIN");
    const std::optional<Row> wheel
        = db.queryOne("SELECT MediaName FROM List_Wheels WHERE ID = ?", {std::to_string(car->i("StockWheelID"))});
    if (wheel) {
        row.wheelMediaName = wheel->text("MediaName");
    }
    row.stance = readStance(db, car->i("Id"));
    return row;
}

bool isFront(Corner corner)
{
    return corner == Corner::FrontLeft || corner == Corner::FrontRight;
}

bool isRight(Corner corner)
{
    return corner == Corner::FrontRight || corner == Corner::RearRight;
}

std::string upperCorner(Corner corner)
{
    std::string code(cornerCode(corner));
    for (char& c : code) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return code;
}

world::TextureSurface::Format blockFormat(std::uint32_t format, bool& isBlock)
{
    isBlock = true;
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
        break;
    }
    isBlock = false;
    return world::TextureSurface::Format::Rgba8;
}

/// How the samplers of one file's batches find their textures: samplers the
/// executable binds in code, by name, and the flag sets tried, in order, on
/// the entries of `media/cars/shared/textures.xml`.
struct TextureContext {
    std::map<std::string, std::string> fixed;
    std::vector<std::vector<std::string>> flagSets;
    TextureFolders folders;
};

/// Merges parsed files into a CarMesh and collects their textures.
class MeshBuilder {
public:
    MeshBuilder(CarModel& model, const Vfs& vfs, std::string carFolder)
        : m_model(model)
        , m_vfs(vfs)
        , m_carFolder(std::move(carFolder))
    {
    }

    /// The effect whose techniques the batches name; techniques missing
    /// from it fall back as the executable does.
    void setEffect(std::shared_ptr<const CarEffect> effect) { m_effect = std::move(effect); }
    void setTextureTable(const CarTextureTable* table) { m_textureTable = table; }

    /// Starts a group of batches from one file. `file` is the carbin's
    /// header (its lighting range) and `textures` how its samplers resolve.
    void beginNode(CarMesh::NodeKind kind, std::optional<Corner> corner, glm::vec3 pivot, const Placement& placement,
        const CarBin& file, TextureContext textures)
    {
        CarMesh::Node node;
        node.kind = kind;
        node.corner = corner;
        node.pivot = pivot;
        node.shCompression = file.lightingRange;
        node.placement = placement.matrix();
        m_textures = std::move(textures);
        node.firstBatch = static_cast<std::uint32_t>(m_model.mesh.batches.size());
        m_model.mesh.nodes.push_back(node);
        m_placement = placement;
    }

    /// Adds the chosen meshes of `part` to the current node and records the
    /// part.
    void addPart(
        const std::string& file, const CarBin::Part& part, ModelSource source, const std::vector<std::size_t>& meshes)
    {
        CarModel::Part info;
        info.file = file;
        info.name = part.name;
        info.source = source;
        info.variant = partVariant(part.name);
        info.used = true;
        info.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        info.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        const auto partIndex = static_cast<std::uint32_t>(m_model.parts.size());
        m_ranges.clear();
        for (const std::size_t m : meshes) {
            addMesh(part, part.meshes[m], partIndex, info);
        }
        if (info.vertexCount == 0) {
            info.boundsMin = info.boundsMax = glm::vec3(0.0F);
        }
        m_model.parts.push_back(std::move(info));
    }

    /// Records a part that was left out.
    void skipPart(const std::string& file, const CarBin::Part& part, ModelSource source)
    {
        CarModel::Part info;
        info.file = file;
        info.name = part.name;
        info.source = source;
        info.variant = partVariant(part.name);
        info.used = false;
        for (const CarBin::Mesh& mesh : part.meshes) {
            info.triangleCount += static_cast<std::uint32_t>(mesh.triangles.size() / 3);
        }
        info.vertexCount = part.lod0Vertices.count + part.vertices.count;
        m_model.parts.push_back(std::move(info));
    }

    void finish(bool decodeTextures)
    {
        CarMesh& mesh = m_model.mesh;
        for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
            const std::uint32_t end = n + 1 < mesh.nodes.size() ? mesh.nodes[n + 1].firstBatch
                                                                : static_cast<std::uint32_t>(mesh.batches.size());
            mesh.nodes[n].batchCount = end - mesh.nodes[n].firstBatch;
        }
        computeBounds();
        if (decodeTextures) {
            loadTextures();
        }
    }

private:
    /// The range holding `buffer`'s vertices, copied in on first use.
    std::uint32_t range(const CarBin::Part& part, const CarBin::VertexBuffer& buffer)
    {
        for (const auto& [known, index] : m_ranges) {
            if (known == &buffer) {
                return index;
            }
        }
        CarMesh& mesh = m_model.mesh;
        CarMesh::VertexRange range;
        range.byteOffset = static_cast<std::uint32_t>(mesh.vertices.size());
        range.count = buffer.count;
        range.packedPosition = glm::vec4(part.position, glm::length(part.halfExtent));
        mesh.vertices.insert(mesh.vertices.end(), buffer.data.begin(), buffer.data.end());
        const auto index = static_cast<std::uint32_t>(mesh.ranges.size());
        mesh.ranges.push_back(range);
        m_ranges.emplace_back(&buffer, index);
        return index;
    }

    void addMesh(const CarBin::Part& part, const CarBin::Mesh& source, std::uint32_t partIndex, CarModel::Part& info)
    {
        const CarBin::VertexBuffer& buffer = part.verticesFor(source);
        if (source.triangles.empty() || buffer.stride != carbin::kVertexStride || buffer.data.empty()) {
            return;
        }
        CarMesh& mesh = m_model.mesh;
        CarMesh::Batch batch;
        batch.material = source.name;
        batch.technique = techniqueName(source.name, part.name);
        if (!m_effect || !m_effect->hasTechnique(batch.technique)) {
            batch.technique = missingTechnique(source.name);
        }
        batch.uv0 = source.uv0;
        batch.uv1 = source.uv1;
        batch.translucent = source.translucent;
        batch.motionBlur = isMotionBlurMaterial(source.name);
        bindSamplers(batch);
        batch.node = static_cast<std::uint32_t>(mesh.nodes.size() - 1);
        batch.part = partIndex;
        batch.range = range(part, buffer);
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());
        mesh.indices.insert(mesh.indices.end(), source.triangles.begin(), source.triangles.end());
        batch.indexCount = static_cast<std::uint32_t>(source.triangles.size());
        info.triangleCount += batch.indexCount / 3;

        m_seen.assign(buffer.count, false);
        for (const std::uint32_t index : source.triangles) {
            if (m_seen[index]) {
                continue;
            }
            m_seen[index] = true;
            const glm::vec3 position = m_placement.point(carbin::decodeVertex(part, buffer, index).position);
            info.boundsMin = glm::min(info.boundsMin, position);
            info.boundsMax = glm::max(info.boundsMax, position);
            ++info.vertexCount;
        }
        mesh.batches.push_back(std::move(batch));
    }

    /// Finds the texture of every sampler the batch's technique reads.
    void bindSamplers(CarMesh::Batch& batch)
    {
        if (!m_effect) {
            return;
        }
        for (const auto& pass : m_effect->passes(batch.technique)) {
            for (const xenos::SamplerBinding& sampler : pass->program.samplers) {
                if (sampler.name.empty()) {
                    continue;
                }
                const bool known = std::any_of(batch.textures.begin(), batch.textures.end(),
                    [&](const auto& t) { return t.first == sampler.name; });
                if (known) {
                    continue;
                }
                if (const std::optional<std::string> path = resolveSampler(sampler.name)) {
                    batch.textures.emplace_back(sampler.name, texture(*path));
                }
            }
        }
    }

    std::optional<std::string> resolveSampler(const std::string& sampler) const
    {
        if (const auto fixed = m_textures.fixed.find(sampler); fixed != m_textures.fixed.end()) {
            return m_vfs.exists(fixed->second) ? std::optional<std::string>(fixed->second) : std::nullopt;
        }
        if (m_textureTable == nullptr) {
            return std::nullopt;
        }
        for (const std::vector<std::string>& flags : m_textures.flagSets) {
            if (std::optional<std::string> path = m_textureTable->resolve(sampler, flags, m_textures.folders)) {
                if (m_vfs.exists(*path)) {
                    return path;
                }
            }
        }
        return std::nullopt;
    }

    std::uint32_t texture(const std::string& path)
    {
        const auto known = m_textureIndex.find(path);
        if (known != m_textureIndex.end()) {
            return known->second;
        }
        const auto index = static_cast<std::uint32_t>(m_model.textures.size());
        m_model.textures.push_back({path, {}});
        m_textureIndex.emplace(path, index);
        return index;
    }

    void computeBounds()
    {
        CarMesh& mesh = m_model.mesh;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        bool any = false;
        for (const CarMesh::Batch& batch : mesh.batches) {
            if (batch.motionBlur) {
                continue;
            }
            for (std::uint32_t i = batch.firstIndex; i < batch.firstIndex + batch.indexCount; ++i) {
                const glm::vec3 p = mesh.position(batch, mesh.indices[i]);
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
                any = true;
            }
        }
        mesh.boundsMin = any ? lo : glm::vec3(0.0F);
        mesh.boundsMax = any ? hi : glm::vec3(0.0F);
    }

    void loadTextures()
    {
        std::vector<std::future<std::optional<world::TextureMipChain>>> jobs;
        jobs.reserve(m_model.textures.size());
        for (const CarModel::Texture& texture : m_model.textures) {
            jobs.push_back(
                std::async(std::launch::async, [this, path = texture.path]() -> std::optional<world::TextureMipChain> {
                const std::optional<Bytes> data = m_vfs.read(path);
                if (!data) {
                    return std::nullopt;
                }
                return decodeXdsTexture(*data);
            }));
        }
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            CarModel::Texture& texture = m_model.textures[i];
            try {
                std::optional<world::TextureMipChain> chain = jobs[i].get();
                if (chain) {
                    texture.chain = std::move(*chain);
                } else {
                    m_model.warnings.push_back(std::format("{}: cannot be read", texture.path));
                }
            } catch (const DataError& e) {
                m_model.warnings.push_back(std::format("{}: {}", texture.path, e.what()));
            }
        }
    }

    CarModel& m_model;
    const Vfs& m_vfs;
    std::string m_carFolder;
    std::shared_ptr<const CarEffect> m_effect;
    const CarTextureTable* m_textureTable = nullptr;
    TextureContext m_textures;
    std::map<std::string, std::uint32_t> m_textureIndex;
    Placement m_placement;
    /// Vertex buffers of the current part already in the mesh.
    std::vector<std::pair<const CarBin::VertexBuffer*, std::uint32_t>> m_ranges;
    std::vector<bool> m_seen;
};

std::optional<CarBin> tryLoad(const Vfs& vfs, const std::string& path, CarBin::Kind kind, CarModel& model)
{
    if (!vfs.exists(path)) {
        return std::nullopt;
    }
    try {
        CarBin bin = loadCarBin(vfs, path, kind);
        model.files.push_back(path);
        return bin;
    } catch (const DataError& e) {
        model.warnings.emplace_back(e.what());
        return std::nullopt;
    }
}

std::size_t stockVertexCount(const CarBin& model)
{
    std::size_t count = 0;
    for (const CarBin::Part& part : model.parts) {
        if (partVariant(part.name).stock) {
            count += part.vertices.count + part.lod0Vertices.count;
        }
    }
    return count;
}

/// Which exterior parts a stock car shows: parts without variants and the
/// "a" variants, and for exhaust slots that have no "a" part but exactly one
/// lettered one, that one (inferred: a few cars name their only exhaust "c"
/// or "u").
std::vector<bool> stockParts(const CarBin& exterior)
{
    std::map<std::string, std::vector<std::size_t>> lettered;
    std::vector<bool> stock(exterior.parts.size(), false);
    for (std::size_t p = 0; p < exterior.parts.size(); ++p) {
        const PartVariant variant = partVariant(exterior.parts[p].name);
        stock[p] = variant.stock;
        if (!variant.variant.empty() && variant.variant != "race") {
            lettered[variant.slot].push_back(p);
        }
    }
    for (const auto& [slot, parts] : lettered) {
        if (!slot.starts_with("exhaust")) {
            continue;
        }
        const bool hasStock = std::any_of(parts.begin(), parts.end(), [&](std::size_t p) { return stock[p]; });
        std::vector<std::string> letters;
        for (const std::size_t p : parts) {
            const std::string letter = partVariant(exterior.parts[p].name).variant;
            if (std::find(letters.begin(), letters.end(), letter) == letters.end()) {
                letters.push_back(letter);
            }
        }
        if (!hasStock && letters.size() == 1) {
            for (const std::size_t p : parts) {
                stock[p] = true;
            }
        }
    }
    return stock;
}

/// Notes batches whose technique the effect lacks, which draw with the
/// executable's `missing` fallback, and translates every technique used so
/// the renderer only has to compile them.
void checkTechniques(CarModel& model)
{
    if (!model.effect) {
        return;
    }
    std::vector<std::string> done;
    for (const CarMesh::Batch& batch : model.mesh.batches) {
        if (std::find(done.begin(), done.end(), batch.technique) != done.end()) {
            continue;
        }
        done.push_back(batch.technique);
        if (batch.technique.starts_with("missing")) {
            model.warnings.push_back(
                std::format("material {}: {} has no technique for it", batch.material, kCarEffect));
        }
        if (model.effect->passes(batch.technique).empty()) {
            model.warnings.push_back(model.effect->error(batch.technique));
        }
    }
}

/// The shader settings files layered for a car, as CCarShaderDef:Load does:
/// the shared base, the paint finish's differences, the car's own file.
ShaderSettings loadSettings(const Vfs& vfs, const std::string& folder, std::string_view finish)
{
    ShaderSettings settings
        = ShaderSettings::load(vfs, "media/cars/shared/shadersettings/normal.xml").value_or(ShaderSettings{});
    if (toLower(finish) != "normal") {
        if (const auto finishSettings
            = ShaderSettings::load(vfs, std::format("media/cars/shared/shadersettings/{}.xml", toLower(finish)))) {
            settings.overlay(*finishSettings);
        }
    }
    if (const auto own = ShaderSettings::load(vfs, folder + "shadersettings.xml")) {
        settings.overlay(*own);
    }
    return settings;
}

std::optional<CarModel::BlurRim> readBlurRim(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText("media/cars/shared/globalcarattribs.xml");
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    const pugi::xml_node node = doc.child("GlobalCarAttributes").child("BlurRim");
    if (!node) {
        return std::nullopt;
    }
    return CarModel::BlurRim{node.attribute("MaxRPM").as_float(), node.attribute("GeomBlendStart").as_float(),
        node.attribute("GeomBlendEnd").as_float()};
}

/// The paint finishes of List_ColorFinish that have their own settings file
/// (0 Normal, 1 Metallic; CarbonFiber has none on the disc).
constexpr std::array<std::string_view, 2> kFinishes{"Normal", "Metallic"};

} // namespace

glm::vec3 CarMesh::position(const Batch& batch, std::uint32_t index) const
{
    const VertexRange& range = ranges.at(batch.range);
    if (index >= range.count) {
        return glm::vec3(0.0F);
    }
    const glm::vec3 file = carbin::decodeFilePosition(
        vertices.data() + range.byteOffset + static_cast<std::size_t>(index) * carbin::kVertexStride,
        range.packedPosition);
    return glm::vec3(nodes.at(batch.node).placement * glm::vec4(file, 1.0F));
}

std::string_view cornerCode(Corner corner)
{
    switch (corner) {
    case Corner::FrontLeft:
        return "lf";
    case Corner::FrontRight:
        return "rf";
    case Corner::RearLeft:
        return "lr";
    case Corner::RearRight:
        return "rr";
    }
    return "lf";
}

CarBin loadCarBin(const Vfs& vfs, std::string_view path, CarBin::Kind kind)
{
    const Bytes data = vfs.readOrThrow(path);
    return carbin::parse(data, std::string(path), kind);
}

world::TextureMipChain decodeXdsTexture(ByteView data)
{
    try {
        const xds::Texture texture = xds::Texture::parse(data);
        const xds::TextureInfo& info = texture.info();
        bool isBlock = false;
        world::TextureSurface top;
        top.format = blockFormat(info.format, isBlock);
        top.width = static_cast<int>(info.width);
        top.height = static_cast<int>(info.height);
        const std::array<xds::Swizzle, 4> identity{xds::Swizzle::X, xds::Swizzle::Y, xds::Swizzle::Z, xds::Swizzle::W};
        if (isBlock && info.swizzle == identity) {
            top.data = texture.extractLevel(0).data;
        } else {
            top.format = world::TextureSurface::Format::Rgba8;
            top.data = texture.decodeLevel(0).rgba;
        }
        return world::buildMipChain(top);
    } catch (const xds::Error& e) {
        throw DataError("{}", e.what());
    }
}

CarModel CarModel::load(const Vfs& vfs, const Database* db, std::string_view mediaName, const CarModelOptions& options)
{
    CarModel model;
    model.mediaName = toLower(mediaName);
    const std::string car = model.mediaName;
    const std::string folder = std::format("media/cars/{}/", car);
    MeshBuilder builder(model, vfs, folder);
    try {
        model.effect = CarEffect::load(vfs, kCarEffect);
    } catch (const DataError& e) {
        model.warnings.emplace_back(e.what());
    }
    builder.setEffect(model.effect);
    const CarTextureTable textureTable = CarTextureTable::load(vfs);
    builder.setTextureTable(&textureTable);

    std::string exteriorFile = folder + car + (options.inRaceLods ? ".carbin" : "_lod0.carbin");
    std::optional<CarBin> exterior = tryLoad(vfs, exteriorFile, CarBin::Kind::Car, model);
    bool exteriorIsLod0 = !options.inRaceLods;
    // Some traffic vehicles made drivable ship a `_lod0` of 4- to 17-vertex
    // stand-ins for their stock parts; their real geometry is in the plain
    // file.
    if (!options.inRaceLods && (!exterior || stockVertexCount(*exterior) < kPlaceholderVertices)) {
        const std::string plainFile = folder + car + ".carbin";
        std::optional<CarBin> plain = tryLoad(vfs, plainFile, CarBin::Kind::Car, model);
        if (plain && (!exterior || stockVertexCount(*plain) > stockVertexCount(*exterior))) {
            if (exterior) {
                model.files.erase(std::find(model.files.begin(), model.files.end(), exteriorFile));
            }
            exterior = std::move(plain);
            exteriorFile = plainFile;
            exteriorIsLod0 = false;
        } else if (plain) {
            model.files.pop_back();
        }
    }
    if (!exterior) {
        throw DataError("car {}: no readable exterior model in {}{}", car, folder,
            model.warnings.empty() ? std::string() : ": " + model.warnings.back());
    }

    CarRow row;
    if (db) {
        row = readCarRow(*db, car);
    }
    model.wheelMediaName = toLower(row.wheelMediaName.empty() ? car : row.wheelMediaName);
    for (const std::string_view finish : kFinishes) {
        model.settings.emplace(std::string(finish), loadSettings(vfs, folder, finish));
    }
    model.tyreSettings
        = ShaderSettings::load(vfs, "media/cars/shared/shadersettings/tire/normal.xml").value_or(ShaderSettings{});
    model.blurRim = readBlurRim(vfs);

    const std::optional<Chassis> chassisRead = readChassis(vfs, folder);
    if (!chassisRead) {
        model.warnings.push_back(std::format("{}physics/maxdata.xml: missing; wheel positions guessed", folder));
    }
    const Chassis chassis = chassisRead.value_or(Chassis{});

    const std::string wheelFolder = std::format("media/wheels/{}/", model.wheelMediaName);
    const std::string rightFolder = std::format("media/wheels/{}_rightside/", model.wheelMediaName);

    // baseSampler and damageSampler are bound in code (0x82E5FEA0): the
    // car's nodamage and damage textures, `_LOD0.xds` for the most detailed
    // model and `.xds` for the in-race LODs. Every other sampler comes from
    // textures.xml. Which load flags the in-race car sets is not traced:
    // LOD0 with Cockpit is tried first (the only set under which the cockpit
    // and lamp samplers of these models resolve), then LOD15, whose entries
    // (number plates) have no LOD0 counterpart.
    const auto carTextures = [&](bool lod0) {
        TextureContext context;
        const std::string suffix = lod0 ? "_lod0.xds" : ".xds";
        context.fixed["baseSampler"] = folder + "nodamage" + suffix;
        context.fixed["damageSampler"] = folder + "damage" + suffix;
        context.flagSets = {{lod0 ? "LOD0" : "LOD15", "Cockpit", "InGameTextures", "CarCommon"},
            {"LOD15", "Cockpit", "InGameTextures", "CarCommon"}};
        context.folders.car = folder;
        context.folders.wheel = wheelFolder;
        return context;
    };

    // As 0x82706048-0x827061A4: forward along (0, front - rear ride height,
    // wheelbase), moved by (0, average ride height, 0) - bottom centre.
    Stance stance{chassis.wheelbaseCentre, chassis.frontRideHeight, chassis.rearRideHeight};
    if (row.stance) {
        stance = *row.stance;
    }
    model.frontRideHeight = stance.frontRideHeight;
    model.rearRideHeight = stance.rearRideHeight;
    const glm::vec3 forward
        = glm::normalize(glm::vec3(0.0F, stance.frontRideHeight - stance.rearRideHeight, chassis.wheelbase));
    Placement body;
    body.rotation = glm::mat3(glm::vec3(1.0F, 0.0F, 0.0F), glm::cross(forward, glm::vec3(1.0F, 0.0F, 0.0F)), forward);
    body.origin = glm::vec3(0.0F, (stance.frontRideHeight + stance.rearRideHeight) * 0.5F, 0.0F) - stance.bottomCentre;
    builder.beginNode(
        CarMesh::NodeKind::Body, std::nullopt, glm::vec3(0.0F), body, *exterior, carTextures(exteriorIsLod0));
    const std::vector<bool> stock = stockParts(*exterior);
    const CarBin::Part* builtInWheel = nullptr;
    for (std::size_t p = 0; p < exterior->parts.size(); ++p) {
        const CarBin::Part& part = exterior->parts[p];
        if (isWheelPart(part.name)) {
            if (!builtInWheel || stock[p]) {
                builtInWheel = &part;
            }
            builder.skipPart(exteriorFile, part, ModelSource::Exterior);
            continue;
        }
        if (options.stockOnly && !stock[p]) {
            builder.skipPart(exteriorFile, part, ModelSource::Exterior);
            continue;
        }
        builder.addPart(exteriorFile, part, ModelSource::Exterior, meshesToDraw(part, ModelSource::Exterior));
    }

    if (options.cockpit) {
        const std::string file = folder + car + "_cockpit.carbin";
        if (const std::optional<CarBin> cockpit = tryLoad(vfs, file, CarBin::Kind::Car, model)) {
            builder.beginNode(
                CarMesh::NodeKind::Cockpit, std::nullopt, glm::vec3(0.0F), body, *cockpit, carTextures(true));
            for (const CarBin::Part& part : cockpit->parts) {
                if (options.stockOnly && !partVariant(part.name).stock) {
                    builder.skipPart(file, part, ModelSource::Cockpit);
                    continue;
                }
                builder.addPart(file, part, ModelSource::Cockpit, meshesToDraw(part, ModelSource::Cockpit));
            }
        } else {
            model.warnings.push_back(std::format("{}: no cockpit model", file));
        }
    }

    if (!options.wheels && !options.brakes) {
        builder.finish(options.textures);
        checkTechniques(model);
        return model;
    }

    // The wheel: its own archive, else the exterior's built-in rim.
    const std::string wheelFile = wheelFolder + model.wheelMediaName + ".carbin";
    const std::string rightFile = rightFolder + model.wheelMediaName + "_rightside.carbin";
    std::optional<CarBin> wheel;
    std::optional<CarBin> rightWheel;
    if (options.wheels) {
        wheel = tryLoad(vfs, wheelFile, CarBin::Kind::Wheel, model);
        rightWheel = tryLoad(vfs, rightFile, CarBin::Kind::Wheel, model);
        if (!wheel) {
            model.warnings.push_back(std::format("{}: no wheel model; using the exterior's rim", wheelFile));
        }
    }
    const CarBin::Part* leftPart = wheel && !wheel->parts.empty() ? &wheel->parts.front() : builtInWheel;
    const CarBin::Part* rightPart = rightWheel && !rightWheel->parts.empty() ? &rightWheel->parts.front() : nullptr;
    const CarBin* leftBin = wheel ? &*wheel : &*exterior;
    std::string leftFile = wheel ? wheelFile : exteriorFile;
    std::vector<std::size_t> leftMeshes
        = leftPart ? meshesToDraw(*leftPart, ModelSource::Wheel) : std::vector<std::size_t>{};
    WheelShape leftShape = leftPart ? measureWheel(*leftPart, leftMeshes) : WheelShape{};
    // The traffic vehicles' wheel archives hold stand-ins a few centimetres
    // across; their exterior's own wheel is the real one.
    if (wheel && leftShape.radius < kMinWheelRadius && builtInWheel) {
        model.warnings.push_back(std::format("{}: stand-in wheel model; using the exterior's wheel", wheelFile));
        leftPart = builtInWheel;
        leftBin = &*exterior;
        rightPart = nullptr;
        leftFile = exteriorFile;
        leftMeshes = meshesToDraw(*leftPart, ModelSource::Wheel);
        leftShape = measureWheel(*leftPart, leftMeshes);
    }
    const std::vector<std::size_t> rightMeshes
        = rightPart ? meshesToDraw(*rightPart, ModelSource::Wheel) : std::vector<std::size_t>{};
    const WheelShape rightShape = rightPart ? measureWheel(*rightPart, rightMeshes) : leftShape;

    // Wheel textures (0x82E68398): `wheel_LOD0.xds` for the rim, the blurred
    // rims `wheel<n>.xds` (`%swheel%d.xds`) and the blurred tyres of the
    // shared `tire<compound><n>.xds`. The stock compound letter is `a` (the
    // executable picks a for compound levels up to 1; stock cars have 0).
    // Inferred: blurRimSampler<n> and blurTireSampler<n> take the files in
    // numeric order (sampler 1 the file numbered 0).
    const auto wheelTextures = [&](const std::string& ownFolder) {
        TextureContext context;
        context.fixed["baseSampler"] = ownFolder + "wheel_lod0.xds";
        const std::array<std::string_view, 4> rimSamplers{
            "blurRimSampler1", "blurRimSampler2", "blurRimSampler3", "blurRimSampler4"};
        const std::array<std::string_view, 4> tyreSamplers{
            "blurTireSampler", "blurTireSampler2", "blurTireSampler3", "blurTireSampler4"};
        for (std::size_t i = 0; i < rimSamplers.size(); ++i) {
            context.fixed[std::string(rimSamplers[i])] = std::format("{}wheel{}.xds", ownFolder, i);
            context.fixed[std::string(tyreSamplers[i])] = std::format("media/cars/shared/tirea{}.xds", i);
        }
        context.folders.car = folder;
        context.folders.wheel = ownFolder;
        return context;
    };

    for (const Corner corner : kCorners) {
        const bool front = isFront(corner);
        const bool right = isRight(corner);
        const bool ownRight = right && rightPart != nullptr;
        const WheelShape& shape = ownRight ? rightShape : leftShape;
        const std::optional<TyreSize>& tyre = front ? row.front : row.rear;
        WheelMount mount;
        mount.corner = corner;
        mount.radius = tyre ? tyre->radius : shape.radius;
        mount.width = tyre ? tyre->width : shape.width;
        mount.radialScale = shape.radius > 0.0F ? mount.radius / shape.radius : 1.0F;
        mount.axialScale = shape.width > 0.0F ? mount.width / shape.width : 1.0F;
        const float track = front ? chassis.frontTrack : chassis.rearTrack;
        const float x = track * 0.5F - mount.width * 0.5F;
        mount.centre = glm::vec3(right ? x : -x, mount.radius, (front ? 0.5F : -0.5F) * chassis.wheelbase);
        mount.mirrored = right && !ownRight;

        if (options.wheels && leftPart) {
            const CarBin::Part& part = ownRight ? *rightPart : *leftPart;
            Placement placement;
            placement.origin = mount.centre;
            placement.centre = glm::vec3(shape.centreX, 0.0F, 0.0F);
            placement.scale = glm::vec3(mount.axialScale, mount.radialScale, mount.radialScale);
            placement.mirror = mount.mirrored;
            TextureContext textures = wheelTextures(ownRight ? rightFolder : (wheel ? wheelFolder : folder));
            builder.beginNode(CarMesh::NodeKind::Wheel, corner, mount.centre, placement,
                ownRight ? *rightWheel : *leftBin, std::move(textures));
            builder.addPart(
                ownRight ? rightFile : leftFile, part, ModelSource::Wheel, ownRight ? rightMeshes : leftMeshes);
        }
        if (options.brakes) {
            const std::string code(cornerCode(corner));
            Placement placement;
            placement.origin = mount.centre;
            for (const auto& [kind, name] : {std::pair{CarMesh::NodeKind::Rotor, std::string("rotor")},
                     std::pair{CarMesh::NodeKind::Caliper, std::string("caliper")}}) {
                const std::string file = std::format("{}{}_{}{}_lod0.carbin", folder, car, name, code);
                const std::optional<CarBin> brake = tryLoad(vfs, file, CarBin::Kind::Brake, model);
                if (!brake) {
                    model.warnings.push_back(std::format("{}: no {} {}", file, name, upperCorner(corner)));
                    continue;
                }
                builder.beginNode(kind, corner, mount.centre, placement, *brake, carTextures(true));
                for (const CarBin::Part& part : brake->parts) {
                    builder.addPart(file, part, ModelSource::Brake, meshesToDraw(part, ModelSource::Brake));
                }
            }
        }
        model.wheels.push_back(mount);
    }
    builder.finish(options.textures);
    checkTechniques(model);
    return model;
}

} // namespace openhorizon::carmodel
