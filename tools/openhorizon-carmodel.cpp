// Loads a car's render models as the engine does and reports parts, meshes,
// materials, textures, wheel mounts and bounds; optional OBJ export, --all.

#include "base/Log.h"
#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "carmodel/CarModel.h"
#include "data/Database.h"
#include "game/PostProcessing.h"
#include "game/TimeOfDay.h"
#include "platform/Window.h"
#include "render/CarRenderer.h"
#include "render/FrameRenderer.h"
#include "render/Gl.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
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

namespace carmodel = openhorizon::carmodel;
using carmodel::CarMesh;
using carmodel::CarModel;

int usage()
{
    std::fprintf(stderr,
        "usage: openhorizon-carmodel DISC CAR [--obj OUT.obj] [--no-textures] [--all-parts] [--in-race-lods]\n"
        "                            [--no-cockpit]\n"
        "                            [--render PREFIX [--time HH:MM]]\n"
        "       openhorizon-carmodel DISC --all [--textures]\n"
        "  DISC         the extracted disc (its root or its media folder)\n"
        "  CAR          a folder of media/cars, such as alf_8c_08\n"
        "  --obj        also write OUT.obj, OUT.mtl and PNG textures beside them;\n"
        "               right-handed, Y up, the car's front towards -Z\n"
        "  --no-textures  do not decode textures\n"
        "  --all-parts  include upgrade variants (race kits, aftermarket parts)\n"
        "  --in-race-lods  use <car>.carbin (LOD 1) instead of <car>_lod0.carbin\n"
        "  --no-cockpit leave out <car>_cockpit.carbin\n"
        "  --render     draw the car offscreen from six sides under colorado's\n"
        "               light at --time (default 13:00) into PREFIX_<view>.png\n"
        "  --all        load every drivable car and report which load\n");
    return 2;
}

