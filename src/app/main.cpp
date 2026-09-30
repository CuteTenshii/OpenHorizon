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
    std::printf("usage: fh1 [options]\n"
                "\n"
                "  --game DIR         extracted disc (holding media/), or set FH1_GAME_DIR,\n"
                "                     or 'game = DIR' in %s\n"
                "  --width W --height H --fullscreen\n"
                "  --log FILE         write the full log there (default %s)\n"
                "\n"
                "Other choices are settings in the same file: car (a media name such as\n"
                "ALF_8C_08), track, manual (gearbox), metric (speedometer), vsync, and debug\n"
                "(verbose console and OpenGL debug output).\n",
        fh::pathToUtf8(fh::Settings::defaultPath()).c_str(), fh::pathToUtf8(fh::defaultLogPath()).c_str());
}

/// What is wrong with `folder` as the extracted disc, in words for the
/// player, or nothing when it looks like one: the disc root (holding
/// media/) or the media folder itself, as Vfs accepts.
std::optional<std::string> gameFolderProblem(const std::filesystem::path& folder)
{
    const std::string name = fh::pathToUtf8(folder);
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
    return logPath.empty() ? std::string()
                           : std::format("\n\nThe log file records what happened:\n{}", fh::pathToUtf8(logPath));
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = fh::commandLineArguments(argc, argv);
    const std::string title = fh::WindowOptions().title;
    if (const std::optional<fh::CrashReport> report = fh::parseCrashReportArguments(arguments)) {
        fh::showErrorDialog(title, fh::crashDialogText(*report, fh::crashDumpPath(report->logPath)));
        return 0;
    }

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
    if (std::optional<std::string> env = fh::environmentVariable("FH1_GAME_DIR")) {
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
            logPath = fh::pathFromUtf8(value());
        } else {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    const std::filesystem::path shownLog = fh::openLogFile(logPath) ? logPath : std::filesystem::path();
    fh::CrashHandlerOptions crash;
    crash.logPath = shownLog;
    if (fh::errorDialogsAllowed()) {
        crash.reporter = fh::currentExecutablePath();
    }
    crash.minidumpPath = fh::crashDumpPath(logPath);
    fh::installCrashHandlers(crash);

    const std::string chooseFolder = "Start the game with --game followed by the folder you extracted the disc to "
                                     "(the one holding the media folder).";
    if (gameDir.empty()) {
        fh::logError("no game folder given");
        std::fprintf(stderr, "No game folder given.\n\n");
        printUsage();
        fh::showErrorDialog(
            title, fh::startupErrorText("No game folder is set.", chooseFolder + " The game remembers it after that."));
        return 2;
    }
    options.gameDir = fh::pathFromUtf8(gameDir);
    if (const std::optional<std::string> problem = gameFolderProblem(options.gameDir)) {
        fh::logError("{}", *problem);
        fh::showErrorDialog(title,
            fh::startupErrorText(*problem,
                std::format("{} If the game remembered this folder from an earlier start, change the game line in {}.",
                    chooseFolder, fh::pathToUtf8(fh::Settings::defaultPath()))));
        return 2;
    }
    // Remember the folder so the next start needs no options; only once it
    // proved to be a disc, so a mistyped folder is not kept.
    if (!settings.get("game")) {
        settings.set("game", gameDir);
        settings.save();
    }

    // Outside the try, so the error box below can open over the window.
    std::optional<fh::Window> win;
    try {
        try {
            win.emplace(window);
        } catch (const fh::GraphicsDriverError& e) {
            fh::logError("{}", e.what());
            fh::showErrorDialog(title,
                fh::startupErrorText("The graphics driver does not offer OpenGL 4.5, which the game needs.",
                    "Update the graphics driver, then start the game again." + logPointer(shownLog)));
            return 1;
        } catch (const std::exception& e) {
            fh::logError("{}", e.what());
            fh::showErrorDialog(title,
                fh::startupErrorText("The game window could not be opened.",
                    "Check that a desktop session is running, then start the game again." + logPointer(shownLog)));
            return 1;
        }
        {
            // Closed before the game starts, so its audio device is gone
            // when the game opens one.
            fh::Title titleScreen(*win, options.gameDir);
            if (!titleScreen.run()) {
                return 0;
            }
        }
        fh::Game game(*win, options);
        return game.run();
    } catch (const fh::DataError& e) {
        fh::logError("{}", e.what());
        fh::showErrorDialog(title, fh::gameDataErrorText(e.what(), shownLog), win ? win->sdlWindow() : nullptr);
        return 1;
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        fh::showErrorDialog(title, fh::crashDialogText({shownLog, e.what()}), win ? win->sdlWindow() : nullptr);
        return 1;
    }
}
