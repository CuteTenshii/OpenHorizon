// Loads a car's render models the way the engine does and reports what it
// found: parts, meshes, materials, textures, wheel mounts and bounds, with an
// optional Wavefront OBJ export for inspection in any 3D viewer. With --all
// it loads every drivable car and reports how many load.

#include "base/Log.h"
#include "carmodel/CarModel.h"
#include "data/Database.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace carmodel = fh::carmodel;
using carmodel::CarMesh;
using carmodel::CarModel;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1carmodel DISC CAR [--obj OUT.obj] [--no-textures] [--all-parts]\n"
        "       fh1carmodel DISC --all [--textures]\n"
        "  DISC         the extracted disc (its root or its media folder)\n"
        "  CAR          a folder of media/cars, such as alf_8c_08\n"
        "  --obj        also write OUT.obj, OUT.mtl and PNG textures beside them;\n"
        "               right-handed, Y up, the car's front towards -Z\n"
        "  --no-textures  do not decode textures\n"
        "  --all-parts  include upgrade variants (race kits, aftermarket parts)\n"
        "  --all        load every drivable car and report which load\n");
    return 2;
}

const char* formatName(fh::world::TextureSurface::Format format)
{
    switch (format) {
    case fh::world::TextureSurface::Format::Dxt1:
        return "DXT1";
    case fh::world::TextureSurface::Format::Dxt3:
        return "DXT3";
    case fh::world::TextureSurface::Format::Dxt5:
        return "DXT5";
    case fh::world::TextureSurface::Format::Bc5:
        return "BC5";
    case fh::world::TextureSurface::Format::Rgba8:
        return "RGBA8";
    }
    return "?";
}

const char* shadingName(carmodel::Shading shading)
{
    switch (shading) {
    case carmodel::Shading::Plain:
        return "plain";
    case carmodel::Shading::Paint:
        return "paint";
    case carmodel::Shading::Glass:
        return "glass";
    case carmodel::Shading::Lights:
        return "lights";
    case carmodel::Shading::Metal:
        return "metal";
    case carmodel::Shading::Rubber:
        return "rubber";
    case carmodel::Shading::Interior:
        return "interior";
    case carmodel::Shading::Decal:
        return "decal";
    }
    return "?";
}

const char* sourceName(carmodel::ModelSource source)
{
    switch (source) {
    case carmodel::ModelSource::Exterior:
        return "exterior";
    case carmodel::ModelSource::Cockpit:
        return "cockpit";
    case carmodel::ModelSource::Wheel:
        return "wheel";
    case carmodel::ModelSource::Brake:
        return "brake";
    }
    return "?";
}

const char* nodeName(CarMesh::NodeKind kind)
{
    switch (kind) {
    case CarMesh::NodeKind::Body:
        return "body";
    case CarMesh::NodeKind::Cockpit:
        return "cockpit";
    case CarMesh::NodeKind::Wheel:
        return "wheel";
    case CarMesh::NodeKind::Rotor:
        return "rotor";
    case CarMesh::NodeKind::Caliper:
        return "caliper";
    }
    return "?";
}

std::string vec(const glm::vec3& v)
{
    return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z);
}

/// The collision box of `physics/maxdata.xml`, in car space.
std::optional<std::pair<glm::vec3, glm::vec3>> maxDataBox(const fh::Vfs& vfs, const std::string& car)
{
    const std::optional<std::string> text = vfs.readText(std::format("media/cars/{}/physics/maxdata.xml", car));
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    const pugi::xml_node box = doc.child("MAXData").child("Collision").child("BoundingBox");
    if (!box) {
        return std::nullopt;
    }
    return std::pair{glm::vec3(box.attribute("MinX").as_float(), box.attribute("MinY").as_float(),
                         box.attribute("MinZ").as_float()),
        glm::vec3(box.attribute("MaxX").as_float(), box.attribute("MaxY").as_float(),
            box.attribute("MaxZ").as_float())};
}

