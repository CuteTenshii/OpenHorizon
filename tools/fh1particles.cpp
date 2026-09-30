// Inspects the game's particle effects: the effects a track loads, one
// effect's parameters, the tyre effect weights of each surface, and renders
// of an effect or of the tyre effects of a spinning car, drawn offscreen
// under colorado's time of day.

#include "base/Log.h"
#include "game/PostProcessing.h"
#include "game/TimeOfDay.h"
#include "particles/Effect.h"
#include "particles/ParticleManager.h"
#include "particles/ParticleRenderer.h"
#include "particles/SurfaceEffects.h"
#include "particles/TyreEffects.h"
#include "platform/Window.h"
#include "render/FrameRenderer.h"
#include "render/Gl.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"

#include <glm/geometric.hpp>

#include <xds/xds.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace particles = openhorizon::particles;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1particles DISC list\n"
        "       fh1particles DISC show EFFECT\n"
        "       fh1particles DISC surfaces\n"
        "       fh1particles DISC render SYSTEM OUT.png [--seconds S] [--time HH:MM]\n"
        "       fh1particles DISC tyres SURFACE OUT.png [--seconds S] [--speed M/S] [--slide X] [--time HH:MM]\n"
        "  list      the systems of DefaultTrack.xml and their effects\n"
        "  show      every parameter of one effect file (name without .xml)\n"
        "  surfaces  the tyre effect weights of each surface type\n"
        "  render    emits SYSTEM continuously from the origin for S seconds\n"
        "            (default 2) at 30 frames a second, then draws it\n"
        "  tyres     runs the game's tyre effect rules for a car with its rear\n"
        "            wheels spinning on SURFACE (a surfaceTypes.xml name) for\n"
        "            S seconds and draws the result. The wheel inputs are\n"
        "            test values: tread speed 20 m/s, friction circle 2, full\n"
        "            load, slide X (default 1.5), car speed M/S (default 1)\n");
    return 2;
}

const char* shapeName(particles::EmitterShape s)
{
    switch (s) {
    case particles::EmitterShape::Box:
        return "Box";
    case particles::EmitterShape::Plane:
        return "Plane";
    case particles::EmitterShape::SuperSpray:
        return "SuperSpray";
    }
    return "?";
}

const char* typeName(particles::ParticleType t)
{
    switch (t) {
    case particles::ParticleType::Quad:
        return "Quad";
    case particles::ParticleType::BillboardedQuad:
        return "Billboard";
    case particles::ParticleType::Modeled:
        return "Modeled";
    }
    return "?";
}

int list(const openhorizon::Vfs& vfs)
{
    const particles::ParticleManager manager = particles::ParticleManager::load(vfs);
    std::printf("%-40s %-28s %-10s %-9s %6s %8s %6s %-8s %s\n", "system", "effect", "shape", "type", "budget", "qty/s",
        "life", "blend", "texture");
    for (const particles::ParticleManager::System& s : manager.systems()) {
        const particles::EffectDefinition& d = s.system->definition();
        std::printf("%-40s %-28s %-10s %-9s %6u %8.1f %6.2f %-8s %s%s%s\n", s.name.c_str(), d.name.c_str(),
            shapeName(d.shape), typeName(d.render.type), d.budget, static_cast<double>(d.quantity.value),
            static_cast<double>(d.life.value),
            d.render.material.blend == particles::BlendType::Additive ? "additive" : "alpha", d.render.texture.c_str(),
            d.gradient() ? " + " : "", d.gradient() ? d.render.texture2.c_str() : "");
    }
    for (const std::string& s : manager.skipped()) {
        std::printf("skipped %s\n", s.c_str());
    }
    std::printf("%zu systems\n", manager.systems().size());
    return 0;
}

