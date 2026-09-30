#include "Vfs.h"

#include "base/Host.h"
#include "base/Log.h"

#include <lzx/zip.h>
#include <pugixml.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace openhorizon {

namespace {

Bytes readHostFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DataError("cannot open {}", pathToUtf8(path));
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    Bytes data(size);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        throw DataError("cannot read {}", pathToUtf8(path));
    }
    return data;
}

/// The parent of a normalized path, or empty at the top.
std::string parentKey(const std::string& key)
{
    const std::size_t slash = key.rfind('/');
    return slash == std::string::npos ? std::string() : key.substr(0, slash);
}

} // namespace

Vfs::Vfs()
{
    raiseOpenFileLimit();
}

Vfs::~Vfs() = default;

std::string Vfs::normalize(std::string_view path)
{
    std::string_view rest = path;
    const std::size_t colon = rest.find(':');
    // npos is never below 16, so this also covers "no colon".
    if (colon < 16) {
        rest.remove_prefix(colon + 1);
    }
    std::string key;
    key.reserve(rest.size());
    for (char c : rest) {
        if (c == '\\') {
            c = '/';
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c == '/' && (key.empty() || key.back() == '/')) {
            continue;
        }
        key.push_back(c);
    }
    while (!key.empty() && key.back() == '/') {
        key.pop_back();
    }
    // "./" segments appear in a few of the game's own path strings.
    std::size_t dot = 0;
    while ((dot = key.find("/./")) != std::string::npos) {
        key.erase(dot, 2);
    }
    if (key.starts_with("./")) {
        key.erase(0, 2);
    }
    return key;
}

void Vfs::indexLooseFiles(
    const std::filesystem::path& root, std::unordered_map<std::string, std::filesystem::path>& into)
{
    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::follow_directory_symlink, error);
         it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error) {
            logWarning("vfs: while indexing {}: {}", pathToUtf8(root), error.message());
            break;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        // normalize() turns Windows' backslashes into the game's slashes.
        const std::string relative = pathToUtf8(std::filesystem::relative(it->path(), root, error));
        into.try_emplace(normalize(relative), it->path());
    }
}

void Vfs::mountDisc(const std::filesystem::path& root)
{
    std::filesystem::path discRoot;
    if (std::filesystem::is_directory(root / "media")) {
        discRoot = root;
    } else if (std::filesystem::is_regular_file(root / "zipmanifest.xml")) {
        // The media folder itself: its files are addressed as media/...
        discRoot = root.parent_path();
    } else {
        throw DataError("{} holds neither a media folder nor zipmanifest.xml", pathToUtf8(root));
    }
    m_discRoot = discRoot;

    std::unordered_map<std::string, std::filesystem::path> files;
    if (std::filesystem::is_directory(root / "media")) {
        indexLooseFiles(discRoot, files);
    } else {
        std::unordered_map<std::string, std::filesystem::path> media;
        indexLooseFiles(root, media);
        for (auto& [key, path] : media) {
            files.emplace("media/" + key, std::move(path));
        }
    }
    m_discFiles = std::move(files);
    logInfo("vfs: {} loose files under {}", m_discFiles.size(), pathToUtf8(discRoot));

    const auto manifestFile = m_discFiles.find("media/zipmanifest.xml");
    if (manifestFile == m_discFiles.end()) {
        throw DataError("{} has no media/zipmanifest.xml", pathToUtf8(discRoot));
    }
    pugi::xml_document manifest;
    const pugi::xml_parse_result parsed = manifest.load_file(manifestFile->second.c_str());
    if (!parsed) {
        throw DataError("{}: {}", pathToUtf8(manifestFile->second), parsed.description());
    }
    int mounted = 0;
    for (const pugi::xml_node zip : manifest.child("ZipFiles").children("Zip")) {
        const std::string key = normalize(zip.attribute("path").as_string());
        const auto file = m_discFiles.find(key);
        if (file == m_discFiles.end()) {
            logDebug("vfs: manifest names {}, which the disc lacks", key);
            continue;
        }
        if (!endsWithNoCase(key, ".zip")) {
            continue;
        }
        mountArchive(file->second, key.substr(0, key.size() - 4), zip.attribute("priority").as_int(0));
        ++mounted;
    }
    logInfo("vfs: {} archives mounted from the manifest", mounted);
}

void Vfs::addOverlay(const std::filesystem::path& root)
{
    std::unordered_map<std::string, std::filesystem::path> files;
    indexLooseFiles(root, files);
    logInfo("vfs: overlay {} with {} files", pathToUtf8(root), files.size());
    m_overlays.insert(m_overlays.begin(), std::move(files));
}

void Vfs::mountArchive(const std::filesystem::path& archive, std::string_view mountPoint, int priority)
{
    auto mount = std::make_unique<Mount>();
    mount->file = archive;
    mount->mountPoint = normalize(mountPoint);
    mount->priority = priority;
    auto& list = m_mounts[mount->mountPoint];
    list.push_back(std::move(mount));
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a->priority > b->priority; });
}