const char* formatName(openhorizon::world::TextureSurface::Format format)
{
    switch (format) {
    case openhorizon::world::TextureSurface::Format::Dxt1:
        return "DXT1";
    case openhorizon::world::TextureSurface::Format::Dxt3:
        return "DXT3";
    case openhorizon::world::TextureSurface::Format::Dxt5:
        return "DXT5";
    case openhorizon::world::TextureSurface::Format::Bc5:
        return "BC5";
    case openhorizon::world::TextureSurface::Format::Rgba8:
        return "RGBA8";
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
std::optional<std::pair<glm::vec3, glm::vec3>> maxDataBox(const openhorizon::Vfs& vfs, const std::string& car)
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
    return std::pair{
        glm::vec3(box.attribute("MinX").as_float(), box.attribute("MinY").as_float(), box.attribute("MinZ").as_float()),
        glm::vec3(
            box.attribute("MaxX").as_float(), box.attribute("MaxY").as_float(), box.attribute("MaxZ").as_float())};
}

void report(const openhorizon::Vfs& vfs, const CarModel& model)
{
    std::printf("car %s, wheel %s\n", model.mediaName.c_str(), model.wheelMediaName.c_str());
    std::printf("files:\n");
    for (const std::string& file : model.files) {
        std::printf("  %s\n", file.c_str());
    }

    std::printf("parts (%zu):\n", model.parts.size());
    std::printf("  %-4s %-9s %-24s %-8s %-4s %8s %8s  %s\n", "#", "source", "name", "variant", "used", "verts", "tris",
        "car-space box");
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
        std::vector<std::string> textures;
        bool translucent = false;
        bool blur = false;
        std::uint32_t batches = 0;
        std::uint32_t triangles = 0;
    };
    std::map<std::string, MaterialUse> materials;
    for (const CarMesh::Batch& batch : mesh.batches) {
        MaterialUse& use = materials[batch.material + " -> " + batch.technique];
        use.textures.clear();
        for (const auto& [sampler, texture] : batch.textures) {
            use.textures.push_back(std::format("{}={}", sampler, model.textures[texture].path));
        }
        use.translucent = batch.translucent;
        use.blur = batch.motionBlur;
        ++use.batches;
        use.triangles += batch.indexCount / 3;
    }
    std::printf("materials (%zu):\n", materials.size());
    for (const auto& [name, use] : materials) {
        std::string passes = "no effect";
        if (model.effect) {
            const std::size_t count = model.effect->passes(name.substr(name.find(" -> ") + 4)).size();
            passes = count > 0 ? std::format("{} pass{}", count, count > 1 ? "es" : "") : "NO TECHNIQUE";
        }
        std::string textures;
        for (const std::string& texture : use.textures) {
            textures += " " + texture;
        }
        std::printf("  %-44s %-12s %-11s %4u batches %8u tris %s\n", name.c_str(), passes.c_str(),
            use.translucent ? "translucent" : (use.blur ? "motion-blur" : "opaque"), use.batches, use.triangles,
            textures.c_str());
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
    std::printf(
        "bounds (without blurred wheels): %s .. %s\n", vec(mesh.boundsMin).c_str(), vec(mesh.boundsMax).c_str());
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

/// Writes OBJ, one group per batch, Z negated (front towards -Z); mirrored
/// placements are rewound.
void writeObj(const CarModel& model, const std::filesystem::path& path)
{
    const CarMesh& mesh = model.mesh;
    std::filesystem::path mtlPath = path;
    mtlPath.replace_extension(".mtl");

    std::ofstream mtl(mtlPath);
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        const CarMesh::Batch& batch = mesh.batches[b];
        mtl << std::format("newmtl m{}_{}\n", b, batch.material);
        for (const auto& [sampler, texture] : batch.textures) {
            if (sampler == "baseSampler" && !model.textures[texture].chain.levels.empty()) {
                mtl << std::format("map_Kd {}\n", pngName(path, texture));
            }
        }
        mtl << "\n";
    }

    std::ofstream obj(path);
    obj << std::format("# {} (FH1 carbin), right-handed, Y up, front towards -Z\n", model.mediaName);
    obj << std::format("mtllib {}\n", mtlPath.filename().string());
    std::size_t written = 0;
    for (std::size_t b = 0; b < mesh.batches.size(); ++b) {
        const CarMesh::Batch& batch = mesh.batches[b];
        const CarMesh::Node& node = mesh.nodes[batch.node];
        const CarMesh::VertexRange& range = mesh.ranges[batch.range];
        obj << std::format("g {}_{}_{}\n", nodeName(node.kind),
            node.corner ? std::string(carmodel::cornerCode(*node.corner)) : std::string("car"), batch.material);
        obj << std::format("usemtl m{}_{}\n", b, batch.material);
        const std::size_t first = written + 1;
        for (std::uint32_t v = 0; v < range.count; ++v) {
            const glm::vec3 p = mesh.position(batch, v);
            const std::uint8_t* raw
                = mesh.vertices.data() + range.byteOffset + std::size_t{v} * carmodel::carbin::kVertexStride;
            const float u = static_cast<float>(openhorizon::loadBe16(raw + 8)) / 65535.0F;
            const float t = static_cast<float>(openhorizon::loadBe16(raw + 10)) / 65535.0F;
            obj << std::format("v {:.5f} {:.5f} {:.5f}\nvt {:.5f} {:.5f}\n", p.x, p.y, -p.z,
                batch.uv0.x + batch.uv0.y * u, 1.0F - (batch.uv0.z + batch.uv0.w * t));
        }
        written += range.count;
        const bool rewind = glm::determinant(glm::mat3(node.placement)) > 0.0F;
        for (std::uint32_t i = batch.firstIndex; i + 2 < batch.firstIndex + batch.indexCount; i += 3) {
            const std::size_t a = first + mesh.indices[i];
            std::size_t c = first + mesh.indices[i + 1];
            std::size_t d = first + mesh.indices[i + 2];
            if (rewind) {
                std::swap(c, d);
            }
            obj << std::format("f {0}/{0} {1}/{1} {2}/{2}\n", a, c, d);
        }
    }

    for (std::size_t t = 0; t < model.textures.size(); ++t) {
        const openhorizon::world::TextureMipChain& chain = model.textures[t].chain;
        if (chain.levels.empty()) {
            continue;
        }
        const openhorizon::world::RgbaImage image = openhorizon::world::decodeSurface(chain.level(0));
        xds::Image png;
        png.width = static_cast<std::uint32_t>(image.width);
        png.height = static_cast<std::uint32_t>(image.height);
        png.rgba = image.rgba;
        const std::vector<std::uint8_t> file = xds::writePng(png);
        std::ofstream out(path.parent_path() / pngName(path, t), std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    std::printf(
        "wrote %s, %s and %zu textures\n", path.string().c_str(), mtlPath.string().c_str(), model.textures.size());
}

int loadAll(const openhorizon::Vfs& vfs, const openhorizon::Database& db, bool textures)
{
    const std::vector<openhorizon::Row> cars
        = db.query("SELECT MediaName FROM Data_Car WHERE IsDrivable = 1 ORDER BY MediaName");
    carmodel::CarModelOptions options;
    options.textures = textures;
    int loaded = 0;
    int withWarnings = 0;
    std::size_t triangles = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const openhorizon::Row& row : cars) {
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
                used, model.parts.size(), model.mesh.vertexCount(), model.mesh.triangleCount(), model.textures.size(),
                model.warnings.size());
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

/// Factory colour `sequence` of the car, linear RGB, as the game picks it
/// (Combo_Colors, RGB packed 0xRRGGBB in sRGB).
std::optional<std::pair<glm::vec3, bool>> factoryPaint(const openhorizon::Database& db, std::string_view mediaName)
{
    const std::optional<openhorizon::Row> car
        = db.queryOne("SELECT Id FROM Data_Car WHERE lower(MediaName) = lower(?)", {std::string(mediaName)});
    if (!car) {
        return std::nullopt;
    }
    const std::optional<openhorizon::Row> paint
        = db.queryOne("SELECT RGB, Metallic FROM Combo_Colors WHERE Ordinal = ? ORDER BY Sequence LIMIT 1",
            {std::to_string(car->i("Id"))});
    if (!paint) {
        return std::nullopt;
    }
    const auto rgb = static_cast<std::uint32_t>(paint->integer("RGB"));
    const auto linear
        = [](std::uint32_t channel) { return std::pow(static_cast<float>(channel & 0xFFU) / 255.0F, 2.2F); };
    return std::pair{glm::vec3(linear(rgb >> 16), linear(rgb >> 8), linear(rgb)), paint->i("Metallic") != 0};
}

bool savePng(const std::filesystem::path& path, int width, int height)
{
    xds::Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height / 2; ++y) {
        std::swap_ranges(image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row),
            image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y + 1) * row),
            image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(height - 1 - y) * row));
    }
    const std::vector<std::uint8_t> png = xds::writePng(image);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