void report(const fh::Vfs& vfs, const CarModel& model)
{
    std::printf("car %s, wheel %s\n", model.mediaName.c_str(), model.wheelMediaName.c_str());
    std::printf("files:\n");
    for (const std::string& file : model.files) {
        std::printf("  %s\n", file.c_str());
    }

    std::printf("parts (%zu):\n", model.parts.size());
    std::printf("  %-4s %-9s %-24s %-8s %-4s %8s %8s  %s\n", "#", "source", "name", "variant", "used", "verts",
        "tris", "car-space box");
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        const CarModel::Part& part = model.parts[i];
        std::printf("  %-4zu %-9s %-24s %-8s %-4s %8u %8u  %s .. %s\n", i, sourceName(part.source), part.name.c_str(),
            part.variant.variant.empty() ? "-" : part.variant.variant.c_str(), part.used ? "yes" : "no",
            part.vertexCount, part.triangleCount, part.used ? vec(part.boundsMin).c_str() : "",
            part.used ? vec(part.boundsMax).c_str() : "");
    }

    const CarMesh& mesh = model.mesh;
    std::printf("nodes (%zu):\n", mesh.nodes.size());
    for (const CarMesh::Node& node : mesh.nodes) {
        std::uint32_t triangles = 0;
        for (std::uint32_t b = node.firstBatch; b < node.firstBatch + node.batchCount; ++b) {
            triangles += mesh.batches[b].indexCount / 3;
        }
        std::printf("  %-8s %-3s pivot %s  %u batches, %u triangles\n", nodeName(node.kind),
            node.corner ? std::string(carmodel::cornerCode(*node.corner)).c_str() : "-", vec(node.pivot).c_str(),
            node.batchCount, triangles);
    }

    struct MaterialUse {
        carmodel::Shading shading = carmodel::Shading::Plain;
        std::uint32_t texture = CarMesh::kNoTexture;
        bool translucent = false;
        bool blur = false;
        std::uint32_t batches = 0;
        std::uint32_t triangles = 0;
    };
    std::map<std::string, MaterialUse> materials;
    for (const CarMesh::Batch& batch : mesh.batches) {
        MaterialUse& use = materials[batch.material];
        use.shading = batch.shading;
        use.texture = batch.texture;
        use.translucent = batch.translucent;
        use.blur = batch.motionBlur;
        ++use.batches;
        use.triangles += batch.indexCount / 3;
    }
    std::printf("materials (%zu):\n", materials.size());
    for (const auto& [name, use] : materials) {
        const std::string texture
            = use.texture == CarMesh::kNoTexture ? std::string("-") : model.textures[use.texture].path;
        std::printf("  %-26s %-8s %-11s %4u batches %8u tris  %s\n", name.c_str(), shadingName(use.shading),
            use.translucent ? "translucent" : (use.blur ? "motion-blur" : "opaque"), use.batches, use.triangles,
            texture.c_str());
    }

    std::printf("textures (%zu):\n", model.textures.size());
    for (const CarModel::Texture& texture : model.textures) {
        std::printf("  %-52s %-5s %4dx%-4d %2zu levels\n", texture.path.c_str(), formatName(texture.chain.format),
            texture.chain.width, texture.chain.height, texture.chain.levels.size());
    }

    std::printf("wheels:\n");
    for (const carmodel::WheelMount& wheel : model.wheels) {
        std::printf("  %s centre %s radius %.3f width %.3f scale %.3f across, %.3f radial%s\n",
            std::string(carmodel::cornerCode(wheel.corner)).c_str(), vec(wheel.centre).c_str(), wheel.radius,
            wheel.width, wheel.axialScale, wheel.radialScale, wheel.mirrored ? ", mirrored" : "");
    }

    std::printf("mesh: %zu vertices, %zu triangles, %zu batches\n", mesh.vertexCount(), mesh.triangleCount(),
        mesh.batches.size());
    std::printf("bounds (without blurred wheels): %s .. %s\n", vec(mesh.boundsMin).c_str(), vec(mesh.boundsMax).c_str());
    if (const auto box = maxDataBox(vfs, model.mediaName)) {
        std::printf("maxdata.xml collision box:     %s .. %s\n", vec(box->first).c_str(), vec(box->second).c_str());
    }
    for (const std::string& warning : model.warnings) {
        std::printf("warning: %s\n", warning.c_str());
    }
}

std::string pngName(const std::filesystem::path& obj, std::size_t texture)
{
    return std::format("{}_tex{}.png", obj.stem().string(), texture);
}

