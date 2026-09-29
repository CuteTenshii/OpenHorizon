// The host layer the rest of the code stands on: UTF-8 paths and
// environment variables, the per-user folders on Linux and Windows,
// positional file reads, settings, and the game's file system over a disc
// folder named the way the host names it (drive letters and backslashes on
// Windows). No disc needed; everything is made under the temporary folder.

#include "TestHarness.h"

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
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fh1-tst-base" / name;
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
        , m_value(fh::environmentVariable(name))
    {
    }
    ~SavedVariable()
    {
        if (m_value) {
            fh::setEnvironmentVariable(m_name, *m_value);
        } else {
            fh::unsetEnvironmentVariable(m_name);
        }
    }
    SavedVariable(const SavedVariable&) = delete;
    SavedVariable& operator=(const SavedVariable&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_value;
};

TEST(pathsRoundTripThroughUtf8)
{
    const std::string text = "dir/" + kUnicodeName + "/file.txt";
    const std::filesystem::path path = fh::pathFromUtf8(text);
    CHECK_EQ(fh::pathToUtf8(path.filename()), std::string("file.txt"));
    CHECK_EQ(fh::pathToUtf8(path.parent_path().filename()), kUnicodeName);
    CHECK_EQ(fh::pathToUtf8(fh::pathFromUtf8(kUnicodeName)), kUnicodeName);
    CHECK(fh::pathFromUtf8("").empty());
}

TEST(filesWithUnicodeNamesOpen)
{
    const std::filesystem::path dir = scratch("unicode") / fh::pathFromUtf8(kUnicodeName);
    const std::filesystem::path file = dir / fh::pathFromUtf8(kUnicodeName + ".bin");
    writeFile(file, "0123456789");
    CHECK(std::filesystem::is_regular_file(file));
    const fh::ReadOnlyFile reader(file);
    CHECK_EQ(reader.size(), std::uint64_t{10});
    std::array<std::uint8_t, 4> bytes{};
    CHECK_EQ(reader.readAt(3, bytes), std::size_t{4});
    CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("3456"));
}

TEST(readOnlyFileReadsAtOffsetsFromManyThreads)
{
    const std::filesystem::path file = scratch("positional") / "data.bin";
    std::string content;
    for (int i = 0; i < 4096; ++i) {
        content.push_back(static_cast<char>(i * 7));
    }
    writeFile(file, content);
    const fh::ReadOnlyFile reader(file);
    CHECK_EQ(reader.size(), std::uint64_t{4096});

    std::array<std::uint8_t, 16> tail{};
    CHECK_EQ(reader.readAt(4090, tail), std::size_t{6});
    CHECK_EQ(reader.readAt(4096, tail), std::size_t{0});
    CHECK_EQ(reader.readAt(100000, tail), std::size_t{0});

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
        CHECK_EQ(count, 0);
    }
}

TEST(filesWrittenToDiskReplaceTheOldContent)
{
    const std::filesystem::path file = scratch("write") / fh::pathFromUtf8(kUnicodeName + ".xml");
    CHECK(fh::writeFileToDisk(file, "a longer first version"));
    CHECK(fh::writeFileToDisk(file, "second"));
    const fh::ReadOnlyFile reader(file);
    std::array<std::uint8_t, 32> bytes{};
    const std::size_t read = reader.readAt(0, bytes);
    CHECK_EQ(std::string(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read)), std::string("second"));

    std::string error;
    CHECK(!fh::writeFileToDisk(file.parent_path() / "no such folder" / "file", "x", &error));
    CHECK(error.find("no such folder") != std::string::npos);
}

TEST(moreFilesStayOpenThanTheDiscHasArchives)
{
    // The disc's 524 archives each keep a stream open once read; the
    // Windows runtime's default of 512 streams ran out before this.
    fh::raiseOpenFileLimit();
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
    CHECK_EQ(failed, 0);
}

TEST(missingFilesThrowWithTheirName)
{
    const std::filesystem::path file = scratch("missing") / "absent.bin";
    bool threw = false;
    try {
        const fh::ReadOnlyFile reader(file);
    } catch (const fh::DataError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("absent.bin") != std::string::npos);
    }
    CHECK(threw);
}

TEST(environmentVariablesCarryUtf8)
{
    const char* name = "FH1_TST_BASE_VARIABLE";
    const SavedVariable saved(name);
    fh::unsetEnvironmentVariable(name);
    CHECK(!fh::environmentVariable(name));
    fh::setEnvironmentVariable(name, kUnicodeName);
    CHECK_EQ(fh::environmentVariable(name).value_or(""), kUnicodeName);
    fh::setEnvironmentVariable(name, "other", false);
    CHECK_EQ(fh::environmentVariable(name).value_or(""), kUnicodeName);
    fh::setEnvironmentVariable(name, "other");
    CHECK_EQ(fh::environmentVariable(name).value_or(""), std::string("other"));
    fh::unsetEnvironmentVariable(name);
    CHECK(!fh::environmentVariable(name));
}

TEST(xdgBaseDirectoryFollowsTheRules)
{
    // Absolute on every platform, so the rule itself is what is tested.
    const std::filesystem::path root = std::filesystem::temp_directory_path();
    const std::string cache = fh::pathToUtf8(root / "cache");
    const std::string home = fh::pathToUtf8(root / "home");
    CHECK_EQ(fh::xdgBaseDirectory(cache.c_str(), home.c_str(), ".cache"), root / "cache");
    CHECK_EQ(fh::xdgBaseDirectory(nullptr, home.c_str(), ".cache"), root / "home" / ".cache");
    CHECK_EQ(fh::xdgBaseDirectory("", home.c_str(), ".local/share"), root / "home" / ".local/share");
    // Relative values are to be ignored.
    CHECK_EQ(fh::xdgBaseDirectory("relative", home.c_str(), ".cache"), root / "home" / ".cache");
    CHECK(fh::xdgBaseDirectory(nullptr, nullptr, ".cache").empty());
    CHECK(fh::xdgBaseDirectory("relative", "also/relative", ".cache").empty());
}

