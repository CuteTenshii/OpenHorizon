#include "Host.h"

#include "Error.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openhorizon {

namespace {

#ifdef _WIN32
constexpr std::string_view kFolderName = "OpenHorizon";
#else
constexpr std::string_view kFolderName = "openhorizon";
#endif

#ifdef _WIN32

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length
        = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string lastErrorMessage()
{
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length
        = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string message = length > 0 ? narrow(std::wstring_view(buffer, length)) : std::format("error {}", code);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

std::filesystem::path knownFolder(const KNOWNFOLDERID& id)
{
    wchar_t* folder = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &folder)) && folder) {
        path = folder;
    }
    CoTaskMemFree(folder);
    return path;
}

/// The variable first, so a test (or a player with a moved profile) can
/// redirect it; the known folder when it is unset or not absolute.
std::filesystem::path appDataFolder(const char* variable, const KNOWNFOLDERID& id)
{
    if (const std::optional<std::string> value = environmentVariable(variable)) {
        std::filesystem::path path = pathFromUtf8(*value);
        if (path.is_absolute()) {
            return path;
        }
    }
    return knownFolder(id);
}

#else

std::string errnoMessage()
{
    return std::strerror(errno);
}

#endif

} // namespace

std::filesystem::path pathFromUtf8(std::string_view text)
{
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const char c : text) {
        utf8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(utf8);
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    std::string out;
    out.reserve(utf8.size());
    for (const char8_t c : utf8) {
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::optional<std::string> environmentVariable(const char* name)
{
#ifdef _WIN32
    const std::wstring wideName = widen(name);
    const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed <= 1) {
        return std::nullopt;
    }
    std::wstring value(needed, L'\0');
    const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(), needed);
    value.resize(length);
    if (value.empty()) {
        return std::nullopt;
    }
    return narrow(value);
#else
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

void setEnvironmentVariable(const char* name, std::string_view value, bool overwrite)
{
    if (!overwrite && environmentVariable(name)) {
        return;
    }
#ifdef _WIN32
    // _wputenv_s updates both the C runtime's copy, which getenv reads, and
    // the process environment, which SDL and child processes read.
    _wputenv_s(widen(name).c_str(), widen(value).c_str());
#else
    ::setenv(name, std::string(value).c_str(), 1);
#endif
}

void unsetEnvironmentVariable(const char* name)
{
#ifdef _WIN32
    // An empty value removes the variable.
    _wputenv_s(widen(name).c_str(), L"");
#else
    ::unsetenv(name);
#endif
}

std::uint32_t processId()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::filesystem::path currentExecutablePath()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        // A full buffer means the name was cut short: grow and ask again.
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::error_code error;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : path;
#endif
}

std::vector<std::string> commandLineArguments(int argc, char** argv)
{
#ifdef _WIN32
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide) {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            arguments.push_back(narrow(wide[i]));
        }
        LocalFree(wide);
        return arguments;
    }
#endif
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    for (int i = 0; i < argc; ++i) {
        arguments.emplace_back(argv[i]);
    }
    return arguments;
}

void raiseOpenFileLimit()
{
#ifdef _WIN32
    constexpr int kRuntimeMaximum = 8192;
    _setmaxstdio(kRuntimeMaximum);
#endif
}

std::filesystem::path xdgBaseDirectory(const char* value, const char* home, std::string_view homeRelative)
{
    if (value && *value) {
        std::filesystem::path path = pathFromUtf8(value);
        if (path.is_absolute()) {
            return path;
        }
    }
    if (home && *home) {
        std::filesystem::path path = pathFromUtf8(home);
        if (path.is_absolute()) {
            return path / pathFromUtf8(homeRelative);
        }
    }
    return {};
}

const char* userFolderVariable(UserFolder kind)
{
#ifdef _WIN32
    return kind == UserFolder::Config ? "APPDATA" : "LOCALAPPDATA";
#else
    switch (kind) {
    case UserFolder::Config:
        return "XDG_CONFIG_HOME";
    case UserFolder::Data:
        return "XDG_DATA_HOME";
    case UserFolder::State:
        return "XDG_STATE_HOME";
    case UserFolder::Cache:
        return "XDG_CACHE_HOME";
    }
    return "XDG_CONFIG_HOME";
#endif
}

