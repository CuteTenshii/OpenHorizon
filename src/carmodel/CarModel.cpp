#include "CarModel.h"

#include "base/Log.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
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

namespace fh::carmodel {

namespace {

/// The lighting byte's value on open surfaces (the peak of its
/// distribution over the cars), mapped to white.
constexpr float kOpenLighting = 106.0F;
constexpr float kMillimetre = 0.001F;
constexpr float kInch = 0.0254F;
constexpr float kPercent = 0.01F;
constexpr std::uint32_t kUnused = std::numeric_limits<std::uint32_t>::max();
/// A `_lod0` exterior whose stock parts have fewer vertices than this is a
/// stand-in (real ones have tens of thousands).
constexpr std::size_t kPlaceholderVertices = 5000;
/// Wheel models with a smaller tyre radius are stand-ins, metres.
constexpr float kMinWheelRadius = 0.15F;

/// Where a mesh's vertices go: car = origin + scale · (vertex - centre),
/// with X negated afterwards when mirrored.
struct Placement {
    glm::vec3 origin{0.0F};
    glm::vec3 centre{0.0F};
    glm::vec3 scale{1.0F};
    bool mirror = false;

    glm::vec3 point(const glm::vec3& p) const
    {
        glm::vec3 q = scale * (p - centre);
        if (mirror) {
            q.x = -q.x;
        }
        return origin + q;
    }

    glm::vec3 direction(const glm::vec3& n) const
    {
        glm::vec3 q = n / scale;
        if (mirror) {
            q.x = -q.x;
        }
        const float length = glm::length(q);
        return length > 0.0F ? q / length : glm::vec3(0.0F, 1.0F, 0.0F);
    }
};

std::uint32_t packColour(const Vertex& v)
{
    std::uint8_t grey = 255;
    const bool hasLighting = v.lighting[0] != 0 || v.lighting[1] != 0 || v.lighting[2] != 0 || v.lighting[3] != 0;
    if (hasLighting) {
        grey = static_cast<std::uint8_t>(std::min(255.0F, std::round(v.lighting[0] * 255.0F / kOpenLighting)));
    }
    const std::array<std::uint8_t, 4> rgba{grey, grey, grey, 255};
    std::uint32_t bits = 0;
    std::memcpy(&bits, rgba.data(), sizeof(bits));
    return bits;
}

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
    const pugi::xml_node centre = misc.child("BottomCenterWheelbasePos");
    chassis.wheelbaseCentre = glm::vec3(
        centre.attribute("x").as_float(), centre.attribute("y").as_float(), centre.attribute("z").as_float());
    return chassis;
}

struct TyreSize {
    float radius = 0.0F;
    float width = 0.0F;
};

/// Stock wheel and tyre sizes from the database.
struct CarRow {
    std::string wheelMediaName;
    std::optional<TyreSize> front;
    std::optional<TyreSize> rear;
};

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

/// Merges parsed files into a CarMesh and collects their textures.
class MeshBuilder {
public:
    MeshBuilder(CarModel& model, const Vfs& vfs, std::string carFolder)
        : m_model(model)
        , m_vfs(vfs)
        , m_carFolder(std::move(carFolder))
    {
    }

    /// Folders searched for TextureFolder::Wheel files, in order.
    void setWheelFolders(std::vector<std::string> folders) { m_wheelFolders = std::move(folders); }

    void beginNode(CarMesh::NodeKind kind, std::optional<Corner> corner, glm::vec3 pivot)
    {
        CarMesh::Node node;
        node.kind = kind;
        node.corner = corner;
        node.pivot = pivot;
        node.firstBatch = static_cast<std::uint32_t>(m_model.mesh.batches.size());
        m_model.mesh.nodes.push_back(node);
    }

    /// Adds the chosen meshes of `part` to the current node and records the
    /// part.
    void addPart(const std::string& file, const CarBin::Part& part, ModelSource source,
        const std::vector<std::size_t>& meshes, const Placement& placement)
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
        for (const std::size_t m : meshes) {
            addMesh(part, part.meshes[m], source, partIndex, placement, info);
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
        for (CarMesh::Node& node : mesh.nodes) {
            node.batchCount = 0;
        }
        for (std::size_t n = 0; n < mesh.nodes.size(); ++n) {
            const std::uint32_t end
                = n + 1 < mesh.nodes.size() ? mesh.nodes[n + 1].firstBatch : static_cast<std::uint32_t>(mesh.batches.size());
            mesh.nodes[n].batchCount = end - mesh.nodes[n].firstBatch;
        }
        computeBounds();
        if (decodeTextures) {
            loadTextures();
        }
    }

private:
    void addMesh(const CarBin::Part& part, const CarBin::Mesh& source, ModelSource from, std::uint32_t partIndex,
        const Placement& placement, CarModel::Part& info)
    {
        const CarBin::VertexBuffer& buffer = part.verticesFor(source);
        if (source.triangles.empty() || buffer.stride != carbin::kVertexStride || buffer.data.empty()) {
            return;
        }
        CarMesh& mesh = m_model.mesh;
        CarMesh::Batch batch;
        batch.material = source.name;
        batch.translucent = source.translucent;
        batch.shading = classifyMaterial(source.name, from, source.translucent);
        batch.motionBlur = isMotionBlurMaterial(source.name);
        batch.texture = texture(diffuseTextures(source.name, from));
        batch.colour = baseColour(source.name, batch.shading, batch.texture != CarMesh::kNoTexture);
        batch.node = static_cast<std::uint32_t>(mesh.nodes.size() - 1);
        batch.part = partIndex;
        batch.firstIndex = static_cast<std::uint32_t>(mesh.indices.size());

        m_remap.assign(buffer.count, kUnused);
        for (std::size_t i = 0; i < source.triangles.size(); i += 3) {
            std::array<std::uint32_t, 3> corners{};
            for (std::size_t k = 0; k < 3; ++k) {
                corners[k] = vertexIndex(part, buffer, source, source.triangles[i + k], placement, info);
            }
            // A mirror reverses the winding; swapping two corners restores it.
            if (placement.mirror) {
                std::swap(corners[1], corners[2]);
            }
            mesh.indices.insert(mesh.indices.end(), corners.begin(), corners.end());
        }
        batch.indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - batch.firstIndex;
        info.triangleCount += batch.indexCount / 3;
        mesh.batches.push_back(std::move(batch));
    }