TEST(userFoldersFollowTheirVariables)
{
    const std::filesystem::path base = scratch("folders") / fh::pathFromUtf8(kUnicodeName);
    for (const fh::UserFolder kind :
        {fh::UserFolder::Config, fh::UserFolder::Data, fh::UserFolder::State, fh::UserFolder::Cache}) {
        const char* variable = fh::userFolderVariable(kind);
        const SavedVariable saved(variable);
        fh::setEnvironmentVariable(variable, fh::pathToUtf8(base));
        const std::filesystem::path folder = fh::userFolder(kind);
        CHECK(folder.native().starts_with((base / "fh1-native").native()));
    }
#ifdef _WIN32
    const SavedVariable roaming("APPDATA");
    const SavedVariable local("LOCALAPPDATA");
    fh::setEnvironmentVariable("APPDATA", fh::pathToUtf8(base / "Roaming"));
    fh::setEnvironmentVariable("LOCALAPPDATA", fh::pathToUtf8(base / "Local"));
    CHECK_EQ(fh::userFolder(fh::UserFolder::Config), base / "Roaming" / "fh1-native");
    CHECK_EQ(fh::userFolder(fh::UserFolder::Data), base / "Local" / "fh1-native");
    CHECK_EQ(fh::userFolder(fh::UserFolder::State), base / "Local" / "fh1-native" / "logs");
    CHECK_EQ(fh::userFolder(fh::UserFolder::Cache), base / "Local" / "fh1-native" / "cache");
    // A relative value falls back to the known folder rather than the
    // working directory.
    fh::setEnvironmentVariable("LOCALAPPDATA", "relative");
    CHECK(fh::userFolder(fh::UserFolder::Cache).is_absolute());
#else
    const SavedVariable home("HOME");
    const SavedVariable config("XDG_CONFIG_HOME");
    const SavedVariable data("XDG_DATA_HOME");
    const SavedVariable state("XDG_STATE_HOME");
    const SavedVariable cache("XDG_CACHE_HOME");
    for (const char* variable : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"}) {
        fh::unsetEnvironmentVariable(variable);
    }
    fh::setEnvironmentVariable("HOME", fh::pathToUtf8(base));
    CHECK_EQ(fh::userFolder(fh::UserFolder::Config), base / ".config" / "fh1-native");
    CHECK_EQ(fh::userFolder(fh::UserFolder::Data), base / ".local/share" / "fh1-native");
    CHECK_EQ(fh::userFolder(fh::UserFolder::State), base / ".local/state" / "fh1-native");
    CHECK_EQ(fh::userFolder(fh::UserFolder::Cache), base / ".cache" / "fh1-native");
    fh::unsetEnvironmentVariable("HOME");
    CHECK(fh::userFolder(fh::UserFolder::Cache).empty());
#endif
}

TEST(settingsAndLogLiveInTheirUserFolders)
{
    const std::filesystem::path base = scratch("settings") / fh::pathFromUtf8(kUnicodeName);
    const SavedVariable config(fh::userFolderVariable(fh::UserFolder::Config));
    const SavedVariable state(fh::userFolderVariable(fh::UserFolder::State));
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Config), fh::pathToUtf8(base / "config"));
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::State), fh::pathToUtf8(base / "state"));
    CHECK_EQ(fh::Settings::defaultPath(), fh::userFolder(fh::UserFolder::Config) / "settings.ini");
    CHECK_EQ(fh::defaultLogPath(), fh::userFolder(fh::UserFolder::State) / "fh1.log");

    // A game folder named in UTF-8 survives a save and a load.
    const std::string gameDir = fh::pathToUtf8(base / "disc");
    fh::Settings settings = fh::Settings::load();
    settings.set("game", gameDir);
    CHECK(settings.save());
    CHECK(std::filesystem::is_regular_file(fh::Settings::defaultPath()));
    CHECK_EQ(fh::Settings::load().get("game", ""), gameDir);
}

TEST(vfsMatchesGamePathsOnHostPaths)
{
    const std::filesystem::path disc = scratch("vfs") / fh::pathFromUtf8(kUnicodeName);
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
    fh::Vfs vfs;
    vfs.mountDisc(root);
    CHECK(vfs.exists("game:\\media\\tracks\\colorado\\info.xml"));
    CHECK(vfs.exists("media/TRACKS/colorado/INFO.xml"));
    CHECK_EQ(vfs.readText("Game:\\Media\\Tracks\\Colorado\\Info.xml").value_or(""), std::string("<Track/>"));
    const std::optional<std::filesystem::path> host = vfs.hostPath("media/tracks/colorado/info.xml");
    CHECK(host.has_value());
    if (host) {
        CHECK(std::filesystem::equivalent(*host, disc / "media" / "Tracks" / "Colorado" / "Info.XML"));
    }
    const std::vector<std::string> files = vfs.list("media/tracks");
    CHECK_EQ(files.size(), std::size_t{1});
    if (!files.empty()) {
        CHECK_EQ(files.front(), std::string("media/tracks/colorado/info.xml"));
    }

    // The media folder itself works as the root too.
    fh::Vfs media;
    media.mountDisc(disc / "media");
    CHECK(media.exists("media/tracks/colorado/info.xml"));
}

} // namespace

int main(int argc, char** argv)
{
    const int result = fh::test::runAllTests(argc > 1 ? argv[1] : "");
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "fh1-tst-base", ignored);
    return result;
}