int show(const openhorizon::Vfs& vfs, const std::string& name)
{
    const std::string path = particles::effectPath(name);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        std::fprintf(stderr, "fh1particles: %s not found\n", path.c_str());
        return 1;
    }
    if (particles::isCompositeName(name)) {
        const particles::CompositeDefinition c = particles::parseComposite(*text, name);
        std::printf("composite %s, repeat every %.2f s\n", c.name.c_str(), static_cast<double>(c.repeatPeriod));
        for (const particles::CompositeMember& m : c.members) {
            std::printf("  %s (%s) at (%.2f %.2f %.2f), %.2f to %.2f s\n", m.emitterName.c_str(), m.fileName.c_str(),
                static_cast<double>(m.positionOffset.x), static_cast<double>(m.positionOffset.y),
                static_cast<double>(m.positionOffset.z), static_cast<double>(m.startOffset),
                static_cast<double>(m.finishOffset));
        }
        return 0;
    }
    const particles::EffectDefinition d = particles::parseEffect(*text, name);
    const auto f = [](float v) { return static_cast<double>(v); };
    std::printf("%s: version %d, budget %u (lower %u), shader %s\n", d.name.c_str(), d.version, d.budget, d.lowerBudget,
        d.shaderName().c_str());
    std::printf("  emitter %s, quantity %.3f /s var %.3f, velocity %.3f var %.3f, life %.3f s var %.3f\n",
        shapeName(d.shape), f(d.quantity.value), f(d.quantity.variation), f(d.velocity.value), f(d.velocity.variation),
        f(d.life.value), f(d.life.variation));
    std::printf("  off axis %.4f rad var %.4f, off plane %.4f rad var %.4f, box %.3f %.3f %.3f\n", f(d.offAxis.value),
        f(d.offAxis.variation), f(d.offPlane.value), f(d.offPlane.variation), f(d.size.box.x), f(d.size.box.y),
        f(d.size.box.z));
    std::printf("  size value %.3f %.3f var %.3f %.3f max %.3f %.3f end %.3f %.3f grow %.3f %.3f from %.3f shrink "
                "%.3f %.3f to %.3f\n",
        f(d.size.value.x), f(d.size.value.y), f(d.size.variation.x), f(d.size.variation.y), f(d.size.maxValue.x),
        f(d.size.maxValue.y), f(d.size.endValue.x), f(d.size.endValue.y), f(d.size.growFor.x), f(d.size.growFor.y),
        f(d.size.growFrom), f(d.size.shrinkFor.x), f(d.size.shrinkFor.y), f(d.size.shrinkTo));
    std::printf("  spin time %.3f var %.3f, phase %.3f rad var %.3f, axis %d alternate %d\n", f(d.spinTime.value),
        f(d.spinTime.variation), f(d.spinPhase.value), f(d.spinPhase.variation), static_cast<int>(d.spinAxis),
        d.spinAlternate ? 1 : 0);
    std::printf("  density %.3f, animation rate %.3f frames %dx%d, gradient rows %d\n", f(d.density),
        f(d.animationRate), d.framesX, d.framesY, d.gradientRows);
    std::printf("  motion inheritance %d%% x %.3f var %.3f offset %.3f\n", d.motionInheritance.influence,
        f(d.motionInheritance.multiplier), f(d.motionInheritance.variation), f(d.motionInheritance.offset));
    for (const particles::Behaviour& b : d.behaviours) {
        switch (b.kind) {
        case particles::Behaviour::Kind::Gravity:
            std::printf("  gravity %.3f\n", f(b.gravity.strength));
            break;
        case particles::Behaviour::Kind::Friction:
            std::printf("  friction %.3f from age %.3f down to %.3f m/s\n", f(b.friction.strength),
                f(b.friction.startFrom), f(b.friction.velocityKept));
            break;
        case particles::Behaviour::Kind::Wind:
            std::printf("  wind %.3f (not simulated)\n", f(b.wind.strength));
            break;
        case particles::Behaviour::Kind::BubbleMotion:
            std::printf("  bubble amplitude %.3f period %.3f phase %.3f\n", f(b.bubble.amplitude.value),
                f(b.bubble.period.value), f(b.bubble.phase.value));
            break;
        case particles::Behaviour::Kind::WheelAttractor:
            std::printf("  wheel attractor (not simulated)\n");
            break;
        case particles::Behaviour::Kind::Collision:
            std::printf("  collision plane (not simulated)\n");
            break;
        }
    }
    const particles::RenderSettings& r = d.render;
    const particles::Material& m = r.material;
    std::printf("  render %s, lit %d, zsort %d, max screen size %.3f, texture %s, texture2 %s, %s\n", typeName(r.type),
        r.dynamicLighting ? 1 : 0, r.zSort ? 1 : 0, f(r.maxScreenSize), r.texture.c_str(), r.texture2.c_str(),
        m.blend == particles::BlendType::Additive ? "additive" : "alpha");
    std::printf("  light influence %.3f %.3f %.3f %.3f, specular %.3f %.3f %.3f, head light %.3f\n",
        f(r.lightInfluence.x), f(r.lightInfluence.y), f(r.lightInfluence.z), f(r.lightInfluence.w),
        f(r.specularLightInfluence.x), f(r.specularLightInfluence.y), f(r.specularLightInfluence.z),
        f(r.headLightPower));
    std::printf("  colour (squared) start %.3f %.3f %.3f %.3f mid %.3f %.3f %.3f %.3f end %.3f %.3f %.3f %.3f, fade in "
                "to %.3f out from %.3f\n",
        f(m.rgbStart.r), f(m.rgbStart.g), f(m.rgbStart.b), f(m.rgbStart.a), f(m.rgbMid.r), f(m.rgbMid.g), f(m.rgbMid.b),
        f(m.rgbMid.a), f(m.rgbEnd.r), f(m.rgbEnd.g), f(m.rgbEnd.b), f(m.rgbEnd.a), f(m.fadeInTo), f(m.fadeOutFrom));
    return 0;
}

