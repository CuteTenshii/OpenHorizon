#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace fh {

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

/// `$XDG_STATE_HOME/fh1-native/fh1.log`, by default
/// `~/.local/state/fh1-native/fh1.log`.
std::filesystem::path defaultLogPath();

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

} // namespace fh
