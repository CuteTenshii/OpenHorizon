// Frame-time checks: free roam driven along the roads with traffic, offscreen
// at a player's window size, timed per part of the frame through the game's
// own FrameStats. Driving the roads matters: it discovers roads, which
// recolours the minimap's road network, and it streams the world and the
// traffic in. The test needs the disc and a GPU, and its timings depend on
// the machine and on what else runs on it, so it carries the ctest label
// "perf" and stays out of the pre-push run and of the scenarios run
// without a filter.
//
// It doubles as the profiling tool: with OPENHORIZON_PERF_SIZE=WxH and
// OPENHORIZON_PERF_SECONDS it drives at another size or for longer, and with
// OPENHORIZON_PERF_SERIALISE=1 it makes the CPU wait for the GPU at the end of each
// part, which shows how much GPU time each part needs beyond sending its
// commands (the per-part GPU timestamps alone also count the GPU's idle
// time while the CPU is still sending them).

#include "scenarios/RoadFollower.h"
#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>

#include "car/CarPhysics.h"
#include "game/Game.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/FrameStats.h"
#include "ui/Navigation.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace openhorizon::test {
namespace {

using Part = openhorizon::render::FrameStats::Part;
using PartTimes = openhorizon::render::FrameStats::PartTimes;

/// Width and height from OPENHORIZON_PERF_SIZE ("1920x1080"), or `fallback`.
std::array<int, 2> perfSize(std::array<int, 2> fallback)
{
    const char* text = std::getenv("OPENHORIZON_PERF_SIZE");
    std::array<int, 2> size{};
    if (text != nullptr && std::sscanf(text, "%dx%d", &size[0], &size[1]) == 2 && size[0] > 0 && size[1] > 0) {
        return size;
    }
    return fallback;
}

float perfSeconds(float fallback)
{
    const char* text = std::getenv("OPENHORIZON_PERF_SECONDS");
    return text != nullptr && std::atof(text) > 0.0 ? static_cast<float>(std::atof(text)) : fallback;
}

bool perfSerialised()
{
    const char* text = std::getenv("OPENHORIZON_PERF_SERIALISE");
    return text != nullptr && *text != '\0' && *text != '0';
}

/// Per-part timings of a drive.
struct Drive {
    std::size_t frames = 0;
    PartTimes cpu{};
    PartTimes gpu{};
    PartTimes wait{};
    /// The slowest frame's CPU time of each part.
    PartTimes worst{};
    /// CPU drawing time of each frame, shadows to menus.
    std::vector<double> drawing;
    float metres = 0.0F;
};

double drawingSeconds(const PartTimes& parts)
{
    double total = 0.0;
    for (std::size_t part = static_cast<std::size_t>(Part::DrawShadows); part < static_cast<std::size_t>(Part::Present);
        ++part) {
        total += parts[part];
    }
    return total;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1))];
}

void report(const Drive& drive, int width, int height)
{
    const double frames = static_cast<double>(std::max<std::size_t>(drive.frames, 1));
    std::fprintf(
        stderr, "%dx%d: %zu frames, %.0f m driven\n", width, height, drive.frames, static_cast<double>(drive.metres));
    std::fprintf(stderr, "  %-14s %8s %8s %8s %8s\n", "part", "CPU ms", "worst", "GPU ms", "wait ms");
    for (std::size_t part = 0; part < openhorizon::render::FrameStats::kParts; ++part) {
        std::fprintf(stderr, "  %-14s %8.2f %8.2f %8.2f %8.2f\n",
            openhorizon::render::FrameStats::partName(static_cast<Part>(part)), drive.cpu[part] * 1000.0 / frames,
            drive.worst[part] * 1000.0, drive.gpu[part] * 1000.0 / frames, drive.wait[part] * 1000.0 / frames);
    }
    double cpuTotal = 0.0;
    for (const double seconds : drive.cpu) {
        cpuTotal += seconds;
    }
    std::fprintf(stderr, "  frame %.2f ms (%.0f fps), drawing mean %.2f, median %.2f, 95th %.2f, worst %.2f ms\n",
        cpuTotal * 1000.0 / frames, frames / cpuTotal, drawingSeconds(drive.cpu) * 1000.0 / frames,
        percentile(drive.drawing, 0.5) * 1000.0, percentile(drive.drawing, 0.95) * 1000.0,
        percentile(drive.drawing, 1.0) * 1000.0);
}

