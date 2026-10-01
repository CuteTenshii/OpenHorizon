#pragma once

#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace openhorizon {

/// `openhorizon --crash-report=LOG [--crash-detail=TEXT]` shows the crash
/// message box for LOG and exits instead of starting the game.
inline constexpr std::string_view kCrashReportArgument = "--crash-report=";
inline constexpr std::string_view kCrashDetailArgument = "--crash-detail=";

/// Set (to anything) to keep every error box closed.
inline constexpr const char* kNoCrashDialogVariable = "OPENHORIZON_NO_CRASH_DIALOG";

struct CrashHandlerOptions {
    /// The log the report names; crash lines go to logFileDescriptor and
    /// standard error. Empty when no log file could be opened.
    std::filesystem::path logPath;
    /// Started after a crash with kCrashReportArgument, normally openhorizon
    /// itself; empty to start none.
    std::filesystem::path reporter;
    /// Where Windows writes a minidump; unused elsewhere (core dumps).
    std::filesystem::path minidumpPath;
};

/// Logs a backtrace on fatal signals (Linux, async-signal-safe, alternate
/// stack for the calling thread only) or unhandled exceptions (Windows,
/// DbgHelp plus a minidump), then starts `options.reporter` as a new
/// process, since the crashed one may have a broken heap or window system.
///
/// Linux re-raises the signal so exit status and core dump stay the
/// signal's; Windows exits with the exception code (3 after abort). Only
/// the first crash is reported. Calling this again only replaces options.
void installCrashHandlers(const CrashHandlerOptions& options);

/// `openhorizon.dmp` next to `logPath`, so a bug report gathers both.
std::filesystem::path crashDumpPath(const std::filesystem::path& logPath);

/// False when no one could answer an error box: kNoCrashDialogVariable is
/// set, SDL_VIDEO_DRIVER names the offscreen or dummy driver, or (outside
/// Windows) neither DISPLAY nor WAYLAND_DISPLAY is set.
bool errorDialogsAllowed();

/// What the reporter was started with.
struct CrashReport {
    std::filesystem::path logPath;
    /// An exception's message, or empty after a plain crash.
    std::string detail;
};

/// The crash report asked for by `arguments` (program name first, as
/// commandLineArguments gives), or nothing when the first argument is not
/// kCrashReportArgument.
std::optional<CrashReport> parseCrashReportArguments(std::span<const std::string> arguments);

/// The crash message box text; names the minidump only when `dumpPath` exists.
std::string crashDialogText(const CrashReport& report, const std::filesystem::path& dumpPath = {});

/// The message box text for a DataError reaching main.
std::string gameDataErrorText(std::string_view detail, const std::filesystem::path& logPath);

/// The message box text for a startup problem the player can fix.
std::string startupErrorText(std::string_view problem, std::string_view fix);

/// Type as the source spells it (`openhorizon::DataError`) and message; the
/// type is empty for a null `error` and "unknown type" when it can't be named.
struct ExceptionDescription {
    std::string type;
    std::string message;
};
ExceptionDescription describeException(const std::exception_ptr& error);

/// `argument` quoted so CommandLineToArgvW and the C runtime read it back
/// unchanged.
std::string windowsCommandLineArgument(std::string_view argument);

} // namespace openhorizon