/// Draws the car standing at the world origin, facing north, from six
/// directions, lit by colorado's time of day, and saves each view.
int renderViews(const openhorizon::Vfs& vfs, const openhorizon::Database* db, const CarModel& model,
    const std::string& prefix, float minutes)
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    openhorizon::WindowOptions windowOptions;
    windowOptions.title = "openhorizon-carmodel";
    windowOptions.width = kWidth;
    windowOptions.height = kHeight;
    windowOptions.vsync = false;
    windowOptions.hidden = true;
    openhorizon::Window window(windowOptions);

    openhorizon::TimeOfDay timeOfDay(openhorizon::TimeOfDayCurves::load(vfs, "colorado"));
    timeOfDay.setSeconds(minutes * 60.0F);
    // Colorado's post-processing outside every zone at that time, with the
    // exposure adapted to this one frame.
    openhorizon::PostProcessing post = openhorizon::PostProcessing::load(vfs, "colorado");
    post.update(std::nullopt, 0.0F, timeOfDay.curves(), timeOfDay.minutes());
    timeOfDay.setFog(post.fog());
    const openhorizon::render::PostFrame postFrame
        = post.frame(static_cast<float>(kWidth) / static_cast<float>(kHeight), true);

    openhorizon::render::FrameRenderer frameRenderer;
    openhorizon::render::CarRenderer car(model);
    openhorizon::render::CarPose pose;
    for (std::size_t w = 0; w < model.wheels.size() && w < pose.hubs.size(); ++w) {
        pose.hubs[w] = model.wheels[w].centre;
    }
    pose.headLights = timeOfDay.lights();
    if (db) {
        if (const auto paint = factoryPaint(*db, model.mediaName)) {
            pose.paint = paint->first;
            pose.metallic = paint->second;
        }
        // Settled on flat ground so body and wheels sit at ride height, as
        // Game::carPose poses it.
        if (const std::optional<openhorizon::Row> row
            = db->queryOne("SELECT Id FROM Data_Car WHERE lower(MediaName) = lower(?)", {model.mediaName})) {
            const openhorizon::CarSpec spec = openhorizon::loadCarSpec(*db, row->i("Id"), &vfs);
            openhorizon::CarPhysics physics(spec);
            const openhorizon::FlatGround ground;
            physics.reset(glm::vec3(0.0F), 0.0F, ground);
            constexpr int kSettleSteps = 360 * 4;
            openhorizon::CarControls controls;
            controls.handbrake = 1.0F;
            for (int i = 0; i < kSettleSteps; ++i) {
                physics.step(controls, ground);
            }
            pose.body = physics.transform();
            const glm::mat4 toCar = glm::inverse(pose.body);
            for (std::size_t w = 0; w < openhorizon::CarPhysics::kWheels; ++w) {
                const openhorizon::CarPhysics::WheelState& wheel = physics.wheels()[w];
                pose.hubs[w] = glm::vec3(toCar * glm::vec4(wheel.hubWorld, 1.0F));
                pose.steer[w] = wheel.steerAngle;
                pose.spin[w] = wheel.spinAngle;
            }
        }
    }

    const glm::vec3 centre
        = glm::vec3(pose.body * glm::vec4((model.mesh.boundsMin + model.mesh.boundsMax) * 0.5F, 1.0F));
    struct View {
        const char* name;
        glm::vec3 direction;
    };
    const std::array<View, 6> views{View{"front", {0.0F, 0.15F, 1.0F}}, View{"side", {-1.0F, 0.1F, 0.0F}},
        View{"rear", {0.0F, 0.15F, -1.0F}}, View{"front34", {-0.8F, 0.3F, 0.8F}}, View{"rear34", {0.8F, 0.3F, -0.8F}},
        View{"top", {0.0F, 1.0F, 0.05F}}};
    for (const View& view : views) {
        openhorizon::render::Camera camera;
        camera.fov = 0.36F;
        camera.target = centre;
        camera.position = centre + glm::normalize(view.direction) * 8.5F;
        openhorizon::render::FrameParams frame;
        frame.view = camera.view();
        frame.projection = camera.projection(static_cast<float>(kWidth) / static_cast<float>(kHeight));
        frame.cameraPosition = camera.position;
        frame.lighting = timeOfDay.lighting();
        frameRenderer.begin(kWidth, kHeight);
        car.draw(frame, pose, false);
        frameRenderer.sky(frame);
        car.draw(frame, pose, true);
        frameRenderer.finish(postFrame);
        const std::string path = std::format("{}_{}.png", prefix, view.name);
        if (!savePng(path, kWidth, kHeight)) {
            std::fprintf(stderr, "openhorizon-carmodel: cannot write %s\n", path.c_str());
            return 1;
        }
        std::printf("wrote %s\n", path.c_str());
        window.swapBuffers();
    }
    return 0;
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
    bool inRaceLods = false;
    bool noCockpit = false;
    std::optional<std::string> renderPrefix;
    float renderMinutes = 13.0F * 60.0F;
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
        } else if (arg == "--in-race-lods") {
            inRaceLods = true;
        } else if (arg == "--no-cockpit") {
            noCockpit = true;
        } else if (arg == "--render" && i + 1 < argc && target != "--all") {
            renderPrefix = argv[++i];
        } else if (arg == "--time" && i + 1 < argc) {
            int hours = 0;
            int mins = 0;
            if (std::sscanf(argv[++i], "%d:%d", &hours, &mins) != 2) {
                return usage();
            }
            renderMinutes = static_cast<float>(hours * 60 + mins);
        } else {
            return usage();
        }
    }
    try {
        openhorizon::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::optional<openhorizon::Bytes> image = vfs.read("media/db/gamedb.slt");
        std::optional<openhorizon::Database> db;
        if (image) {
            db.emplace(*image);
        }
        if (target == "--all") {
            if (!db) {
                std::fprintf(stderr, "openhorizon-carmodel: media/db/gamedb.slt is missing\n");
                return 1;
            }
            return loadAll(vfs, *db, textures);
        }
        carmodel::CarModelOptions options;
        options.textures = textures || objPath.has_value() || renderPrefix.has_value();
        options.stockOnly = !allParts;
        options.inRaceLods = inRaceLods;
        options.cockpit = !noCockpit;
        const CarModel model = CarModel::load(vfs, db ? &*db : nullptr, target, options);
        report(vfs, model);
        if (objPath) {
            writeObj(model, *objPath);
        }
        if (renderPrefix) {
            return renderViews(vfs, db ? &*db : nullptr, model, *renderPrefix, renderMinutes);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "openhorizon-carmodel: %s\n", e.what());
        return 1;
    }
    return 0;
}
