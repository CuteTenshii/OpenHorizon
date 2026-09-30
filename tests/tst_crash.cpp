// The crash handler, run for real: the test starts itself again as a child
// that crashes on purpose (a null write, abort, an uncaught exception, an
// uncaught DataError) and checks what the child left in its log and how it
// ended. The child's reporter is this executable too, which writes down
// its arguments instead of opening a message box.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include "base/Crash.h"
#include "base/Error.h"
#include "base/Host.h"
#include "base/Log.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr std::string_view kChildArgument = "--crash-child";
/// Where the test reporter writes the arguments it was started with.
constexpr const char* kReportOutVariable = "FH1_TST_CRASH_REPORT_OUT";

std::filesystem::path scratchFolder()
{
    return std::filesystem::temp_directory_path() / std::format("fh1-tst-crash-{}", openhorizon::processId());
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::size_t countOf(std::string_view text, std::string_view part)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

/// The line after the first line holding `marker`, or empty.
std::string lineAfter(std::string_view text, std::string_view marker)
{
    const std::size_t at = text.find(marker);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t start = text.find('\n', at);
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find('\n', start + 1);
    return std::string(text.substr(start + 1, end == std::string_view::npos ? end : end - start - 1));
}

struct ChildEnd {
    bool signaled = false;
    int signal = 0;
    unsigned long exitCode = 0;
};

ChildEnd runChild(const std::vector<std::string>& arguments)
{
    const std::filesystem::path self = openhorizon::currentExecutablePath();
    ChildEnd end;
#ifdef _WIN32
    std::string commandLine = openhorizon::windowsCommandLineArgument(openhorizon::pathToUtf8(self));
    for (const std::string& argument : arguments) {
        commandLine += ' ' + openhorizon::windowsCommandLineArgument(argument);
    }
    std::wstring wide = openhorizon::pathFromUtf8(commandLine).native();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self.c_str(), wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("cannot start the crashing child");
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    end.exitCode = code;
#else
    std::vector<std::string> all = {self.native()};
    all.insert(all.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(all.size() + 1);
    for (std::string& argument : all) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    pid_t child = 0;
    if (posix_spawn(&child, self.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        throw std::runtime_error("cannot start the crashing child");
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("lost the crashing child");
        }
    }
    end.signaled = WIFSIGNALED(status);
    end.signal = end.signaled ? WTERMSIG(status) : 0;
    end.exitCode = WIFEXITED(status) ? static_cast<unsigned long>(WEXITSTATUS(status)) : 0;
#endif
    return end;
}

/// Runs a child that crashes as `kind` and returns how it ended and its
/// log.
std::pair<ChildEnd, std::string> crash(std::string_view kind, bool withReporter = false)
{
    const std::filesystem::path folder = scratchFolder() / kind;
    std::filesystem::create_directories(folder);
    const std::filesystem::path log = folder / "fh1.log";
    const ChildEnd end = runChild(
        {std::string(kChildArgument), std::string(kind), openhorizon::pathToUtf8(log), withReporter ? "1" : "0"});
    return {end, readFile(log)};
}

void checkEndedWithSignal(const ChildEnd& end, [[maybe_unused]] int signal, [[maybe_unused]] unsigned long windowsCode)
{
#ifdef _WIN32
    CHECK(end.exitCode == windowsCode);
#else
    CHECK(end.signaled);
    CHECK(end.signal == signal);
#endif
}

/// A report has one crash line, however often the handlers were installed,
/// and a stack of at least one frame after it.
void checkReport(const std::string& log, std::string_view headline)
{
    INFO("the child's log:\n" << log);
    CHECK(log.find(std::string("[    crash] fatal   ") + std::string(headline)) != std::string::npos);
#ifdef _WIN32
    CHECK(countOf(log, "fatal   stack:") == std::size_t{1});
    CHECK(lineAfter(log, "fatal   stack:").starts_with("  #0"));
#else
    CHECK(countOf(log, "fatal   backtrace:") == std::size_t{1});
    CHECK(lineAfter(log, "fatal   backtrace:").find("[0x") != std::string::npos);
#endif
}

} // namespace

TEST_CASE("null_write_logs_the_signal_and_a_backtrace", "[crash]")
{
    const auto [end, log] = crash("null");
#ifdef _WIN32
    checkEndedWithSignal(end, 0, 0xC0000005UL);
    checkReport(log, "exception 0xc0000005 (EXCEPTION_ACCESS_VIOLATION)");
    CHECK(log.find("writing address 0x0000000000000000") != std::string::npos);
    CHECK(std::filesystem::is_regular_file(scratchFolder() / "null" / "fh1.dmp"));
#else
    checkEndedWithSignal(end, SIGSEGV, 0);
    checkReport(log, "signal 11 (SIGSEGV), code 1, address 0x0");
    CHECK(countOf(log, "fatal   signal") == std::size_t{1});
#endif
    // The lines logged before the crash are all there.
    CHECK(log.find("crash test child ready") != std::string::npos);
}

TEST_CASE("abort_logs_and_ends_as_abort", "[crash]")
{
    const auto [end, log] = crash("abort");
#ifdef _WIN32
    checkEndedWithSignal(end, 0, 3);
    checkReport(log, "abort was called");
#else
    checkEndedWithSignal(end, SIGABRT, 0);
    checkReport(log, "signal 6 (SIGABRT)");
#endif
}

TEST_CASE("uncaught_exception_logs_its_type_and_message", "[crash]")
{
    const auto [end, log] = crash("exception");
    checkEndedWithSignal(end, SIGABRT, 3);
    checkReport(log, "uncaught exception std::runtime_error: boom from tst_crash");
    // The terminate handler ends the process itself: no second report from
    // the abort signal.
    CHECK(countOf(log, "[    crash] fatal   uncaught exception") == std::size_t{1});
    CHECK(countOf(log, "SIGABRT") == std::size_t{0});
}

TEST_CASE("uncaught_data_error_logs_its_type_and_message", "[crash]")
{
    const auto [end, log] = crash("dataerror");
    checkEndedWithSignal(end, SIGABRT, 3);
    checkReport(log, "uncaught exception openhorizon::DataError: media/missing.xml: cannot open");
}

TEST_CASE("crash_starts_the_reporter_with_the_log_and_the_message", "[crash]")
{
    const std::filesystem::path out = scratchFolder() / "reporter.txt";
    std::filesystem::create_directories(scratchFolder());
    std::filesystem::remove(out);
    openhorizon::setEnvironmentVariable(kReportOutVariable, openhorizon::pathToUtf8(out));
    const auto [end, log] = crash("exception", true);
    openhorizon::unsetEnvironmentVariable(kReportOutVariable);
    checkEndedWithSignal(end, SIGABRT, 3);
    // The reporter is not waited for by the crashed process, so it may
    // still be starting.
    std::string arguments;
    for (int tries = 0; tries < 100 && arguments.empty(); ++tries) {
        if (std::filesystem::exists(out)) {
            arguments = readFile(out);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    const std::string logPath = openhorizon::pathToUtf8(scratchFolder() / "exception" / "fh1.log");
    CHECK(arguments == "--crash-report=" + logPath + "\n--crash-detail=boom from tst_crash\n");
}

TEST_CASE("crash_dialog_text_names_the_log", "[crash]")
{
    const std::filesystem::path folder = scratchFolder() / "text";
    std::filesystem::create_directories(folder);
    const std::filesystem::path log = folder / "fh1.log";
    const std::string logName = openhorizon::pathToUtf8(log);
    CHECK(openhorizon::crashDialogText({log, ""})
        == "Forza Horizon has stopped unexpectedly.\n\n"
           "It ran into a problem it could not recover from.\n\n"
           "The log file records what happened:\n"
            + logName + "\n\nPlease attach it to your bug report.");
    CHECK(openhorizon::crashDialogText({log, "media/x.xml: cannot open"})
        == "Forza Horizon has stopped unexpectedly.\n\n"
           "media/x.xml: cannot open\n\n"
           "The log file records what happened:\n"
            + logName + "\n\nPlease attach it to your bug report.");

    // A dump is named only when there is one.
    const std::filesystem::path dump = openhorizon::crashDumpPath(log);
    CHECK(dump == folder / "fh1.dmp");
    std::filesystem::remove(dump);
    CHECK(openhorizon::crashDialogText({log, ""}, dump).find("dump") == std::string::npos);
    std::ofstream(dump) << "MDMP";
    CHECK(openhorizon::crashDialogText({log, ""}, dump)
        == "Forza Horizon has stopped unexpectedly.\n\n"
           "It ran into a problem it could not recover from.\n\n"
           "The log file records what happened:\n"
            + logName + "\n\nA crash dump was saved next to it:\n" + openhorizon::pathToUtf8(dump)
            + "\n\nPlease attach both files to your bug report.");

    CHECK(openhorizon::crashDialogText({{}, ""})
        == "Forza Horizon has stopped unexpectedly.\n\n"
           "It ran into a problem it could not recover from.\n\n"
           "No log file could be written for this run.");
}

TEST_CASE("startup_and_data_error_texts", "[crash]")
{
    CHECK(openhorizon::startupErrorText("No game folder is set.", "Start the game with --game.")
        == "Forza Horizon could not start.\n\nNo game folder is set.\n\nStart the game with --game.");
    const std::filesystem::path log = openhorizon::pathFromUtf8("/home/player/fh1.log");
    CHECK(openhorizon::gameDataErrorText("media/db/gamedb.slt: cannot open", log)
        == "Forza Horizon has stopped because it could not read one of the game's files.\n\n"
           "media/db/gamedb.slt: cannot open\n\n"
           "Check that the game folder holds a complete copy of the disc.\n\n"
           "The log file records what happened:\n"
            + openhorizon::pathToUtf8(log));
    // User-visible text avoids em dashes.
    for (const std::string& text :
        {openhorizon::crashDialogText({log, ""}), openhorizon::gameDataErrorText("x", log)}) {
        CHECK(text.find("\xE2\x80\x94") == std::string::npos);
    }
}

TEST_CASE("crash_report_arguments", "[crash]")
{
    const std::vector<std::string> report = {"fh1", "--crash-report=/tmp/fh1.log", "--crash-detail=a \"quoted\" line"};
    const std::optional<openhorizon::CrashReport> parsed = openhorizon::parseCrashReportArguments(report);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->logPath == openhorizon::pathFromUtf8("/tmp/fh1.log"));
        CHECK(parsed->detail == "a \"quoted\" line");
    }
    const std::vector<std::string> plain = {"fh1", "--crash-report="};
    CHECK(openhorizon::parseCrashReportArguments(plain).has_value());
    const std::vector<std::string> game = {"fh1", "--game", "/disc", "--crash-report=/tmp/fh1.log"};
    CHECK(!openhorizon::parseCrashReportArguments(game).has_value());
    const std::vector<std::string> none = {"fh1"};
    CHECK(!openhorizon::parseCrashReportArguments(none).has_value());
}

TEST_CASE("windows_command_line_quoting", "[crash]")
{
    CHECK(openhorizon::windowsCommandLineArgument("plain") == "\"plain\"");
    CHECK(openhorizon::windowsCommandLineArgument("two words") == "\"two words\"");
    CHECK(openhorizon::windowsCommandLineArgument("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(openhorizon::windowsCommandLineArgument("C:\\dir\\") == "\"C:\\dir\\\\\"");
    CHECK(openhorizon::windowsCommandLineArgument("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(openhorizon::windowsCommandLineArgument("") == "\"\"");
}

TEST_CASE("exception_descriptions", "[crash]")
{
    CHECK(openhorizon::describeException(nullptr).type.empty());
    const openhorizon::ExceptionDescription runtime
        = openhorizon::describeException(std::make_exception_ptr(std::runtime_error("went wrong")));
    CHECK(runtime.type == "std::runtime_error");
    CHECK(runtime.message == "went wrong");
    const openhorizon::ExceptionDescription data
        = openhorizon::describeException(std::make_exception_ptr(openhorizon::DataError("bad file")));
    CHECK(data.type == "openhorizon::DataError");
    CHECK(data.message == "bad file");
    const openhorizon::ExceptionDescription number = openhorizon::describeException(std::make_exception_ptr(42));
    CHECK(!number.type.empty());
    CHECK(number.message.empty());
}

TEST_CASE("error_dialogs_stay_closed_under_tests_and_without_a_display", "[crash]")
{
    const std::optional<std::string> savedDriver = openhorizon::environmentVariable("SDL_VIDEO_DRIVER");
    const std::optional<std::string> savedDisplay = openhorizon::environmentVariable("DISPLAY");
    const std::optional<std::string> savedWayland = openhorizon::environmentVariable("WAYLAND_DISPLAY");
    const auto restore = [](const char* name, const std::optional<std::string>& value) {
        if (value) {
            openhorizon::setEnvironmentVariable(name, *value);
        } else {
            openhorizon::unsetEnvironmentVariable(name);
        }
    };

    // The harness sets the variable for every test.
    CHECK(!openhorizon::errorDialogsAllowed());

    openhorizon::unsetEnvironmentVariable(openhorizon::kNoCrashDialogVariable);
    openhorizon::setEnvironmentVariable("DISPLAY", ":0");
    openhorizon::unsetEnvironmentVariable("WAYLAND_DISPLAY");
    openhorizon::setEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen");
    CHECK(!openhorizon::errorDialogsAllowed());
    openhorizon::setEnvironmentVariable("SDL_VIDEO_DRIVER", "wayland,dummy");
    CHECK(!openhorizon::errorDialogsAllowed());
    openhorizon::setEnvironmentVariable("SDL_VIDEO_DRIVER", "x11");
    CHECK(openhorizon::errorDialogsAllowed());
#ifndef _WIN32
    openhorizon::unsetEnvironmentVariable("DISPLAY");
    CHECK(!openhorizon::errorDialogsAllowed());
    openhorizon::setEnvironmentVariable("WAYLAND_DISPLAY", "wayland-0");
    CHECK(openhorizon::errorDialogsAllowed());
#endif

    openhorizon::setEnvironmentVariable(openhorizon::kNoCrashDialogVariable, "1");
    restore("SDL_VIDEO_DRIVER", savedDriver);
    restore("DISPLAY", savedDisplay);
    restore("WAYLAND_DISPLAY", savedWayland);
}

TEST_CASE("executable_path_is_this_test", "[crash]")
{
    const std::filesystem::path self = openhorizon::currentExecutablePath();
    CHECK(std::filesystem::is_regular_file(self));
    CHECK(openhorizon::pathToUtf8(self.stem()) == "tst_crash");
}

namespace {

/// The child: logs a line, installs the handlers (twice, as a second
/// install must change nothing) and crashes as asked.
int crashChild(const std::vector<std::string>& arguments)
{
#ifndef _WIN32
    // Crashing on purpose: no core dump for the system to keep.
    prctl(PR_SET_DUMPABLE, 0);
#endif
    const std::string& kind = arguments.at(2);
    const std::filesystem::path log = openhorizon::pathFromUtf8(arguments.at(3));
    openhorizon::openLogFile(log);
    openhorizon::logInfo("crash test child ready");
    openhorizon::CrashHandlerOptions options;
    options.logPath = log;
    options.minidumpPath = openhorizon::crashDumpPath(log);
    if (arguments.at(4) == "1") {
        options.reporter = openhorizon::currentExecutablePath();
    }
    openhorizon::installCrashHandlers(options);
    openhorizon::installCrashHandlers(options);
    if (kind == "null") {
        int* volatile target = nullptr;
        *target = 1;
    } else if (kind == "abort") {
        std::abort();
    } else if (kind == "exception") {
        throw std::runtime_error("boom from tst_crash");
    } else if (kind == "dataerror") {
        throw openhorizon::DataError("media/missing.xml: cannot open");
    }
    return 0;
}

/// The reporter a crashed child starts: writes its arguments, one per
/// line, where the test looks for them. Written aside and renamed so the
/// test never reads half of it.
int recordReport(const std::vector<std::string>& arguments)
{
    const std::optional<std::string> out = openhorizon::environmentVariable(kReportOutVariable);
    if (!out) {
        return 1;
    }
    std::string text;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        text += arguments[i] + '\n';
    }
    const std::filesystem::path target = openhorizon::pathFromUtf8(*out);
    std::filesystem::path partial = target;
    partial += ".part";
    if (!openhorizon::writeFileToDisk(partial, text)) {
        return 1;
    }
    std::filesystem::rename(partial, target);
    return 0;
}

} // namespace

// The crash kinds throw out of main on purpose: that is the uncaught
// exception under test.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = openhorizon::commandLineArguments(argc, argv);
    if (arguments.size() >= 5 && arguments[1] == kChildArgument) {
        return crashChild(arguments);
    }
    if (openhorizon::parseCrashReportArguments(arguments)) {
        return recordReport(arguments);
    }
    const int result = openhorizon::test::runTests(argc, argv);
    std::error_code ignored;
    std::filesystem::remove_all(scratchFolder(), ignored);
    return result;
}
