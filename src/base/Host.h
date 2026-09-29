#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace fh {

/// Text crosses into and out of std::filesystem::path as UTF-8 on every
/// platform. On Windows a path built from a plain std::string is read in
/// the process code page, and path::string() throws for names that page
/// cannot spell, so conversions go through these two.
std::filesystem::path pathFromUtf8(std::string_view text);
std::string pathToUtf8(const std::filesystem::path& path);

/// An environment variable as UTF-8, or nothing when it is unset or empty.
std::optional<std::string> environmentVariable(const char* name);
/// Sets a variable for this process and the programs it starts; with
/// `overwrite` false an existing non-empty value is kept.
void setEnvironmentVariable(const char* name, std::string_view value, bool overwrite = true);
void unsetEnvironmentVariable(const char* name);

std::uint32_t processId();

/// Raises the number of files the process may hold open at once to what
/// the game's file system needs: a disc mounts 524 archives, and each stays
/// open once read. The Windows C runtime allows 512 stdio streams unless
/// told otherwise (std::ifstream sits on them with MSVC's runtime), so
/// there it is raised to the runtime's maximum of 8192. Linux's default of
/// 1024 descriptors is already enough, so this does nothing there. Safe to
/// call more than once.
void raiseOpenFileLimit();

/// The kinds of file fh1-native keeps for the player, each in a folder of
/// its own:
///
/// | Kind   | Holds          | Linux (XDG base directories)          | Windows                          |
/// |--------|----------------|---------------------------------------|----------------------------------|
/// | Config | settings.ini   | `$XDG_CONFIG_HOME`, `~/.config`       | `%APPDATA%\fh1-native`           |
/// | Data   | profile/       | `$XDG_DATA_HOME`, `~/.local/share`    | `%LOCALAPPDATA%\fh1-native`      |
/// | State  | fh1.log        | `$XDG_STATE_HOME`, `~/.local/state`   | `%LOCALAPPDATA%\fh1-native\logs` |
/// | Cache  | shaders/       | `$XDG_CACHE_HOME`, `~/.cache`         | `%LOCALAPPDATA%\fh1-native\cache`|
///
/// On Linux the folder is `fh1-native` inside the XDG base directory. On
/// Windows settings roam with the account (the Roaming AppData folder)
/// while everything bulky or machine-specific stays local.
enum class UserFolder { Config, Data, State, Cache };

/// The folder for `kind`, or an empty path when the environment names no
/// usable base (no absolute XDG value or HOME on Linux, no AppData folder
/// on Windows). The folder is not created.
std::filesystem::path userFolder(UserFolder kind);

/// The environment variable that moves the base of `kind`'s folder
/// (XDG_CONFIG_HOME and so on on Linux, APPDATA or LOCALAPPDATA on
/// Windows), so tests can point it at a scratch folder.
const char* userFolderVariable(UserFolder kind);

/// The XDG base directory rule on its own: `value` when it is an absolute
/// path, else `home` joined with `homeRelative` when home is absolute, else
/// empty. Relative values are ignored, as the specification requires.
std::filesystem::path xdgBaseDirectory(const char* value, const char* home, std::string_view homeRelative);

/// The system's font folder on Windows (FOLDERID_Fonts). Empty on other
/// platforms, where fontconfig finds the fonts.
std::filesystem::path systemFontFolder();

/// Creates or replaces `path` with `content` and waits until the data is on
/// the disk (fsync on POSIX, FlushFileBuffers on Windows), so a rename that
/// follows never leaves the name pointing at data still in flight. Returns
/// false with the reason in `error` (when given) if any step fails.
bool writeFileToDisk(const std::filesystem::path& path, std::string_view content, std::string* error = nullptr);

/// A read-only file read at explicit offsets, from any number of threads
/// at once without a lock (pread on POSIX, overlapped offsets on Windows).
class ReadOnlyFile {
public:
    /// Throws DataError naming the file when it cannot be opened.
    explicit ReadOnlyFile(const std::filesystem::path& path);
    ~ReadOnlyFile();
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;

    std::uint64_t size() const { return m_size; }

    /// Fills `out` from `offset` and returns how many bytes were read,
    /// fewer only at the end of the file. Throws DataError on a read error.
    std::size_t readAt(std::uint64_t offset, std::span<std::uint8_t> out) const;

private:
    std::string m_name;
    std::uint64_t m_size = 0;
#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_fd = -1;
#endif
};

} // namespace fh
