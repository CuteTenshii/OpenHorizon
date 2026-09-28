#pragma once

#include <format>
#include <string_view>

namespace fh {

enum class LogLevel { Debug, Info, Warning, Error };

/// Messages below this level are dropped. Set from the command line or the
/// FH1_LOG environment variable.
void setLogLevel(LogLevel level);
LogLevel logLevel();

/// Writes one line to standard error, prefixed with the time since start and
/// the level. Safe to call from any thread.
void logLine(LogLevel level, std::string_view message);

template <typename... Args> void logDebug(std::format_string<Args...> format, Args&&... args)
{
    if (logLevel() <= LogLevel::Debug) {
        logLine(LogLevel::Debug, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logInfo(std::format_string<Args...> format, Args&&... args)
{
    if (logLevel() <= LogLevel::Info) {
        logLine(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logWarning(std::format_string<Args...> format, Args&&... args)
{
    if (logLevel() <= LogLevel::Warning) {
        logLine(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args> void logError(std::format_string<Args...> format, Args&&... args)
{
    logLine(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
}

} // namespace fh
