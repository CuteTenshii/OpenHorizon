#include "Log.h"

#include "Host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>

namespace fh {

namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
// Read on every log call without the mutex, so the check stays cheap.
std::atomic<bool> g_fileOpen{false};
std::atomic<int> g_fileDescriptor{-1};
std::mutex g_mutex;
std::FILE* g_file = nullptr;
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

struct FileCloser {
    ~FileCloser()
    {
        const std::lock_guard lock(g_mutex);
        if (g_file) {
            g_fileDescriptor = -1;
            std::fclose(g_file);
            g_file = nullptr;
            g_fileOpen = false;
        }
    }
};
const FileCloser g_closer;

} // namespace

void setLogLevel(LogLevel level)
{
    g_level = level;
}

LogLevel logLevel()
{
    return g_level;
}

int logFileDescriptor() noexcept
{
    return g_fileDescriptor;
}

void tryFlushLogFile() noexcept
{
    if (!g_mutex.try_lock()) {
        return;
    }
    if (g_file) {
        std::fflush(g_file);
    }
    g_mutex.unlock();
}

bool logEnabled(LogLevel level)
{
    return level >= g_level || g_fileOpen;
}

std::filesystem::path defaultLogPath()
{
    const std::filesystem::path folder = userFolder(UserFolder::State);
    if (folder.empty()) {
        return std::filesystem::temp_directory_path() / "fh1-native" / "fh1.log";
    }
    return folder / "fh1.log";
}

bool openLogFile(const std::filesystem::path& path)
{
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
#ifdef _WIN32
    // The wide-character open, so a folder name outside the code page works.
    std::FILE* file = _wfopen(path.c_str(), L"w");
#else
    std::FILE* file = std::fopen(path.c_str(), "w");
#endif
    if (!file) {
        logWarning("log: cannot write {}", pathToUtf8(path));
        return false;
    }
    // Line buffered so the last lines before a crash reach the disk.
    std::setvbuf(file, nullptr, _IOLBF, 0);
    {
        const std::lock_guard lock(g_mutex);
        if (g_file) {
            std::fclose(g_file);
        }
        g_file = file;
        g_fileOpen = true;
#ifdef _WIN32
        g_fileDescriptor = _fileno(file);
#else
        g_fileDescriptor = fileno(file);
#endif
    }
    logInfo("log: writing to {}", pathToUtf8(path));
    return true;
}

void logLine(LogLevel level, std::string_view message)
{
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    const std::string line = std::format("[{:9.3f}] {:<7} {}\n", seconds, levelName(level), message);
    const std::lock_guard lock(g_mutex);
    if (level >= g_level) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
    if (g_file) {
        std::fwrite(line.data(), 1, line.size(), g_file);
    }
}

} // namespace fh