std::filesystem::path userFolder(UserFolder kind)
{
#ifdef _WIN32
    const std::filesystem::path base = kind == UserFolder::Config
        ? appDataFolder(userFolderVariable(kind), FOLDERID_RoamingAppData)
        : appDataFolder(userFolderVariable(kind), FOLDERID_LocalAppData);
    if (base.empty()) {
        return {};
    }
    const std::filesystem::path folder = base / pathFromUtf8(kFolderName);
    switch (kind) {
    case UserFolder::State:
        return folder / "logs";
    case UserFolder::Cache:
        return folder / "cache";
    case UserFolder::Config:
    case UserFolder::Data:
        break;
    }
    return folder;
#else
    std::string_view homeRelative = ".config";
    switch (kind) {
    case UserFolder::Config:
        break;
    case UserFolder::Data:
        homeRelative = ".local/share";
        break;
    case UserFolder::State:
        homeRelative = ".local/state";
        break;
    case UserFolder::Cache:
        homeRelative = ".cache";
        break;
    }
    const std::optional<std::string> value = environmentVariable(userFolderVariable(kind));
    const std::optional<std::string> home = environmentVariable("HOME");
    const std::filesystem::path base
        = xdgBaseDirectory(value ? value->c_str() : nullptr, home ? home->c_str() : nullptr, homeRelative);
    return base.empty() ? base : base / pathFromUtf8(kFolderName);
#endif
}

std::filesystem::path systemFontFolder()
{
#ifdef _WIN32
    return knownFolder(FOLDERID_Fonts);
#else
    return {};
#endif
}

bool writeFileToDisk(const std::filesystem::path& path, std::string_view content, std::string* error)
{
    const auto failed = [&](std::string_view step, const std::string& reason) {
        if (error) {
            *error = std::format("cannot {} {}: {}", step, pathToUtf8(path), reason);
        }
        return false;
    };
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return failed("create", lastErrorMessage());
    }
    while (!content.empty()) {
        constexpr std::size_t kMaxChunk = 1U << 30U;
        const auto want = static_cast<DWORD>(std::min(content.size(), kMaxChunk));
        DWORD written = 0;
        if (!WriteFile(handle, content.data(), want, &written, nullptr)) {
            const std::string reason = lastErrorMessage();
            CloseHandle(handle);
            return failed("write", reason);
        }
        content.remove_prefix(written);
    }
    if (!FlushFileBuffers(handle)) {
        const std::string reason = lastErrorMessage();
        CloseHandle(handle);
        return failed("flush", reason);
    }
    if (!CloseHandle(handle)) {
        return failed("close", lastErrorMessage());
    }
    return true;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return failed("create", errnoMessage());
    }
    while (!content.empty()) {
        const ssize_t written = ::write(fd, content.data(), content.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string reason = errnoMessage();
            ::close(fd);
            return failed("write", reason);
        }
        content.remove_prefix(static_cast<std::size_t>(written));
    }
    if (::fsync(fd) != 0) {
        const std::string reason = errnoMessage();
        ::close(fd);
        return failed("flush", reason);
    }
    if (::close(fd) != 0) {
        return failed("close", errnoMessage());
    }
    return true;
#endif
}

ReadOnlyFile::ReadOnlyFile(const std::filesystem::path& path)
    : m_name(pathToUtf8(path))
{
#ifdef _WIN32
    // FILE_SHARE_READ so the game can stay open while a tool reads the same
    // bank, and no FILE_FLAG_OVERLAPPED: a synchronous handle still takes
    // an offset per ReadFile call, which is all readAt needs.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw DataError("{}: cannot open: {}", m_name, lastErrorMessage());
    }
    m_handle = handle;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        const std::string message = lastErrorMessage();
        CloseHandle(handle);
        m_handle = nullptr;
        throw DataError("{}: cannot read its size: {}", m_name, message);
    }
    m_size = static_cast<std::uint64_t>(size.QuadPart);
#else
    m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (m_fd < 0) {
        throw DataError("{}: cannot open: {}", m_name, errnoMessage());
    }
    struct stat info{};
    if (::fstat(m_fd, &info) != 0) {
        const std::string message = errnoMessage();
        ::close(m_fd);
        m_fd = -1;
        throw DataError("{}: cannot stat: {}", m_name, message);
    }
    m_size = static_cast<std::uint64_t>(info.st_size);
#endif
}

ReadOnlyFile::~ReadOnlyFile()
{
#ifdef _WIN32
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
#else
    if (m_fd >= 0) {
        ::close(m_fd);
    }
#endif
}

std::size_t ReadOnlyFile::readAt(std::uint64_t offset, std::span<std::uint8_t> out) const
{
    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t position = offset + done;
#ifdef _WIN32
        // ReadFile takes a DWORD count, so large reads go in pieces.
        constexpr std::size_t kMaxChunk = 1U << 30U;
        const auto want = static_cast<DWORD>(std::min(out.size() - done, kMaxChunk));
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(position & 0xffffffffU);
        at.OffsetHigh = static_cast<DWORD>(position >> 32U);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(m_handle), out.data() + done, want, &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            throw DataError("{}: read failed at 0x{:x}: {}", m_name, position, lastErrorMessage());
        }
#else
        const ssize_t got = ::pread(m_fd, out.data() + done, out.size() - done, static_cast<off_t>(position));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw DataError("{}: read failed at 0x{:x}: {}", m_name, position, errnoMessage());
        }
#endif
        if (got == 0) {
            break;
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

} // namespace openhorizon