int surfaces(const openhorizon::Vfs& vfs)
{
    const particles::SurfaceEffectTable table = particles::SurfaceEffectTable::load(vfs);
    for (std::size_t id = 0; id < table.surfaces().size(); ++id) {
        const particles::SurfaceEffects& s = table.surfaces()[id];
        std::printf("%3zu %-26s min %.2f", id, s.surfaceName.c_str(), static_cast<double>(s.minIntensity));
        for (std::size_t c = 0; c < particles::kTyreChannelCount; ++c) {
            if (s.weights[c] != 0.0F) {
                std::printf(" %s %.2f",
                    std::string(particles::tyreChannelName(static_cast<particles::TyreChannel>(c))).c_str(),
                    static_cast<double>(s.weights[c]));
            }
        }
        std::printf("\n");
    }
    return 0;
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

struct Options {
    float seconds = 2.0F;
    float minutes = 13.0F * 60.0F;
    float speed = 1.0F;
    float slide = 1.5F;
};

/// Draws the manager's particles from `eye` towards `target` under
/// colorado's lighting at the options' time and saves the frame.
int drawScene(const openhorizon::Vfs& vfs, const particles::ParticleManager& manager,
    particles::ParticleRenderer& renderer, const glm::vec3& eye, const glm::vec3& target, const Options& options,
    const std::string& out)
{
    constexpr int kWidth = 960;
    constexpr int kHeight = 540;
    openhorizon::TimeOfDay timeOfDay(openhorizon::TimeOfDayCurves::load(vfs, "colorado"));
    timeOfDay.setSeconds(options.minutes * 60.0F);
    // Colorado's post-processing outside every zone at that time, with the
    // exposure adapted to this one frame.
    openhorizon::PostProcessing post = openhorizon::PostProcessing::load(vfs, "colorado");
    post.update(std::nullopt, 0.0F, timeOfDay.curves(), timeOfDay.minutes());
    timeOfDay.setFog(post.fog());
    const openhorizon::render::PostFrame postFrame
        = post.frame(static_cast<float>(kWidth) / static_cast<float>(kHeight), true);

    openhorizon::render::FrameRenderer frameRenderer;
    openhorizon::render::Camera camera;
    camera.position = eye;
    camera.target = target;
    camera.fov = 0.7F;
    openhorizon::render::FrameParams frame;
    frame.view = camera.view();
    frame.projection = camera.projection(static_cast<float>(kWidth) / static_cast<float>(kHeight));
    frame.cameraPosition = camera.position;
    frame.lighting = timeOfDay.lighting();

    particles::ParticleFrame pf;
    pf.viewProjection = frame.projection * frame.view;
    pf.cameraPosition = camera.position;
    pf.viewForward = glm::normalize(target - eye);
    pf.nearPlane = camera.nearPlane;
    pf.towardsSun = -frame.lighting.sunDirection;
    pf.sunColour = frame.lighting.sunColour;
    pf.ambientColour = frame.lighting.ambientColour;

    frameRenderer.begin(kWidth, kHeight);
    frameRenderer.sky(frame);
    renderer.draw(manager, pf);
    frameRenderer.finish(postFrame);
    if (!savePng(out, kWidth, kHeight)) {
        std::fprintf(stderr, "fh1particles: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s: %zu particles drawn\n", out.c_str(), renderer.lastDrawn());
    for (const std::string& t : renderer.missingTextures()) {
        std::printf("missing texture %s\n", t.c_str());
    }
    return 0;
}

openhorizon::WindowOptions windowOptions()
{
    openhorizon::WindowOptions options;
    options.title = "fh1particles";
    options.width = 960;
    options.height = 540;
    options.vsync = false;
    options.hidden = true;
    return options;
}

int render(const openhorizon::Vfs& vfs, const std::string& systemName, const std::string& out, const Options& options)
{
    particles::ParticleManager manager = particles::ParticleManager::load(vfs);
    particles::ParticleSystem* system = manager.find(systemName);
    if (system == nullptr) {
        std::fprintf(stderr, "fh1particles: no system %s in DefaultTrack.xml\n", systemName.c_str());
        return 1;
    }
    constexpr float kDt = 1.0F / 30.0F;
    const int frames = static_cast<int>(options.seconds / kDt);
    manager.update(kDt);
    for (int i = 0; i < frames; ++i) {
        particles::EmitRequest request;
        request.position = glm::vec3(0.0F, 0.2F, 0.0F);
        request.direction = glm::vec3(0.0F, 0.0F, -1.0F);
        manager.emit(*system, request);
        manager.update(kDt);
    }
    std::printf("%s: %zu particles after %d frames\n", systemName.c_str(), system->size(), frames);
    const openhorizon::Window window(windowOptions());
    particles::ParticleRenderer renderer(vfs);
    return drawScene(vfs, manager, renderer, glm::vec3(3.0F, 1.5F, 3.0F), glm::vec3(0.0F, 0.6F, 0.0F), options, out);
}

int tyres(const openhorizon::Vfs& vfs, const std::string& surfaceName, const std::string& out, const Options& options)
{
    particles::ParticleManager manager = particles::ParticleManager::load(vfs);
    particles::SurfaceEffectTable table = particles::SurfaceEffectTable::load(vfs);
    const std::optional<std::size_t> surface = table.idOf(surfaceName);
    if (!surface) {
        std::fprintf(stderr, "fh1particles: no surface %s\n", surfaceName.c_str());
        return 1;
    }
    particles::TyreEffects effects(manager, std::move(table));

    // A car at the origin facing north (+z) with wheels 1.4 m apart and a
    // 2.6 m wheelbase, rear wheels spinning.
    particles::TyreCarInput car;
    car.velocity = glm::vec3(0.0F, 0.0F, options.speed);
    car.position = glm::vec3(0.0F);
    const std::array<glm::vec3, 4> hubs = {glm::vec3(-0.7F, 0.33F, 1.3F), glm::vec3(0.7F, 0.33F, 1.3F),
        glm::vec3(-0.7F, 0.33F, -1.3F), glm::vec3(0.7F, 0.33F, -1.3F)};
    for (std::size_t w = 0; w < 4; ++w) {
        particles::TyreWheelInput& wheel = car.wheels[w];
        wheel.onGround = true;
        wheel.surface = *surface;
        wheel.frame.axle = car.right;
        wheel.frame.row1 = car.up;
        wheel.frame.row2 = car.forward;
        wheel.frame.position = hubs[w];
        wheel.radius = 0.33F;
        wheel.normalisedLoad = 1.0F;
        wheel.wheelVelocity = car.velocity;
        const bool driven = w >= 2;
        wheel.treadSpeed = driven ? 20.0F : options.speed;
        wheel.wheelRpm = wheel.treadSpeed / wheel.radius * 9.549296F;
        wheel.slide = driven ? options.slide : 0.0F;
        wheel.frictionCircle = driven ? 2.0F : 0.0F;
    }

    constexpr float kDt = 1.0F / 30.0F;
    const int frames = static_cast<int>(options.seconds / kDt);
    manager.update(kDt);
    for (int i = 0; i < frames; ++i) {
        effects.emit(car);
        manager.update(kDt);
    }
    std::size_t total = 0;
    for (const particles::ParticleManager::System& s : manager.systems()) {
        if (s.system->size() > 0) {
            std::printf("  %-14s %zu particles\n", s.name.c_str(), s.system->size());
            total += s.system->size();
        }
    }
    std::printf("%s: %zu particles after %d frames\n", surfaceName.c_str(), total, frames);
    const openhorizon::Window window(windowOptions());
    particles::ParticleRenderer renderer(vfs);
    return drawScene(vfs, manager, renderer, glm::vec3(4.5F, 1.6F, -6.0F), glm::vec3(0.0F, 0.6F, -1.5F), options, out);
}

bool parseOptions(int argc, char** argv, int first, Options& options)
{
    for (int i = first; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) {
            return false;
        }
        if (arg == "--seconds") {
            options.seconds = std::stof(argv[++i]);
        } else if (arg == "--speed") {
            options.speed = std::stof(argv[++i]);
        } else if (arg == "--slide") {
            options.slide = std::stof(argv[++i]);
        } else if (arg == "--time") {
            int hours = 0;
            int mins = 0;
            if (std::sscanf(argv[++i], "%d:%d", &hours, &mins) != 2) {
                return false;
            }
            options.minutes = static_cast<float>(hours * 60 + mins);
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    try {
        openhorizon::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string_view command = argv[2];
        if (command == "list") {
            return list(vfs);
        }
        if (command == "surfaces") {
            return surfaces(vfs);
        }
        if (command == "show" && argc == 4) {
            return show(vfs, argv[3]);
        }
        Options options;
        if ((command == "render" || command == "tyres") && argc >= 5 && parseOptions(argc, argv, 5, options)) {
            return command == "render" ? render(vfs, argv[3], argv[4], options) : tyres(vfs, argv[3], argv[4], options);
        }
        return usage();
    } catch (const std::exception& e) {
        openhorizon::logError("{}", e.what());
        return 1;
    }
}
