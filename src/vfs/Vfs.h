#pragma once

#include "base/Bytes.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lzx {
class ZipArchive;
struct ZipEntry;
} // namespace lzx

namespace openhorizon {

/// The extracted disc plus every `media/zipmanifest.xml` archive mounted at
/// its path minus ".zip", under any overlay folders (DLC, mods).
/// Paths ignore case, slash direction and `game:\`. Precedence: overlays,
/// loose disc files, then archives by manifest priority. Reads are
/// thread-safe.
class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    /// Mounts an extracted disc. `root` may be the disc root (holding
    /// `default.xex` and `media`) or the `media` folder itself. Throws
    /// DataError if neither layout is found.
    void mountDisc(const std::filesystem::path& root);

    /// Adds a folder whose files shadow the disc's, laid out like the disc
    /// root (a `media` folder inside).
    void addOverlay(const std::filesystem::path& root);

    /// Mounts one more archive at `mountPoint` (a game path such as
    /// "media/tracks/colorado/bin"), with the given priority.
    void mountArchive(const std::filesystem::path& archive, std::string_view mountPoint, int priority);

    /// The key a game path is looked up by: lower case, forward slashes, no
    /// device prefix, no leading or doubled slashes.
    static std::string normalize(std::string_view path);

    bool exists(std::string_view path) const;
    /// The file's contents, or nothing if no mount provides it. Throws
    /// DataError if an archive entry is found but cannot be decoded.
    std::optional<Bytes> read(std::string_view path) const;
    /// As read(), but a missing file throws DataError.
    Bytes readOrThrow(std::string_view path) const;
    std::optional<std::string> readText(std::string_view path) const;

    /// The files and archive entries directly or indirectly under `folder`
    /// (a game path), as normalized paths, sorted. Each path appears once.
    std::vector<std::string> list(std::string_view folder) const;

    /// The real file behind a loose game path, if there is one.
    std::optional<std::filesystem::path> hostPath(std::string_view path) const;

    /// The archive mounted exactly at `mountPoint`, opening it if needed.
    std::shared_ptr<const lzx::ZipArchive> archiveAt(std::string_view mountPoint) const;

    const std::filesystem::path& discRoot() const { return m_discRoot; }

private:
    struct Mount {
        std::filesystem::path file;
        std::string mountPoint;
        int priority = 0;
        mutable std::shared_ptr<const lzx::ZipArchive> archive;
        mutable bool failed = false;
    };

    void indexLooseFiles(const std::filesystem::path& root, std::unordered_map<std::string, std::filesystem::path>& into);
    const lzx::ZipArchive* open(const Mount& mount) const;
    /// The archive mounts whose mount point is a prefix of `key`, best first,
    /// with the remainder of the path inside each.
    std::vector<std::pair<const Mount*, std::string>> candidates(const std::string& key) const;

    std::filesystem::path m_discRoot;
    std::unordered_map<std::string, std::filesystem::path> m_discFiles;
    std::vector<std::unordered_map<std::string, std::filesystem::path>> m_overlays;
    /// Mounts by mount point; several archives can share one.
    std::unordered_map<std::string, std::vector<std::unique_ptr<Mount>>> m_mounts;
    mutable std::mutex m_openMutex;
};

} // namespace openhorizon
