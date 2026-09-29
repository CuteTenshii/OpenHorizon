// Forza Horizon, running natively: reads the settings and the few command
// line options a player needs, opens the window, shows the title screen and
// runs the game. Testing goes through the scenario tests and scripts/, not
// through options here.

#include "base/Log.h"
#include "base/Settings.h"
#include "game/Game.h"
#include "game/Title.h"
#include "platform/Window.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

void printUsage()
{
    std::printf(
        "usage: fh1 [options]\n"
        "\n"
        "  --game DIR         extracted disc (holding media/), or set FH1_GAME_DIR,\n"
        "                     or 'game = DIR' in %s\n"
        "  --width W --height H --fullscreen\n"
        "  --log FILE         write the full log there (default %s)\n"
        "\n"
        "Other choices are settings in the same file: car (a media name such as\n"
        "ALF_8C_08), track, manual (gearbox), metric (speedometer), vsync, and debug\n"
        "(verbose console and OpenGL debug output).\n",
        fh::Settings::defaultPath().string().c_str(), fh::defaultLogPath().string().c_str());
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
    options.track = settings.get("track", options.track);
    options.manualGearbox = settings.getBool("manual", false);
    options.metric = settings.getBool("metric", true);
    if (settings.getBool("debug", false)) {
        fh::setLogLevel(fh::LogLevel::Debug);
        window.glDebug = true;
    }

    std::filesystem::path logPath = fh::defaultLogPath();
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
        } else if (arg == "--width") {
            window.width = std::atoi(value().c_str());
        } else if (arg == "--height") {
            window.height = std::atoi(value().c_str());
        } else if (arg == "--fullscreen") {
            window.fullscreen = true;
        } else if (arg == "--log") {
            logPath = value();
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    fh::openLogFile(logPath);
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
        {
            // Closed before the game starts, so its audio device is gone
            // when the game opens one.
            fh::Title title(win, options.gameDir);
            if (!title.run()) {
                return 0;
            }
        }
        fh::Game game(win, options);
        return game.run();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
