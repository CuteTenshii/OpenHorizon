// The host layer the rest of the code stands on: UTF-8 paths and
// environment variables, the per-user folders on Linux and Windows,
// positional file reads, settings, and the game's file system over a disc
// folder named the way the host names it (drive letters and backslashes on
// Windows). No disc needed; everything is made under the temporary folder.

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "base/Error.h"
#include "base/Host.h"
#include "base/Log.h"
#include "base/Settings.h"
#include "vfs/Vfs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

/// "café フォルダ", spelled in bytes so the test does
/// not depend on the compiler's source character set; one name no single
/// Windows code page can spell.
const std::string kUnicodeName = "caf\xc3\xa9 \xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80";

std::filesystem::path scratch(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openhorizon-tst-base" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

/// Restores a variable when the test ends, so tests do not leak into each
/// other.
class SavedVariable {
public:
    explicit SavedVariable(const char* name)
        : m_name(name)
        , m_value(openhorizon::environmentVariable(name))
    {
    }
    ~SavedVariable()
    {
        if (m_value) {
            openhorizon::setEnvironmentVariable(m_name, *m_value);
        } else {
            openhorizon::unsetEnvironmentVariable(m_name);
        }
    }
    SavedVariable(const SavedVariable&) = delete;
    SavedVariable& operator=(const SavedVariable&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_value;
};

TEST_CASE("pathsRoundTripThroughUtf8", "[base]")
{
    const std::string text = "dir/" + kUnicodeName + "/file.txt";
    const std::filesystem::path path = openhorizon::pathFromUtf8(text);
    CHECK(openhorizon::pathToUtf8(path.filename()) == std::string("file.txt"));
    CHECK(openhorizon::pathToUtf8(path.parent_path().filename()) == kUnicodeName);
    CHECK(openhorizon::pathToUtf8(openhorizon::pathFromUtf8(kUnicodeName)) == kUnicodeName);
    CHECK(openhorizon::pathFromUtf8("").empty());
}

TEST_CASE("filesWithUnicodeNamesOpen", "[base]")
{
    const std::filesystem::path dir = scratch("unicode") / openhorizon::pathFromUtf8(kUnicodeName);
    const std::filesystem::path file = dir / openhorizon::pathFromUtf8(kUnicodeName + ".bin");
    writeFile(file, "0123456789");
    CHECK(std::filesystem::is_regular_file(file));
    const openhorizon::ReadOnlyFile reader(file);
    CHECK(reader.size() == std::uint64_t{10});
    std::array<std::uint8_t, 4> bytes{};
    CHECK(reader.readAt(3, bytes) == std::size_t{4});
    CHECK(std::string(bytes.begin(), bytes.end()) == std::string("3456"));
}

TEST_CASE("readOnlyFileReadsAtOffsetsFromManyThreads", "[base]")
{
    const std::filesystem::path file = scratch("positional") / "data.bin";
    std::string content;
    for (int i = 0; i < 4096; ++i) {
        content.push_back(static_cast<char>(i * 7));
    }
    writeFile(file, content);
    const openhorizon::ReadOnlyFile reader(file);
    CHECK(reader.size() == std::uint64_t{4096});

    std::array<std::uint8_t, 16> tail{};
    CHECK(reader.readAt(4090, tail) == std::size_t{6});
    CHECK(reader.readAt(4096, tail) == std::size_t{0});
    CHECK(reader.readAt(100000, tail) == std::size_t{0});

    // Each thread reads its own offsets with no seek to share.
    std::vector<int> mismatches(4, 0);
    std::vector<std::thread> threads;
    threads.reserve(mismatches.size());
    for (std::size_t t = 0; t < mismatches.size(); ++t) {
        threads.emplace_back([&, t] {
            std::array<std::uint8_t, 32> chunk{};
            for (std::size_t offset = t * 17; offset + chunk.size() <= content.size(); offset += 61) {
                if (reader.readAt(offset, chunk) != chunk.size()) {
                    ++mismatches[t];
                    continue;
                }
                for (std::size_t i = 0; i < chunk.size(); ++i) {
                    if (chunk[i] != static_cast<std::uint8_t>(content[offset + i])) {
                        ++mismatches[t];
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const int count : mismatches) {
        CHECK(count == 0);
    }
}

TEST_CASE("filesWrittenToDiskReplaceTheOldContent", "[base]")
{
    const std::filesystem::path file = scratch("write") / openhorizon::pathFromUtf8(kUnicodeName + ".xml");
    CHECK(openhorizon::writeFileToDisk(file, "a longer first version"));
    CHECK(openhorizon::writeFileToDisk(file, "second"));
    const openhorizon::ReadOnlyFile reader(file);
    std::array<std::uint8_t, 32> bytes{};
    const std::size_t read = reader.readAt(0, bytes);
    CHECK(std::string(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read)) == std::string("second"));

    std::string error;
    CHECK(!openhorizon::writeFileToDisk(file.parent_path() / "no such folder" / "file", "x", &error));
    CHECK(error.find("no such folder") != std::string::npos);
}

TEST_CASE("moreFilesStayOpenThanTheDiscHasArchives", "[base]")
{
    // The disc's 524 archives each keep a stream open once read; the
    // Windows runtime's default of 512 streams ran out before this.
    openhorizon::raiseOpenFileLimit();
    const std::filesystem::path dir = scratch("open");
    constexpr int kFiles = 600;
    std::vector<std::ifstream> streams;
    streams.reserve(kFiles);
    int failed = 0;
    for (int i = 0; i < kFiles; ++i) {
        const std::filesystem::path file = dir / std::format("{}.bin", i);
        writeFile(file, "x");
        streams.emplace_back(file, std::ios::binary);
        failed += streams.back().is_open() ? 0 : 1;
    }
    CHECK(failed == 0);
}

TEST_CASE("missingFilesThrowWithTheirName", "[base]")
{
    const std::filesystem::path file = scratch("missing") / "absent.bin";
    bool threw = false;
    try {
        const openhorizon::ReadOnlyFile reader(file);
    } catch (const openhorizon::DataError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("absent.bin") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("environmentVariablesCarryUtf8", "[base]")
{
    const char* name = "OPENHORIZON_TST_BASE_VARIABLE";
    const SavedVariable saved(name);
    openhorizon::unsetEnvironmentVariable(name);
    CHECK(!openhorizon::environmentVariable(name));
    openhorizon::setEnvironmentVariable(name, kUnicodeName);
    CHECK(openhorizon::environmentVariable(name).value_or("") == kUnicodeName);
    openhorizon::setEnvironmentVariable(name, "other", false);
    CHECK(openhorizon::environmentVariable(name).value_or("") == kUnicodeName);
    openhorizon::setEnvironmentVariable(name, "other");
    CHECK(openhorizon::environmentVariable(name).value_or("") == std::string("other"));
    openhorizon::unsetEnvironmentVariable(name);
    CHECK(!openhorizon::environmentVariable(name));
}

TEST_CASE("xdgBaseDirectoryFollowsTheRules", "[base]")
{
    // Absolute on every platform, so the rule itself is what is tested.
    const std::filesystem::path root = std::filesystem::temp_directory_path();
    const std::string cache = openhorizon::pathToUtf8(root / "cache");
    const std::string home = openhorizon::pathToUtf8(root / "home");
    CHECK(openhorizon::xdgBaseDirectory(cache.c_str(), home.c_str(), ".cache") == root / "cache");
    CHECK(openhorizon::xdgBaseDirectory(nullptr, home.c_str(), ".cache") == root / "home" / ".cache");
    CHECK(openhorizon::xdgBaseDirectory("", home.c_str(), ".local/share") == root / "home" / ".local/share");
    // Relative values are to be ignored.
    CHECK(openhorizon::xdgBaseDirectory("relative", home.c_str(), ".cache") == root / "home" / ".cache");
    CHECK(openhorizon::xdgBaseDirectory(nullptr, nullptr, ".cache").empty());
    CHECK(openhorizon::xdgBaseDirectory("relative", "also/relative", ".cache").empty());
}

#ifdef _WIN32
constexpr const char* kFolderName = "OpenHorizon";
#else
constexpr const char* kFolderName = "openhorizon";
#endif

TEST_CASE("userFoldersFollowTheirVariables", "[base]")
{
    const std::filesystem::path base = scratch("folders") / openhorizon::pathFromUtf8(kUnicodeName);
    for (const openhorizon::UserFolder kind : {openhorizon::UserFolder::Config, openhorizon::UserFolder::Data,
             openhorizon::UserFolder::State, openhorizon::UserFolder::Cache}) {
        const char* variable = openhorizon::userFolderVariable(kind);
        const SavedVariable saved(variable);
        openhorizon::setEnvironmentVariable(variable, openhorizon::pathToUtf8(base));
        const std::filesystem::path folder = openhorizon::userFolder(kind);
        CHECK(folder.native().starts_with((base / kFolderName).native()));
    }
#ifdef _WIN32
    const SavedVariable roaming("APPDATA");
    const SavedVariable local("LOCALAPPDATA");
    openhorizon::setEnvironmentVariable("APPDATA", openhorizon::pathToUtf8(base / "Roaming"));
    openhorizon::setEnvironmentVariable("LOCALAPPDATA", openhorizon::pathToUtf8(base / "Local"));
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Config) == base / "Roaming" / "OpenHorizon");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Data) == base / "Local" / "OpenHorizon");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::State) == base / "Local" / "OpenHorizon" / "logs");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Cache) == base / "Local" / "OpenHorizon" / "cache");
    // A relative value falls back to the known folder rather than the
    // working directory.
    openhorizon::setEnvironmentVariable("LOCALAPPDATA", "relative");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Cache).is_absolute());