/// Loads free roam at `width` x `height`, lets it settle and drives the
/// roads for `seconds` of game time, timing every frame after the settling.
std::optional<Drive> drive(int width, int height, float seconds)
{
    window().setSize(width, height);
    Session session({});
    openhorizon::Game& game = session.game();
    game.frameStats().setSerialised(perfSerialised());
    if (!session.waitForCar() || game.navigation() == nullptr) {
        std::fprintf(stderr, "free roam did not start\n");
        return std::nullopt;
    }
    RoadFollower follower(*game.navigation());
    const auto step = [&](bool timed, Drive& result) {
        const glm::vec3 before = game.playerCar().position();
        if (!game.frame(follower.controls(game.playerCar()), kStep)) {
            return false;
        }
        if (timed) {
            const openhorizon::render::FrameStats& stats = game.frameStats();
            ++result.frames;
            for (std::size_t part = 0; part < openhorizon::render::FrameStats::kParts; ++part) {
                result.cpu[part] += stats.frameSeconds()[part];
                result.gpu[part] += stats.gpuFrameSeconds()[part];
                result.wait[part] += stats.waitSeconds()[part];
                result.worst[part] = std::max(result.worst[part], stats.frameSeconds()[part]);
            }
            result.drawing.push_back(drawingSeconds(stats.frameSeconds()));
            result.metres += glm::length(game.playerCar().position() - before);
        }
        return true;
    };
    Drive result;
    // Up to speed, with the traffic and the tiles around the start loaded.
    for (float t = 0.0F; t < 5.0F; t += kStep) {
        if (!step(false, result)) {
            return std::nullopt;
        }
    }
    std::fprintf(stderr, "timing %.0f s of driving\n", static_cast<double>(seconds));
    for (float t = 0.0F; t < seconds; t += kStep) {
        if (!step(true, result)) {
            return std::nullopt;
        }
    }
    report(result, width, height);
    return result;
}

// The budget for the CPU's drawing of a frame (world, cars, sky, glass,
// effects, tonemap, HUD, menus) while driving free roam at 1600x900, the
// player's window when the frame rate was reported: the median frame's
// drawing has to fit in a 60 Hz frame (16.7 ms), rounded down. After the
// fixes it takes 8 to 11 ms on an RTX 3060 Ti with other builds running;
// before them it took 117 ms (the HUD re-evaluating its whole scene for
// every attribute its text stackers read, and every traffic car drawn
// whether in view or not). The median rather than a percentile is checked
// because tile builds, effect links and traffic cars loading make single
// frames slower, and a machine shared with builds makes the tail noisy.
constexpr double kDrawingBudgetMs = 16.0;

TEST_CASE_METHOD(GameScenario, "freeRoamDrawingStaysInBudget", "[scenario][perf][.]")
{
    const std::array<int, 2> size = perfSize({1600, 900});
    const std::optional<Drive> result = drive(size[0], size[1], perfSeconds(30.0F));
    window().setSize(640, 360);
    CHECK(result.has_value());
    if (!result) {
        return;
    }
    // The drive has to be one: moving along the roads, discovering them.
    CHECK(result->metres > 200.0F);
    if (!perfSerialised()) {
        const double drawingMedian = percentile(result->drawing, 0.5) * 1000.0;
        CHECK(drawingMedian <= kDrawingBudgetMs);
    }
}

// Each traffic spawn reloads its model and effects; their passes still link once.
TEST_CASE_METHOD(GameScenario, "carProgramsAreLinkedOnceHoweverOftenAModelLoads", "[scenario][memory]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::render::CarRenderer first(openhorizon::carmodel::CarModel::load(vfs, nullptr, "ALF_8C_08"));
    const std::size_t linked = openhorizon::render::CarRenderer::programsLinked();
    CHECK(linked > 0);
    for (int spawn = 0; spawn < 3; ++spawn) {
        const openhorizon::render::CarRenderer again(openhorizon::carmodel::CarModel::load(vfs, nullptr, "ALF_8C_08"));
        CHECK(openhorizon::render::CarRenderer::programsLinked() == linked);
    }
}

} // namespace
} // namespace openhorizon::test