const lzx::ZipArchive* Vfs::open(const Mount& mount) const
{
    const std::lock_guard lock(m_openMutex);
    if (!mount.archive && !mount.failed) {
        try {
            mount.archive = std::make_shared<const lzx::ZipArchive>(lzx::ZipArchive::open(mount.file));
            logDebug("vfs: opened {} ({} entries)", pathToUtf8(mount.file), mount.archive->entries().size());
        } catch (const std::exception& e) {
            logError("vfs: cannot open {}: {}", pathToUtf8(mount.file), e.what());
            mount.failed = true;
        }
    }
    return mount.archive.get();
}

std::vector<std::pair<const Vfs::Mount*, std::string>> Vfs::candidates(const std::string& key) const
{
    std::vector<std::pair<const Mount*, std::string>> out;
    std::string prefix = key;
    while (!prefix.empty()) {
        prefix = parentKey(prefix);
        const auto it = m_mounts.find(prefix);
        if (it != m_mounts.end()) {
            const std::string inner = prefix.empty() ? key : key.substr(prefix.size() + 1);
            for (const auto& mount : it->second) {
                out.emplace_back(mount.get(), inner);
            }
        }
    }
    std::stable_sort(
        out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first->priority > b.first->priority; });
    return out;
}

std::optional<std::filesystem::path> Vfs::hostPath(std::string_view path) const
{
    const std::string key = normalize(path);
    for (const auto& overlay : m_overlays) {
        const auto it = overlay.find(key);
        if (it != overlay.end()) {
            return it->second;
        }
    }
    const auto it = m_discFiles.find(key);
    if (it != m_discFiles.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool Vfs::exists(std::string_view path) const
{
    const std::string key = normalize(path);
    if (hostPath(key)) {
        return true;
    }
    for (const auto& [mount, inner] : candidates(key)) {
        const lzx::ZipArchive* archive = open(*mount);
        if (archive && archive->find(inner)) {
            return true;
        }
    }
    return false;
}

std::optional<Bytes> Vfs::read(std::string_view path) const
{
    const std::string key = normalize(path);
    if (const auto host = hostPath(key)) {
        return readHostFile(*host);
    }
    for (const auto& [mount, inner] : candidates(key)) {
        const lzx::ZipArchive* archive = open(*mount);
        if (!archive) {
            continue;
        }
        if (const lzx::ZipEntry* entry = archive->find(inner)) {
            try {
                return archive->read(*entry);
            } catch (const std::exception& e) {
                throw DataError("{} in {}: {}", inner, pathToUtf8(mount->file), e.what());
            }
        }
    }
    return std::nullopt;
}

Bytes Vfs::readOrThrow(std::string_view path) const
{
    std::optional<Bytes> data = read(path);
    if (!data) {
        throw DataError("game file not found: {}", path);
    }
    return std::move(*data);
}

std::optional<std::string> Vfs::readText(std::string_view path) const
{
    std::optional<Bytes> data = read(path);
    if (!data) {
        return std::nullopt;
    }
    std::string text(data->begin(), data->end());
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return text;
}

std::vector<std::string> Vfs::list(std::string_view folder) const
{
    const std::string key = normalize(folder);
    const std::string prefix = key.empty() ? std::string() : key + "/";
    std::set<std::string> found;
    auto addLoose = [&](const std::unordered_map<std::string, std::filesystem::path>& files) {
        for (const auto& [name, host] : files) {
            if (name.starts_with(prefix)) {
                found.insert(name);
            }
        }
    };
    for (const auto& overlay : m_overlays) {
        addLoose(overlay);
    }
    addLoose(m_discFiles);
    for (const auto& [mountPoint, mounts] : m_mounts) {
        const bool mountInside = mountPoint.starts_with(prefix) || key.empty();
        const bool folderInside = key == mountPoint || key.starts_with(mountPoint + "/");
        if (!mountInside && !folderInside) {
            continue;
        }
        for (const auto& mount : mounts) {
            const lzx::ZipArchive* archive = open(*mount);
            if (!archive) {
                continue;
            }
            for (const lzx::ZipEntry& entry : archive->entries()) {
                if (entry.isDirectory()) {
                    continue;
                }
                std::string full = normalize(mountPoint + "/" + entry.path());
                if (full.starts_with(prefix)) {
                    found.insert(std::move(full));
                }
            }
        }
    }
    return {found.begin(), found.end()};
}

std::shared_ptr<const lzx::ZipArchive> Vfs::archiveAt(std::string_view mountPoint) const
{
    const auto it = m_mounts.find(normalize(mountPoint));
    if (it == m_mounts.end() || it->second.empty()) {
        return nullptr;
    }
    open(*it->second.front());
    const std::lock_guard lock(m_openMutex);
    return it->second.front()->archive;
}

} // namespace openhorizon
