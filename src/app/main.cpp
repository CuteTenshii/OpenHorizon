// Forza Horizon, running natively: reads the settings and the few command
// line options a player needs, opens the window, shows the title screen and
// runs the game. Testing goes through the scenario tests and scripts/, not
// through options here. Started with --crash-report=LOG, it is instead the
// crash reporter a crashed game starts (see installCrashHandlers): it shows
// the message box and exits.

#include "base/Crash.h"
#include "base/Error.h"
#include "base/Host.h"
#include "base/Log.h"
#include "base/Settings.h"
#include "game/Game.h"
#include "game/Title.h"
#include "platform/ErrorDialog.h"
#include "platform/Window.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void printUsage()
{
    std::printf("usage: openhorizon [options]\n"
                "\n"
                "  --game DIR         extracted disc (holding media/), or set OPENHORIZON_GAME_DIR,\n"
                "                     or 'game = DIR' in %s\n"
                "  --width W --height H --fullscreen\n"
                "  --log FILE         write the full log there (default %s)\n"
                "\n"
                "Other choices are settings in the same file: car (a media name such as\n"
                "ALF_8C_08), track, manual (gearbox), metric (speedometer), vsync, and debug\n"
                "(verbose console and OpenGL debug output).\n",
        openhorizon::pathToUtf8(openhorizon::Settings::defaultPath()).c_str(),
        openhorizon::pathToUtf8(openhorizon::defaultLogPath()).c_str());
}

/// What is wrong with `folder` as the extracted disc, in words for the
/// player, or nothing when it looks like one: the disc root (holding
/// media/) or the media folder itself, as Vfs accepts.
std::optional<std::string> gameFolderProblem(const std::filesystem::path& folder)
{
    const std::string name = openhorizon::pathToUtf8(folder);
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(folder, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::format("The game folder {} does not exist.", name);
    }
    if (error) {
        return std::format("The game folder {} cannot be opened.", name);
    }
    if (!std::filesystem::is_directory(status)) {
        return std::format("{} is a file, not the folder the disc was extracted to.", name);
    }
    const std::filesystem::directory_iterator listing(folder, error);
    if (error) {
        return std::format("The game folder {} cannot be opened.", name);
    }
    if (!std::filesystem::is_directory(folder / "media", error)
        && !std::filesystem::is_regular_file(folder / "zipmanifest.xml", error)) {
        return std::format("The folder {} does not hold the game's files: it has no media folder.", name);
    }
    return std::nullopt;
}

/// The last lines of an error box that points at the log, or nothing more
/// when no log could be written.
std::string logPointer(const std::filesystem::path& logPath)
{
    return logPath.empty()
        ? std::string()
        : std::format("\n\nThe log file records what happened:\n{}", openhorizon::pathToUtf8(logPath));
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = openhorizon::commandLineArguments(argc, argv);
    const std::string title = openhorizon::WindowOptions().title;
    if (const std::optional<openhorizon::CrashReport> report = openhorizon::parseCrashReportArguments(arguments)) {
        openhorizon::showErrorDialog(
            title, openhorizon::crashDialogText(*report, openhorizon::crashDumpPath(report->logPath)));
        return 0;
    }

    openhorizon::Settings settings = openhorizon::Settings::load();
    openhorizon::GameOptions options;
    openhorizon::WindowOptions window;
    window.width = static_cast<int>(settings.getFloat("width", 1600.0F));
    window.height = static_cast<int>(settings.getFloat("height", 900.0F));
    window.fullscreen = settings.getBool("fullscreen", false);
    window.vsync = settings.getBool("vsync", true);
    options.car = settings.get("car", options.car);
    options.track = settings.get("track", options.track);
    options.manualGearbox = settings.getBool("manual", false);
    options.metric = settings.getBool("metric", true);
    if (settings.getBool("debug", false)) {
        openhorizon::setLogLevel(openhorizon::LogLevel::Debug);
        window.glDebug = true;
    }

    std::filesystem::path logPath = openhorizon::defaultLogPath();
    std::string gameDir = settings.get("game", "");
    if (std::optional<std::string> env = openhorizon::environmentVariable("OPENHORIZON_GAME_DIR")) {
        gameDir = std::move(*env);
    }
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string& arg = arguments[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= arguments.size()) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return arguments[++i];
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
            logPath = openhorizon::pathFromUtf8(value());
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    const std::filesystem::path shownLog = openhorizon::openLogFile(logPath) ? logPath : std::filesystem::path();
    openhorizon::CrashHandlerOptions crash;
    crash.logPath = shownLog;
    if (openhorizon::errorDialogsAllowed()) {
        crash.reporter = openhorizon::currentExecutablePath();
    }
    crash.minidumpPath = openhorizon::crashDumpPath(logPath);
    openhorizon::installCrashHandlers(crash);

    const std::string chooseFolder = "Start the game with --game followed by the folder you extracted the disc to "
                                     "(the one holding the media folder).";
    if (gameDir.empty()) {
        openhorizon::logError("no game folder given");
        std::fprintf(stderr, "No game folder given.\n\n");
        printUsage();
        openhorizon::showErrorDialog(title,
            openhorizon::startupErrorText(
                "No game folder is set.", chooseFolder + " The game remembers it after that."));
        return 2;
    }
    options.gameDir = openhorizon::pathFromUtf8(gameDir);
    if (const std::optional<std::string> problem = gameFolderProblem(options.gameDir)) {
        openhorizon::logError("{}", *problem);
        openhorizon::showErrorDialog(title,
            openhorizon::startupErrorText(*problem,
                std::format("{} If the game remembered this folder from an earlier start, change the game line in {}.",
                    chooseFolder, openhorizon::pathToUtf8(openhorizon::Settings::defaultPath()))));
        return 2;
    }
    // Remember the folder so the next start needs no options; only once it
    // proved to be a disc, so a mistyped folder is not kept.
    if (!settings.get("game")) {
        settings.set("game", gameDir);
        settings.save();
    }

    // Outside the try, so the error box below can open over the window.
    std::optional<openhorizon::Window> win;
    try {
        try {
            win.emplace(window);
        } catch (const openhorizon::GraphicsDriverError& e) {
            openhorizon::logError("{}", e.what());
            openhorizon::showErrorDialog(title,
                openhorizon::startupErrorText("The graphics driver does not offer OpenGL 4.5, which the game needs.",
                    "Update the graphics driver, then start the game again." + logPointer(shownLog)));
            return 1;
        } catch (const std::exception& e) {
            openhorizon::logError("{}", e.what());
            openhorizon::showErrorDialog(title,
                openhorizon::startupErrorText("The game window could not be opened.",
                    "Check that a desktop session is running, then start the game again." + logPointer(shownLog)));
            return 1;
        }
        {
            // Closed before the game starts, so its audio device is gone
            // when the game opens one.
            openhorizon::Title titleScreen(*win, options.gameDir);
            if (!titleScreen.run()) {
                return 0;
            }
        }
        openhorizon::Game game(*win, options);
        return game.run();
    } catch (const openhorizon::DataError& e) {
        openhorizon::logError("{}", e.what());
        openhorizon::showErrorDialog(
            title, openhorizon::gameDataErrorText(e.what(), shownLog), win ? win->sdlWindow() : nullptr);
        return 1;
    } catch (const std::exception& e) {
        openhorizon::logError("{}", e.what());
        openhorizon::showErrorDialog(
            title, openhorizon::crashDialogText({shownLog, e.what()}), win ? win->sdlWindow() : nullptr);
        return 1;
    }
}
