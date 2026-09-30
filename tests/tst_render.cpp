// The GL program cache: its key and folder without a GL context, and with
// an offscreen context the round trip through the cache and the fallback to
// compiling when a cached binary is refused.

#include "support/GlContext.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "base/Host.h"
#include "platform/Window.h"
#include "render/GlProgram.h"
#include "render/ScatterPass.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* kVertex = R"(#version 450 core
void main()
{
    gl_Position = vec4(float(gl_VertexID & 1), float(gl_VertexID >> 1), 0.0, 1.0);
}
)";

constexpr const char* kFragment = R"(#version 450 core
layout(location = 0) out vec4 colour;
void main()
{
    colour = vec4(0.25, 0.5, 0.75, 1.0);
}
)";

/// The folder the tests point the user cache folder's variable at,
/// emptied by CacheFolder.
std::filesystem::path& cacheHome()
{
    static std::filesystem::path path;
    return path;
}

TEST_CASE("cacheKeyIsStableAndCoversEveryInput", "[render]")
{
    const std::string key = openhorizon::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment");
    CHECK(key.size() == std::size_t{32});
    CHECK(key.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(openhorizon::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment") == key);
    // A driver update, or either source changing, gives another key.
    CHECK(openhorizon::render::programCacheKey("vendor\nrenderer\n4.6", "vertex", "fragment") != key);
    CHECK(openhorizon::render::programCacheKey("vendor\nrenderer\n4.5", "vertex ", "fragment") != key);
    CHECK(openhorizon::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment ") != key);
    // Text moved from one source to the other is not the same program.
    CHECK(openhorizon::render::programCacheKey("d", "ab", "c") != openhorizon::render::programCacheKey("d", "a", "bc"));
    CHECK(openhorizon::render::programCacheKey("d", "vertex", "fragment")
        != openhorizon::render::programCacheKey("d", "fragment", "vertex"));
}

TEST_CASE("cacheFolderIsInTheUserCacheFolder", "[render]")
{
    // CacheFolder points the cache's variable at cacheHome(); the folder rules
    // themselves are tst_base's.
    CHECK(openhorizon::render::programCacheDirectory().native().starts_with(cacheHome().native()));
    CHECK(openhorizon::render::programCacheDirectory().filename() == std::filesystem::path("shaders"));
}

TEST_CASE("programsRoundTripThroughTheCache", "[render]")
{
    openhorizon::test::requireGlContext();
    const std::filesystem::path file = openhorizon::render::programCacheFile(kVertex, kFragment);
    if (file.empty()) {
        SKIP("the driver has no program binary formats");
    }
    CHECK(file.native().starts_with(cacheHome().native()));
    const openhorizon::render::ProgramCacheStats before = openhorizon::render::programCacheStats();

    const GLuint cold = openhorizon::render::linkProgram("cold", kVertex, kFragment);
    CHECK(cold != 0);
    glDeleteProgram(cold);
    const openhorizon::render::ProgramCacheStats afterCold = openhorizon::render::programCacheStats();
    CHECK(afterCold.compiled == before.compiled + 1);
    CHECK(afterCold.stored == before.stored + 1);
    CHECK(std::filesystem::exists(file));

    openhorizon::render::ProgramBuild warm("warm", kVertex, kFragment);
    CHECK(warm.fromCache());
    CHECK(warm.ready());
    const GLuint restored = warm.finish();
    CHECK(restored != 0);
    GLint linked = 0;
    glGetProgramiv(restored, GL_LINK_STATUS, &linked);
    CHECK(linked != 0);
    glDeleteProgram(restored);
    const openhorizon::render::ProgramCacheStats afterWarm = openhorizon::render::programCacheStats();
    CHECK(afterWarm.fromCache == afterCold.fromCache + 1);
    CHECK(afterWarm.compiled == afterCold.compiled);
}

TEST_CASE("refusedBinariesFallBackToCompiling", "[render]")
{
    openhorizon::test::requireGlContext();
    // Sources of their own, so this test's entry is not the other test's.
    const std::string vertex = std::string(kVertex) + "// refused\n";
    const std::filesystem::path file = openhorizon::render::programCacheFile(vertex, kFragment);
    if (file.empty()) {
        SKIP("the driver has no program binary formats");
    }
    glDeleteProgram(openhorizon::render::linkProgram("first", vertex, kFragment));
    CHECK(std::filesystem::exists(file));

    // Keep the header, so the entry is read and handed to the driver, and
    // spoil the binary after it.
    const auto size = std::filesystem::file_size(file);
    {
        std::fstream entry(file, std::ios::binary | std::ios::in | std::ios::out);
        std::vector<char> junk(64, '\x5a');
        entry.seekp(static_cast<std::streamoff>(size) - static_cast<std::streamoff>(junk.size()));
        entry.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        std::vector<char> header(80, '\x5a');
        // Past magic (8), version (4), key length (4), key (32), format (4)
        // and length (8): the start of the binary itself.
        entry.seekp(60);
        entry.write(header.data(), static_cast<std::streamsize>(header.size()));
    }
    const openhorizon::render::ProgramCacheStats before = openhorizon::render::programCacheStats();
    openhorizon::render::ProgramBuild spoiled("spoiled", vertex, kFragment);
    CHECK(!spoiled.fromCache());
    const GLuint program = spoiled.finish();
    CHECK(program != 0);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked != 0);
    glDeleteProgram(program);
    const openhorizon::render::ProgramCacheStats after = openhorizon::render::programCacheStats();
    CHECK(after.rejected == before.rejected + 1);
    CHECK(after.compiled == before.compiled + 1);
    CHECK(after.stored == before.stored + 1);

    // The rewritten entry loads again.
    openhorizon::render::ProgramBuild again("again", vertex, kFragment);
    CHECK(again.fromCache());
    glDeleteProgram(again.finish());

    // A cut-short entry is not handed to the driver at all.
    std::filesystem::resize_file(file, 20);
    openhorizon::render::ProgramBuild truncated("truncated", vertex, kFragment);
    CHECK(!truncated.fromCache());
    glDeleteProgram(truncated.finish());
    CHECK(openhorizon::render::programCacheStats().rejected == after.rejected + 1);
}

TEST_CASE("brokenSourcesThrowWithTheirName", "[render]")
{
    openhorizon::test::requireGlContext();
    const std::string broken = "#version 450 core\nvoid main() { this is not glsl }\n";
    bool threw = false;
    try {
        glDeleteProgram(openhorizon::render::linkProgram("broken one", kVertex, broken));
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).starts_with("broken one fragment shader: "));
    }
    CHECK(threw);
    const std::filesystem::path file = openhorizon::render::programCacheFile(kVertex, broken);
    CHECK((file.empty() || !std::filesystem::exists(file)));
}

} // namespace