#else
    const SavedVariable home("HOME");
    const SavedVariable config("XDG_CONFIG_HOME");
    const SavedVariable data("XDG_DATA_HOME");
    const SavedVariable state("XDG_STATE_HOME");
    const SavedVariable cache("XDG_CACHE_HOME");
    for (const char* variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"}) {
        openhorizon::unsetEnvironmentVariable(variable);
    }
    openhorizon::setEnvironmentVariable("HOME", openhorizon::pathToUtf8(base));
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Config) == base / ".config" / "openhorizon");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Data) == base / ".local/share" / "openhorizon");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::State) == base / ".local/state" / "openhorizon");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Cache) == base / ".cache" / "openhorizon");
    openhorizon::unsetEnvironmentVariable("HOME");
    CHECK(openhorizon::userFolder(openhorizon::UserFolder::Cache).empty());
#endif
}

TEST_CASE("settingsAndLogLiveInTheirUserFolders", "[base]")
{
    const std::filesystem::path base = scratch("settings") / openhorizon::pathFromUtf8(kUnicodeName);
    const SavedVariable config(openhorizon::userFolderVariable(openhorizon::UserFolder::Config));
    const SavedVariable state(openhorizon::userFolderVariable(openhorizon::UserFolder::State));
    openhorizon::setEnvironmentVariable(
        openhorizon::userFolderVariable(openhorizon::UserFolder::Config), openhorizon::pathToUtf8(base / "config"));
    openhorizon::setEnvironmentVariable(
        openhorizon::userFolderVariable(openhorizon::UserFolder::State), openhorizon::pathToUtf8(base / "state"));
    CHECK(openhorizon::Settings::defaultPath()
        == openhorizon::userFolder(openhorizon::UserFolder::Config) / "settings.ini");
    CHECK(openhorizon::defaultLogPath() == openhorizon::userFolder(openhorizon::UserFolder::State) / "openhorizon.log");

    // A game folder named in UTF-8 survives a save and a load.
    const std::string gameDir = openhorizon::pathToUtf8(base / "disc");
    openhorizon::Settings settings = openhorizon::Settings::load();
    settings.set("game", gameDir);
    CHECK(settings.save());
    CHECK(std::filesystem::is_regular_file(openhorizon::Settings::defaultPath()));
    CHECK(openhorizon::Settings::load().get("game", "") == gameDir);
}

