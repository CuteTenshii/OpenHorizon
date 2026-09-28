// Forza Horizon, running natively: parses the command line and settings,
// opens the window and runs the game.

#include "base/Log.h"
#include "base/Settings.h"
#include "game/Game.h"
#include "platform/Window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void printUsage()
{
    std::printf(
        "usage: fh1 [options]\n"
        "\n"
        "  --game DIR         extracted disc (holding media/), or set FH1_GAME_DIR,\n"
        "                     or 'game = DIR' in %s\n"
        "  --car NAME         car media name, e.g. ALF_8C_08 (default)\n"
        "  --track NAME       track folder under media/tracks (default colorado)\n"
        "  --spawn ID         start at a GameObjs.xml GameplayID instead of the free-roam start\n"
        "  --at X,Z           start at world coordinates instead\n"
        "  --heading DEG      start heading, clockwise from north\n"
        "  --time HH:MM       time of day to start at (default 16:00)\n"
        "  --race ID          start a race at once, by its event id (e.g. FR02)\n"
        "  --autopilot        let the AI drive the player's car in races\n"
        "  --manual           manual gearbox (E/Q or B/X to shift)\n"
        "  --demo-drive       drive with full throttle and a gentle weave, for testing\n"
        "  --width W --height H --fullscreen\n"
        "  --frames N         quit after N frames\n"
        "  --screenshot FILE  with --frames, save the last frame as PNG\n"
        "  --debug            verbose log and OpenGL debug output\n",
        fh::Settings::defaultPath().string().c_str());
}

} // namespace

int main(int argc, char** argv)
{
    fh::Settings settings = fh::Settings::load();
    fh::GameOptions options;
    fh::WindowOptions window;
    window.width = static_cast<int>(settings.getFloat("width", 1600.0F));
    window.height = static_cast<int>(settings.getFloat("height", 900.0F));
    window.fullscreen = settings.getBool("fullscreen", false);
    window.vsync = settings.getBool("vsync", true);
    options.car = settings.get("car", options.car);
    options.manualGearbox = settings.getBool("manual", false);

    std::string gameDir = settings.get("game", "");
    if (const char* env = std::getenv("FH1_GAME_DIR"); env && *env) {
        gameDir = env;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--game") {
            gameDir = value();
        } else if (arg == "--car") {
            options.car = value();
        } else if (arg == "--track") {
            options.track = value();
        } else if (arg == "--spawn") {
            options.spawnObject = value();
        } else if (arg == "--at") {
            const std::string text = value();
            float x = 0.0F;
            float z = 0.0F;
            if (std::sscanf(text.c_str(), "%f,%f", &x, &z) != 2) {
                std::fprintf(stderr, "--at expects X,Z\n");
                return 2;
            }
            options.spawnAt = glm::vec2(x, z);
        } else if (arg == "--heading") {
            options.spawnHeading = std::strtof(value().c_str(), nullptr) * 3.14159265F / 180.0F;
        } else if (arg == "--time") {
            int hours = 0;
            int minutes = 0;
            if (std::sscanf(value().c_str(), "%d:%d", &hours, &minutes) < 1) {
                std::fprintf(stderr, "--time expects HH:MM\n");
                return 2;
            }
            options.startTime = static_cast<float>(hours * 3600 + minutes * 60);
        } else if (arg == "--race") {
            options.race = value();
        } else if (arg == "--autopilot") {
            options.autopilot = true;
        } else if (arg == "--manual") {
            options.manualGearbox = true;
        } else if (arg == "--demo-drive") {
            options.demoDrive = true;
        } else if (arg == "--width") {
            window.width = std::atoi(value().c_str());
        } else if (arg == "--height") {
            window.height = std::atoi(value().c_str());
        } else if (arg == "--fullscreen") {
            window.fullscreen = true;
        } else if (arg == "--frames") {
            options.frames = std::atoi(value().c_str());
        } else if (arg == "--screenshot") {
            options.screenshot = value();
        } else if (arg == "--debug") {
            fh::setLogLevel(fh::LogLevel::Debug);
            window.glDebug = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    if (gameDir.empty()) {
        std::fprintf(stderr, "No game folder given.\n\n");
        printUsage();
        return 2;
    }
    options.gameDir = gameDir;
    // Remember the folder so the next start needs no options.
    if (!settings.get("game")) {
        settings.set("game", gameDir);
        settings.save();
    }

    try {
        fh::Window win(window);
        fh::Game game(win, options);
        return game.run();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