/// Writes the mesh as OBJ with one group per batch. The OBJ is right-handed
/// with the front towards -Z (car space with Z negated), so the index order
/// of CarMesh, clockwise in car space, comes out counter-clockwise, as OBJ
/// viewers expect.
void writeObj(const CarModel& model, const std::filesystem::path& path)
{
    const CarMesh& mesh = model.mesh;
    std::filesystem::path mtlPath = path;
    mtlPath.replace_extension(".mtl");

    std::ofstream mtl(mtlPath);
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        const CarMesh::Batch& batch = mesh.batches[b];
        mtl << std::format("newmtl m{}_{}\n", b, batch.material);
        mtl << std::format("Kd {:.3f} {:.3f} {:.3f}\n", batch.colour.r, batch.colour.g, batch.colour.b);
        if (batch.translucent) {
            mtl << std::format("d {:.2f}\n", batch.colour.a);
        }
        if (batch.texture != CarMesh::kNoTexture && !model.textures[batch.texture].chain.levels.empty()) {
            mtl << std::format("map_Kd {}\n", pngName(path, batch.texture));
        }
        mtl << "\n";
    }

    std::ofstream obj(path);
    obj << std::format("# {} (FH1 carbin), right-handed, Y up, front towards -Z\n", model.mediaName);
    obj << std::format("mtllib {}\n", mtlPath.filename().string());
    const std::size_t count = mesh.vertexCount();
    for (std::size_t i = 0; i < count; ++i) {
        const float* v = mesh.vertices.data() + i * CarMesh::kFloatsPerVertex;
        obj << std::format("v {:.5f} {:.5f} {:.5f}\n", v[0], v[1], -v[2]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float* v = mesh.vertices.data() + i * CarMesh::kFloatsPerVertex;
        obj << std::format("vn {:.4f} {:.4f} {:.4f}\n", v[CarMesh::kNormal], v[CarMesh::kNormal + 1],
            -v[CarMesh::kNormal + 2]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float* v = mesh.vertices.data() + i * CarMesh::kFloatsPerVertex;
        obj << std::format("vt {:.5f} {:.5f}\n", v[CarMesh::kTexcoord], 1.0F - v[CarMesh::kTexcoord + 1]);
    }
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        const CarMesh::Batch& batch = mesh.batches[b];
        const CarMesh::Node& node = mesh.nodes[batch.node];
        obj << std::format("g {}_{}_{}\n", nodeName(node.kind),
            node.corner ? std::string(carmodel::cornerCode(*node.corner)) : std::string("car"), batch.material);
        obj << std::format("usemtl m{}_{}\n", b, batch.material);
        for (std::uint32_t i = batch.firstIndex; i + 2 < batch.firstIndex + batch.indexCount; i += 3) {
            const std::uint32_t a = mesh.indices[i] + 1;
            const std::uint32_t c = mesh.indices[i + 1] + 1;
            const std::uint32_t d = mesh.indices[i + 2] + 1;
            obj << std::format("f {0}/{0}/{0} {1}/{1}/{1} {2}/{2}/{2}\n", a, c, d);
        }
    }

    for (std::size_t t = 0; t < model.textures.size(); ++t) {
        const fh::world::TextureMipChain& chain = model.textures[t].chain;
        if (chain.levels.empty()) {
            continue;
        }
        const fh::world::RgbaImage image = fh::world::decodeSurface(chain.level(0));
        xds::Image png;
        png.width = static_cast<std::uint32_t>(image.width);
        png.height = static_cast<std::uint32_t>(image.height);
        png.rgba = image.rgba;
        const std::vector<std::uint8_t> file = xds::writePng(png);
        std::ofstream out(path.parent_path() / pngName(path, t), std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    std::printf("wrote %s, %s and %zu textures\n", path.string().c_str(), mtlPath.string().c_str(),
        model.textures.size());
}

int loadAll(const fh::Vfs& vfs, const fh::Database& db, bool textures)
{
    const std::vector<fh::Row> cars
        = db.query("SELECT MediaName FROM Data_Car WHERE IsDrivable = 1 ORDER BY MediaName");
    carmodel::CarModelOptions options;
    options.textures = textures;
    int loaded = 0;
    int withWarnings = 0;
    std::size_t triangles = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const fh::Row& row : cars) {
        const std::string name = row.text("MediaName");
        try {
            const CarModel model = CarModel::load(vfs, &db, name, options);
            ++loaded;
            triangles += model.mesh.triangleCount();
            withWarnings += model.warnings.empty() ? 0 : 1;
            std::size_t used = 0;
            for (const CarModel::Part& part : model.parts) {
                used += part.used ? 1 : 0;
            }
            std::printf("ok   %-28s %3zu/%-3zu parts %7zu verts %7zu tris %3zu textures %zu warnings\n", name.c_str(),
                used, model.parts.size(), model.mesh.vertexCount(), model.mesh.triangleCount(),
                model.textures.size(), model.warnings.size());
            for (const std::string& warning : model.warnings) {
                std::printf("       warning: %s\n", warning.c_str());
            }
        } catch (const std::exception& e) {
            std::printf("FAIL %-28s %s\n", name.c_str(), e.what());
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%d of %zu drivable cars loaded (%d with warnings), %zu triangles, %.1f s\n", loaded, cars.size(),
        withWarnings, triangles, seconds);
    return loaded == static_cast<int>(cars.size()) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    const std::string_view target = argv[2];
    std::optional<std::filesystem::path> objPath;
    bool textures = target != "--all";
    bool allParts = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--obj" && i + 1 < argc && target != "--all") {
            objPath = argv[++i];
        } else if (arg == "--no-textures") {
            textures = false;
        } else if (arg == "--textures") {
            textures = true;
        } else if (arg == "--all-parts") {
            allParts = true;
        } else {
            return usage();
        }
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::optional<fh::Bytes> image = vfs.read("media/db/gamedb.slt");
        std::optional<fh::Database> db;
        if (image) {
            db.emplace(*image);
        }
        if (target == "--all") {
            if (!db) {
                std::fprintf(stderr, "fh1carmodel: media/db/gamedb.slt is missing\n");
                return 1;
            }
            return loadAll(vfs, *db, textures);
        }
        carmodel::CarModelOptions options;
        options.textures = textures || objPath.has_value();
        options.stockOnly = !allParts;
        const CarModel model = CarModel::load(vfs, db ? &*db : nullptr, target, options);
        report(vfs, model);
        if (objPath) {
            writeObj(model, *objPath);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fh1carmodel: %s\n", e.what());
        return 1;
    }
    return 0;
}
