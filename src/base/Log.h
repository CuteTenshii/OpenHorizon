#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace openhorizon {

enum class LogLevel { Debug, Info, Warning, Error };

/// Messages below this level are dropped from standard error. Set from the
/// command line (`--debug`).
void setLogLevel(LogLevel level);
LogLevel logLevel();

/// Also writes every message, debug ones included, to `path`, replacing the
/// file. A problem report then carries the whole story even when the console
/// showed only the summary. Returns false (and logs why) if the file cannot
/// be opened; logging to standard error carries on either way.
bool openLogFile(const std::filesystem::path& path);

/// `openhorizon.log` in the State user folder: `~/.local/state/openhorizon` on
/// Linux, `%LOCALAPPDATA%\OpenHorizon\logs` on Windows (see UserFolder).
std::filesystem::path defaultLogPath();

/// The open log file's descriptor (a C runtime descriptor on Windows), or
/// -1 when none is open. For the crash handler, which writes its report
/// through the descriptor because a signal handler may take no lock.
int logFileDescriptor() noexcept;

/// Writes out whatever the log file still holds in memory, unless another
/// thread is writing to it at that moment. For the crash handler outside a
/// signal handler: Windows' C runtime fully buffers the file even when
/// asked for line buffering, so its last lines would otherwise be lost.
void tryFlushLogFile() noexcept;

/// True if a message at `level` goes anywhere.
bool logEnabled(LogLevel level);

/// Writes one line to standard error and the log file, prefixed with the
/// time since start and the level. Safe to call from any thread.
void logLine(LogLevel level, std::string_view message);

template <typename... Args> void logDebug(std::format_string<Args...> format, Args&&... args)
{
    if (logEnabled(LogLevel::Debug)) {
        logLine(LogLevel::Debug, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logInfo(std::format_string<Args...> format, Args&&... args)
{
    if (logEnabled(LogLevel::Info)) {
        logLine(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logWarning(std::format_string<Args...> format, Args&&... args)
{
    if (logEnabled(LogLevel::Warning)) {
        logLine(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logError(std::format_string<Args...> format, Args&&... args)
{
    logLine(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
}

} // namespace openhorizon
