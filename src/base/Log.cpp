#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace fh {

namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;
const auto g_start = std::chrono::steady_clock::now();

const char* levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Error:
        return "error";
    }
    return "?";
}

} // namespace

void setLogLevel(LogLevel level)
{
    g_level = level;
}

LogLevel logLevel()
{
    return g_level;
}

void logLine(LogLevel level, std::string_view message)
{
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "[%9.3f] %-7s %.*s\n", seconds, levelName(level), static_cast<int>(message.size()),
        message.data());
}

} // namespace fh
