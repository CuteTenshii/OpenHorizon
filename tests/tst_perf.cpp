// Frame-time checks: free roam driven along the roads with traffic, offscreen
// at a player's window size, timed per part of the frame through the game's
// own FrameStats. Driving the roads matters: it discovers roads, which
// recolours the minimap's road network, and it streams the world and the
// traffic in. The test needs the disc and a GPU, and its timings depend on
// the machine and on what else runs on it, so it carries the ctest label
// "perf" and stays out of the pre-push run.
//
// It doubles as the profiling tool: with FH1_PERF_SIZE=WxH and
// FH1_PERF_SECONDS it drives at another size or for longer, and with
// FH1_PERF_SERIALISE=1 it makes the CPU wait for the GPU at the end of each
// part, which shows how much GPU time each part needs beyond sending its
// commands (the per-part GPU timestamps alone also count the GPU's idle
// time while the CPU is still sending them).

#include "TestHarness.h"

#include "base/Host.h"
#include "base/Log.h"
#include "car/CarPhysics.h"
#include "game/Game.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/FrameStats.h"
#include "ui/Minimap.h"
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

namespace {

using Part = fh::render::FrameStats::Part;
using PartTimes = fh::render::FrameStats::PartTimes;

/// The scenarios' fixed step: every run drives the same way.
constexpr float kStep = 1.0F / 60.0F;
constexpr float kPi = 3.14159265F;

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

fh::Window& window()
{
    static fh::Window instance([] {
        fh::WindowOptions options;
        options.hidden = true;
        options.vsync = false;
        return options;
    }());
    return instance;
}

/// Width and height from FH1_PERF_SIZE ("1920x1080"), or `fallback`.
std::array<int, 2> perfSize(std::array<int, 2> fallback)
{
    const char* text = std::getenv("FH1_PERF_SIZE");
    std::array<int, 2> size{};
    if (text != nullptr && std::sscanf(text, "%dx%d", &size[0], &size[1]) == 2 && size[0] > 0 && size[1] > 0) {
        return size;
    }
    return fallback;
}

float perfSeconds(float fallback)
{
    const char* text = std::getenv("FH1_PERF_SECONDS");
    return text != nullptr && std::atof(text) > 0.0 ? static_cast<float>(std::atof(text)) : fallback;
}

bool perfSerialised()
{
    const char* text = std::getenv("FH1_PERF_SERIALISE");
    return text != nullptr && *text != '\0' && *text != '0';
}

float headingOf(glm::vec2 direction)
{
    return std::atan2(direction.x, direction.y);
}

/// Drives along the road network: follows a way to its end and there takes
/// the way through the end node that turns least, as a driver cruising the
/// roads would. The car stays on the roads the minimap draws, so it keeps
/// discovering them.
class RoadFollower {
public:
    explicit RoadFollower(const fh::ui::Navigation& navigation)
        : m_navigation(navigation)
    {
    }

    /// Controls for this frame, and whether the car needs putting back on
    /// the road (it has been stuck for a while).
    fh::InputState controls(const fh::CarPhysics& car)
    {
        fh::InputState input;
        const glm::vec3 position = car.position();
        const glm::vec2 ground(position.x, position.z);
        const glm::vec2 forward(car.forward().x, car.forward().z);
        m_stuck = car.speed() < 1.0F ? m_stuck + kStep : 0.0F;
        if (m_stuck > 4.0F) {
            input.keys[static_cast<std::size_t>(fh::Key::R)] = true;
            m_stuck = 0.0F;
            m_way.reset();
            return input;
        }
        if (!m_way && !start(position, forward)) {
            input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = 0.3F;
            return input;
        }
        // A point ahead on the road to steer at.
        for (int guard = 0; guard < 64 && glm::length(point() - ground) < kLookahead; ++guard) {
            advance();
        }
        const float error = std::remainder(headingOf(point() - ground) - headingOf(forward), 2.0F * kPi);
        const float steer = std::clamp(error * 1.5F, -1.0F, 1.0F);
        const float target = kCruise * (1.0F - 0.6F * std::min(std::abs(error), 1.0F));
        const float speed = car.speed();
        input.axes[static_cast<std::size_t>(fh::Axis::LeftX)] = steer;
        input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = speed < target ? 0.7F : 0.0F;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftTrigger)] = speed > target + 4.0F ? 0.5F : 0.0F;
        return input;
    }

