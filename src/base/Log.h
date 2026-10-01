#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace openhorizon {

enum class LogLevel { Debug, Info, Warning, Error };

/// Messages below this level are dropped from standard error.
void setLogLevel(LogLevel level);
LogLevel logLevel();

/// Also writes every message, debug ones included, to `path`, replacing the
/// file. Returns false (and logs why) if it cannot be opened.
bool openLogFile(const std::filesystem::path& path);

/// `openhorizon.log` in the State user folder (see UserFolder).
std::filesystem::path defaultLogPath();

/// The open log file's descriptor (C runtime's on Windows), or -1; lets the
/// crash handler write without taking a lock.
int logFileDescriptor() noexcept;

/// Flushes the log file unless another thread is writing to it. Windows'
/// C runtime fully buffers it even when asked for line buffering.
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