// The world draws a chunk's triangles only while the camera is within its
// band: the batches' runs of chunks become index ranges, adjacent ones
// merged, and a batch none of whose chunks is drawn gets no range.
TEST_CASE("selectedChunksBecomeMergedIndexRanges", "[render]")
{
    struct Batch {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        std::vector<openhorizon::world::ChunkRun> runs;
    };
    const std::vector<Batch> batches{
        {0, 30, {{4, 0, 6}, {5, 6, 12}, {6, 18, 12}}},
        {30, 9, {{7, 30, 9}}},
        {39, 12, {{4, 39, 6}, {6, 45, 6}}},
    };
    std::vector<std::uint8_t> drawn(8, 0);
    drawn[5] = 1;
    drawn[6] = 1;
    drawn[4] = 1;
    std::vector<openhorizon::render::IndexRange> ranges;
    std::vector<std::uint32_t> start;
    openhorizon::render::selectRanges(batches, drawn, ranges, start);
    CHECK(start == (std::vector<std::uint32_t>{0, 1, 1, 2}));
    CHECK(ranges.size() == std::size_t{2});
    CHECK(ranges[0].first == 0U);
    CHECK(ranges[0].count == 30U);
    CHECK(ranges[1].first == 39U);
    CHECK(ranges[1].count == 12U);

    drawn[5] = 0;
    openhorizon::render::selectRanges(batches, drawn, ranges, start);
    CHECK(start == (std::vector<std::uint32_t>{0, 2, 2, 3}));
    CHECK(ranges[0].count == 6U);
    CHECK(ranges[1].first == 18U);

    openhorizon::render::wholeRanges(batches, ranges, start);
    CHECK(start == (std::vector<std::uint32_t>{0, 1, 2, 3}));
    CHECK(ranges[1].first == 30U);
    CHECK(ranges[1].count == 9U);

    std::vector<openhorizon::world::ChunkRun> runs;
    openhorizon::world::appendChunkRun(runs, 3, 0, 6);
    openhorizon::world::appendChunkRun(runs, 3, 6, 3);
    openhorizon::world::appendChunkRun(runs, 4, 9, 0);
    openhorizon::world::appendChunkRun(runs, 4, 9, 3);
    CHECK(runs.size() == std::size_t{2});
    CHECK(runs[0].indexCount == 9U);
    CHECK(runs[1].chunk == 4U);
}

namespace {

/// Points the user cache folder at an empty cacheHome() before the cases
/// run, never the real cache, and empties it after them.
class CacheFolder : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override
    {
        cacheHome() = std::filesystem::temp_directory_path() / "fh1-tst-render-cache";
        std::filesystem::remove_all(cacheHome());
        openhorizon::setEnvironmentVariable(
            openhorizon::userFolderVariable(openhorizon::UserFolder::Cache), openhorizon::pathToUtf8(cacheHome()));
    }

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override { std::filesystem::remove_all(cacheHome()); }
};

} // namespace

CATCH_REGISTER_LISTENER(CacheFolder)