    std::uint32_t vertexIndex(const CarBin::Part& part, const CarBin::VertexBuffer& buffer, const CarBin::Mesh& source,
        std::uint32_t index, const Placement& placement, CarModel::Part& info)
    {
        std::uint32_t& mapped = m_remap[index];
        if (mapped != kUnused) {
            return mapped;
        }
        CarMesh& mesh = m_model.mesh;
        mapped = static_cast<std::uint32_t>(mesh.vertexCount());
        const Vertex v = carbin::decodeVertex(part, buffer, index);
        const glm::vec3 position = placement.point(v.position);
        const glm::vec3 normal = placement.direction(v.normal);
        const glm::vec2 uv0 = carbin::meshUv0(source, v);
        const glm::vec2 uv1 = carbin::meshUv1(source, v);
        const float colour = std::bit_cast<float>(packColour(v));
        const std::array<float, CarMesh::kFloatsPerVertex> floats{position.x, position.y, position.z, normal.x,
            normal.y, normal.z, uv0.x, uv0.y, uv1.x, uv1.y, colour};
        mesh.vertices.insert(mesh.vertices.end(), floats.begin(), floats.end());
        info.boundsMin = glm::min(info.boundsMin, position);
        info.boundsMax = glm::max(info.boundsMax, position);
        ++info.vertexCount;
        return mapped;
    }

    std::vector<std::string> folderPaths(TextureFolder folder) const
    {
        switch (folder) {
        case TextureFolder::Car:
            return {m_carFolder};
        case TextureFolder::Shared:
            return {"media/cars/shared/"};
        case TextureFolder::Wheel:
            return m_wheelFolders;
        }
        return {m_carFolder};
    }

    std::uint32_t texture(const std::vector<TextureCandidate>& candidates)
    {
        for (const TextureCandidate& candidate : candidates) {
            for (const std::string& folder : folderPaths(candidate.folder)) {
                const std::string path = folder + candidate.file;
                const auto known = m_textureIndex.find(path);
                if (known != m_textureIndex.end()) {
                    return known->second;
                }
                if (!m_vfs.exists(path)) {
                    continue;
                }
                const auto index = static_cast<std::uint32_t>(m_model.textures.size());
                m_model.textures.push_back({path, {}});
                m_textureIndex.emplace(path, index);
                return index;
            }
        }
        return CarMesh::kNoTexture;
    }