TEST_CASE("vfsMatchesGamePathsOnHostPaths", "[base]")
{
    const std::filesystem::path disc = scratch("vfs") / openhorizon::pathFromUtf8(kUnicodeName);
    // mountDisc() looks for these two names as the disc has them; below
    // them any case goes.
    writeFile(disc / "media" / "zipmanifest.xml", "<ZipFiles></ZipFiles>");
    writeFile(disc / "media" / "Tracks" / "Colorado" / "Info.XML", "<Track/>");

    // The folder as a player would type it: on Windows with the drive
    // letter and backslashes, on Linux as it is.
#ifdef _WIN32
    std::wstring typed = std::filesystem::absolute(disc).native();
    for (wchar_t& c : typed) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    const std::filesystem::path root(typed + L"\\");
    CHECK(root.has_root_name());
#else
    const std::filesystem::path& root = disc;
#endif
    openhorizon::Vfs vfs;
    vfs.mountDisc(root);
    CHECK(vfs.exists("game:\\media\\tracks\\colorado\\info.xml"));
    CHECK(vfs.exists("media/TRACKS/colorado/INFO.xml"));
    CHECK(vfs.readText("Game:\\Media\\Tracks\\Colorado\\Info.xml").value_or("") == std::string("<Track/>"));
    const std::optional<std::filesystem::path> host = vfs.hostPath("media/tracks/colorado/info.xml");
    CHECK(host.has_value());
    if (host) {
        CHECK(std::filesystem::equivalent(*host, disc / "media" / "Tracks" / "Colorado" / "Info.XML"));
    }
    const std::vector<std::string> files = vfs.list("media/tracks");
    CHECK(files.size() == std::size_t{1});
    if (!files.empty()) {
        CHECK(files.front() == std::string("media/tracks/colorado/info.xml"));
    }

    // The media folder itself works as the root too.
    openhorizon::Vfs media;
    media.mountDisc(disc / "media");
    CHECK(media.exists("media/tracks/colorado/info.xml"));
}

/// Removes the folders the cases made once they have all run.
class ScratchCleanup : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override
    {
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::temp_directory_path() / "openhorizon-tst-base", ignored);
    }
};

} // namespace

CATCH_REGISTER_LISTENER(ScratchCleanup)
