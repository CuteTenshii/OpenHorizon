#pragma once

#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace openhorizon {

/// Starts fh1 as the crash reporter rather than the game: `fh1
/// --crash-report=LOG [--crash-detail=TEXT]` shows the crash message box
/// for the log at LOG and exits. The crashed process starts it, see
/// installCrashHandlers.
inline constexpr std::string_view kCrashReportArgument = "--crash-report=";
inline constexpr std::string_view kCrashDetailArgument = "--crash-detail=";

/// Set (to anything) to keep every error box closed: the test scripts and
/// the test harness set it, so a crashing test never waits on a click.
inline constexpr const char* kNoCrashDialogVariable = "FH1_NO_CRASH_DIALOG";

struct CrashHandlerOptions {
    /// The log the report names; the crash lines themselves go through the
    /// descriptor openLogFile opened (logFileDescriptor) and to standard
    /// error. Empty when no log file could be opened.
    std::filesystem::path logPath;
    /// The program started after a crash as `reporter --crash-report=LOG
    /// [--crash-detail=TEXT]`, normally fh1 itself; empty to start none
    /// (tests, no display, see errorDialogsAllowed).
    std::filesystem::path reporter;
    /// Where Windows writes a minidump of the crashed process
    /// (crashDumpPath); unused on other platforms, where the system's core
    /// dump does that job.
    std::filesystem::path minidumpPath;
};

/// Catches the crashes the game cannot recover from and reports them.
///
/// On Linux, handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT run
/// on an alternate stack (for the calling thread; other threads use their
/// own stack, so their stack overflows die without a report) and use only
/// async-signal-safe calls: they write the signal, the faulting address and
/// a backtrace (backtrace_symbols_fd: module and offset, for addr2line) to
/// the log's descriptor and to standard error. On Windows an unhandled
/// exception filter writes the exception code and address and a stack walk
/// with symbols and lines (DbgHelp), and a minidump to
/// `options.minidumpPath`; abort() goes through the same report.
/// std::terminate, an uncaught exception included, first logs the
/// exception's type and message, then takes the same path.
///
/// The message box never opens inside the crashed process, whose heap,
/// locks or window system connection may be what broke: the handler
/// starts `options.reporter` as a new process (_Fork and execve on Linux,
/// both async-signal-safe; CreateProcessW on Windows), does not wait for
/// it, and dies. Linux then restores the default action and raises the
/// signal again, so the exit status and core dumps are the signal's;
/// Windows ends with the exception code as exit status (3 after abort or
/// std::terminate, as abort gives).
///
/// Only the first crash is reported: a crash inside the handler ends the
/// process at once, and a second thread crashing meanwhile waits for the
/// first report to end the process. Calling this again replaces the
/// options and installs nothing twice.
void installCrashHandlers(const CrashHandlerOptions& options);

/// `fh1.dmp` next to `logPath`: the Windows minidump, kept beside the log
/// so a bug report gathers both from one folder.
std::filesystem::path crashDumpPath(const std::filesystem::path& logPath);

/// False when no one could answer an error box: kNoCrashDialogVariable is
/// set, SDL_VIDEO_DRIVER names the offscreen or dummy driver, or (outside
/// Windows) neither DISPLAY nor WAYLAND_DISPLAY is set.
bool errorDialogsAllowed();

/// What the reporter was started with.
struct CrashReport {
    std::filesystem::path logPath;
    /// One line on what happened (an exception's message), or empty when
    /// all the report knows is that the process crashed.
    std::string detail;
};

/// The crash report asked for by `arguments` (program name first, as
/// commandLineArguments gives), or nothing when the first argument is not
/// kCrashReportArgument.
std::optional<CrashReport> parseCrashReportArguments(std::span<const std::string> arguments);

/// The text of the message box after a crash: that the game stopped, what
/// happened, and where the log is (and the minidump when `dumpPath` names
/// one that exists) so the player can attach it to a bug report.
std::string crashDialogText(const CrashReport& report, const std::filesystem::path& dumpPath = {});

/// The text of the message box when the game stops on a file of the disc
/// it cannot read (a DataError reaching main).
std::string gameDataErrorText(std::string_view detail, const std::filesystem::path& logPath);

/// The text of the message box when the game cannot start for a reason the
/// player can fix: `problem` says what is wrong, `fix` what to do.
std::string startupErrorText(std::string_view problem, std::string_view fix);

/// An exception's type as the source spells it (`openhorizon::DataError`) and its
/// message; the type is empty when `error` is null, and "unknown type"
/// stands in for what cannot be named.
struct ExceptionDescription {
    std::string type;
    std::string message;
};
ExceptionDescription describeException(const std::exception_ptr& error);

/// `argument` quoted for a Windows command line so that CommandLineToArgvW
/// and the C runtime read it back unchanged (backslashes doubled before a
/// quote, quotes escaped). Plain text in, plain text out, so it is tested
/// on every platform.
std::string windowsCommandLineArgument(std::string_view argument);

} // namespace openhorizon