    void computeBounds()
    {
        CarMesh& mesh = m_model.mesh;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        bool any = false;
        for (const CarMesh::Node& node : mesh.nodes) {
            for (std::uint32_t b = node.firstBatch; b < node.firstBatch + node.batchCount; ++b) {
                const CarMesh::Batch& batch = mesh.batches[b];
                if (batch.motionBlur) {
                    continue;
                }
                for (std::uint32_t i = batch.firstIndex; i < batch.firstIndex + batch.indexCount; ++i) {
                    const float* v = mesh.vertices.data() + static_cast<std::size_t>(mesh.indices[i]) * CarMesh::kFloatsPerVertex;
                    lo = glm::min(lo, glm::vec3(v[0], v[1], v[2]));
                    hi = glm::max(hi, glm::vec3(v[0], v[1], v[2]));
                    any = true;
                }
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
            jobs.push_back(std::async(std::launch::async, [this, path = texture.path]() -> std::optional<world::TextureMipChain> {
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
    std::vector<std::string> m_wheelFolders;
    std::map<std::string, std::uint32_t> m_textureIndex;
    std::vector<std::uint32_t> m_remap;
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
        model.warnings.push_back(e.what());
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

} // namespace

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

    std::string exteriorFile = folder + car + "_lod0.carbin";
    std::optional<CarBin> exterior = tryLoad(vfs, exteriorFile, CarBin::Kind::Car, model);
    // Some traffic vehicles made drivable ship a `_lod0` of 4- to 17-vertex
    // stand-ins for their stock parts; their real geometry is in the plain
    // file.
    if (!exterior || stockVertexCount(*exterior) < kPlaceholderVertices) {
        const std::string plainFile = folder + car + ".carbin";
        std::optional<CarBin> plain = tryLoad(vfs, plainFile, CarBin::Kind::Car, model);
        if (plain && (!exterior || stockVertexCount(*plain) > stockVertexCount(*exterior))) {
            if (exterior) {
                model.files.erase(std::find(model.files.begin(), model.files.end(), exteriorFile));
            }
            exterior = std::move(plain);
            exteriorFile = plainFile;
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

    const std::optional<Chassis> chassisRead = readChassis(vfs, folder);
    if (!chassisRead) {
        model.warnings.push_back(std::format("{}physics/maxdata.xml: missing; wheel positions guessed", folder));
    }
    const Chassis chassis = chassisRead.value_or(Chassis{});

    // Models are centred on their own box; BottomCenterWheelbasePos (car
    // space) is where the wheelbase centre on the ground lies in the model
    // once Z is flipped, so subtracting it moves the origin there. Verified:
    // it brings the exteriors closer to their collision boxes (the 1968
    // Beetle's floor from 0.158 m below the ground up to it), and the wheel
    // arch openings of the Aston Martin DB5, Jaguar E-type and Audi R8 onto
    // the axles at half the wheelbase either side.
    Placement body;
    body.origin = -chassis.wheelbaseCentre;
    builder.beginNode(CarMesh::NodeKind::Body, std::nullopt, glm::vec3(0.0F));
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
        builder.addPart(exteriorFile, part, ModelSource::Exterior, meshesToDraw(part, ModelSource::Exterior), body);
    }

    if (options.cockpit) {
        const std::string file = folder + car + "_cockpit.carbin";
        if (const std::optional<CarBin> cockpit = tryLoad(vfs, file, CarBin::Kind::Car, model)) {
            builder.beginNode(CarMesh::NodeKind::Cockpit, std::nullopt, glm::vec3(0.0F));
            for (const CarBin::Part& part : cockpit->parts) {
                if (options.stockOnly && !partVariant(part.name).stock) {
                    builder.skipPart(file, part, ModelSource::Cockpit);
                    continue;
                }
                builder.addPart(file, part, ModelSource::Cockpit, meshesToDraw(part, ModelSource::Cockpit), body);
            }
        } else {
            model.warnings.push_back(std::format("{}: no cockpit model", file));
        }
    }

    if (!options.wheels && !options.brakes) {
        builder.finish(options.textures);
        return model;
    }

    // The wheel: its own archive, else the exterior's built-in rim.
    const std::string wheelFolder = std::format("media/wheels/{}/", model.wheelMediaName);
    const std::string wheelFile = wheelFolder + model.wheelMediaName + ".carbin";
    const std::string rightFolder = std::format("media/wheels/{}_rightside/", model.wheelMediaName);
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
    std::string leftFile = wheel ? wheelFile : exteriorFile;
    std::vector<std::size_t> leftMeshes
        = leftPart ? meshesToDraw(*leftPart, ModelSource::Wheel) : std::vector<std::size_t>{};
    WheelShape leftShape = leftPart ? measureWheel(*leftPart, leftMeshes) : WheelShape{};
    // The traffic vehicles' wheel archives hold stand-ins a few centimetres
    // across; their exterior's own wheel is the real one.
    if (wheel && leftShape.radius < kMinWheelRadius && builtInWheel) {
        model.warnings.push_back(std::format("{}: stand-in wheel model; using the exterior's wheel", wheelFile));
        leftPart = builtInWheel;
        rightPart = nullptr;
        leftFile = exteriorFile;
        leftMeshes = meshesToDraw(*leftPart, ModelSource::Wheel);
        leftShape = measureWheel(*leftPart, leftMeshes);
    }
    const std::vector<std::size_t> rightMeshes
        = rightPart ? meshesToDraw(*rightPart, ModelSource::Wheel) : std::vector<std::size_t>{};
    const WheelShape rightShape = rightPart ? measureWheel(*rightPart, rightMeshes) : leftShape;

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
            std::vector<std::string> textureFolders;
            if (ownRight) {
                textureFolders.push_back(rightFolder);
            }
            if (wheel) {
                textureFolders.push_back(wheelFolder);
            }
            builder.setWheelFolders(std::move(textureFolders));
            builder.beginNode(CarMesh::NodeKind::Wheel, corner, mount.centre);
            builder.addPart(ownRight ? rightFile : leftFile, part, ModelSource::Wheel,
                ownRight ? rightMeshes : leftMeshes, placement);
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
                builder.beginNode(kind, corner, mount.centre);
                for (const CarBin::Part& part : brake->parts) {
                    builder.addPart(file, part, ModelSource::Brake, meshesToDraw(part, ModelSource::Brake), placement);
                }
            }
        }
        model.wheels.push_back(mount);
    }
    builder.finish(options.textures);
    return model;
}

} // namespace fh::carmodel