private:
    static constexpr float kLookahead = 18.0F;
    static constexpr float kCruise = 22.0F;

    glm::vec2 point() const { return m_navigation.network().ways[*m_way].points[m_index]; }

    bool start(const glm::vec3& position, glm::vec2 forward)
    {
        const std::optional<std::uint32_t> node
            = m_navigation.locator().nearest(position, fh::ui::NavFilter::NoShortcutOrNone);
        if (!node) {
            return false;
        }
        return turnAt(*node, forward, std::nullopt);
    }

    /// Takes the way through `node` whose next point lies most nearly along
    /// `direction`, other than `from`.
    bool turnAt(std::uint32_t node, glm::vec2 direction, std::optional<std::uint32_t> from)
    {
        const fh::ui::NavFile& network = m_navigation.network();
        float best = -2.0F;
        bool found = false;
        for (const fh::ui::NavLink& link : network.nodes[node].links) {
            if (link.way == from || !m_navigation.locator().acceptsWay(link.way, fh::ui::NavFilter::NoShortcutOrNone)) {
                continue;
            }
            const std::vector<glm::vec2>& points = network.ways[link.way].points;
            for (const int step : {-1, 1}) {
                const long next = static_cast<long>(link.position) + step;
                if (next < 0 || next >= static_cast<long>(points.size())) {
                    continue;
                }
                const glm::vec2 along = points[static_cast<std::size_t>(next)] - points[link.position];
                if (glm::length(along) <= 0.0F || glm::length(direction) <= 0.0F) {
                    continue;
                }
                const float score = glm::dot(glm::normalize(along), glm::normalize(direction));
                if (score > best) {
                    best = score;
                    m_way = link.way;
                    m_index = static_cast<std::size_t>(next);
                    m_step = step;
                    found = true;
                }
            }
        }
        return found;
    }

    void advance()
    {
        const fh::ui::NavWay& way = m_navigation.network().ways[*m_way];
        const long next = static_cast<long>(m_index) + m_step;
        if (next >= 0 && next < static_cast<long>(way.points.size())) {
            m_index = static_cast<std::size_t>(next);
            return;
        }
        if (way.points.size() < 2) {
            m_step = -m_step;
            return;
        }
        const std::size_t previous = m_step > 0 ? m_index - 1 : m_index + 1;
        const glm::vec2 direction = way.points[m_index] - way.points[previous];
        if (!turnAt(way.nodes[m_index], direction, m_way)) {
            // A dead end: back the way it came.
            m_step = -m_step;
        }
    }

    const fh::ui::Navigation& m_navigation;
    std::optional<std::uint32_t> m_way;
    std::size_t m_index = 0;
    int m_step = 1;
    float m_stuck = 0.0F;
};

/// Per-part timings of a drive.
struct Drive {
    std::size_t frames = 0;
    PartTimes cpu{};
    PartTimes gpu{};
    PartTimes wait{};
    /// The slowest frame's CPU time of each part.
    PartTimes worst{};
    /// CPU drawing time of each frame, world to menus.
    std::vector<double> drawing;
    float metres = 0.0F;
};

double drawingSeconds(const PartTimes& parts)
{
    double total = 0.0;
    for (std::size_t part = static_cast<std::size_t>(Part::DrawWorld); part < static_cast<std::size_t>(Part::Present);
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
    for (std::size_t part = 0; part < fh::render::FrameStats::kParts; ++part) {
        std::fprintf(stderr, "  %-14s %8.2f %8.2f %8.2f %8.2f\n",
            fh::render::FrameStats::partName(static_cast<Part>(part)), drive.cpu[part] * 1000.0 / frames,
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
std::optional<Drive> drive(const char* dir, int width, int height, float seconds)
{
    window().setSize(width, height);
    fh::GameOptions options;
    options.gameDir = dir;
    options.profileDir = std::filesystem::temp_directory_path() / "fh1-perf-profile";
    std::filesystem::remove_all(options.profileDir);
    fh::Game game(window(), options);
    game.frameStats().setSerialised(perfSerialised());
    for (float t = 0.0F; t < 120.0F && !game.inControl(); t += kStep) {
        if (!game.frame({}, kStep)) {
            return std::nullopt;
        }
    }
    if (!game.inControl() || game.navigation() == nullptr) {
        std::fprintf(stderr, "free roam did not start\n");
        return std::nullopt;
    }
    RoadFollower follower(*game.navigation());
    fh::InputState previous;
    const auto step = [&](bool timed, Drive& result) {
        fh::InputState input = follower.controls(game.playerCar());
        input.previousKeys = previous.keys;
        input.previousButtons = previous.buttons;
        previous = input;
        const glm::vec3 before = game.playerCar().position();
        if (!game.frame(input, kStep)) {
            return false;
        }
        if (timed) {
            const fh::render::FrameStats& stats = game.frameStats();
            ++result.frames;
            for (std::size_t part = 0; part < fh::render::FrameStats::kParts; ++part) {
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

TEST(freeRoamDrawingStaysInBudget)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    const std::array<int, 2> size = perfSize({1600, 900});
    const std::optional<Drive> result = drive(dir, size[0], size[1], perfSeconds(30.0F));
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

} // namespace

int main(int argc, char** argv)
{
    fh::setEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen", false);
    fh::setEnvironmentVariable("SDL_AUDIO_DRIVER", "dummy", false);
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Cache),
        fh::pathToUtf8(std::filesystem::temp_directory_path() / "fh1-scenarios-cache"));
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Data),
        fh::pathToUtf8(std::filesystem::temp_directory_path() / "fh1-perf-data"));
    fh::openLogFile(std::filesystem::temp_directory_path() / "fh1-perf.log");
    try {
        window();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "skipped: no OpenGL context (%s)\n", e.what());
        return 0;
    }
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
