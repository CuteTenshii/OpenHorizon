// The GL program cache: its key and folder without a GL context, and with
// an offscreen context the round trip through the cache and the fallback to
// compiling when a cached binary is refused.

#include "TestHarness.h"

#include "platform/Window.h"
#include "render/GlProgram.h"

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

/// The folder the GL tests point XDG_CACHE_HOME at, emptied by main().
std::filesystem::path& cacheHome()
{
    static std::filesystem::path path;
    return path;
}

/// One offscreen context for the GL tests, or null when this machine has
/// none (the tests then skip).
fh::Window* window()
{
    static std::unique_ptr<fh::Window> instance = [] {
        fh::WindowOptions options;
        options.width = 64;
        options.height = 64;
        options.hidden = true;
        options.vsync = false;
        try {
            return std::make_unique<fh::Window>(options);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "skipped: no OpenGL context (%s)\n", e.what());
            return std::unique_ptr<fh::Window>();
        }
    }();
    return instance.get();
}

TEST(cacheKeyIsStableAndCoversEveryInput)
{
    const std::string key = fh::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment");
    CHECK_EQ(key.size(), std::size_t{32});
    CHECK(key.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK_EQ(fh::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment"), key);
    // A driver update, or either source changing, gives another key.
    CHECK(fh::render::programCacheKey("vendor\nrenderer\n4.6", "vertex", "fragment") != key);
    CHECK(fh::render::programCacheKey("vendor\nrenderer\n4.5", "vertex ", "fragment") != key);
    CHECK(fh::render::programCacheKey("vendor\nrenderer\n4.5", "vertex", "fragment ") != key);
    // Text moved from one source to the other is not the same program.
    CHECK(fh::render::programCacheKey("d", "ab", "c") != fh::render::programCacheKey("d", "a", "bc"));
    CHECK(fh::render::programCacheKey("d", "vertex", "fragment")
        != fh::render::programCacheKey("d", "fragment", "vertex"));
}

TEST(cacheFolderFollowsTheXdgRules)
{
    using fh::render::programCacheDirectory;
    CHECK_EQ(programCacheDirectory("/x/cache", "/home/u"), std::filesystem::path("/x/cache/fh1-native/shaders"));
    CHECK_EQ(programCacheDirectory(nullptr, "/home/u"), std::filesystem::path("/home/u/.cache/fh1-native/shaders"));
    CHECK_EQ(programCacheDirectory("", "/home/u"), std::filesystem::path("/home/u/.cache/fh1-native/shaders"));
    // Relative values are to be ignored.
    CHECK_EQ(programCacheDirectory("relative", "/home/u"), std::filesystem::path("/home/u/.cache/fh1-native/shaders"));
    CHECK(programCacheDirectory(nullptr, nullptr).empty());
    CHECK(programCacheDirectory("relative", "also/relative").empty());
}

TEST(programsRoundTripThroughTheCache)
{
    if (!window()) {
        return;
    }
    const std::filesystem::path file = fh::render::programCacheFile(kVertex, kFragment);
    if (file.empty()) {
        std::fprintf(stderr, "skipped: the driver has no program binary formats\n");
        return;
    }
    CHECK(file.string().starts_with(cacheHome().string()));
    const fh::render::ProgramCacheStats before = fh::render::programCacheStats();

    const GLuint cold = fh::render::linkProgram("cold", kVertex, kFragment);
    CHECK(cold != 0);
    glDeleteProgram(cold);
    const fh::render::ProgramCacheStats afterCold = fh::render::programCacheStats();
    CHECK_EQ(afterCold.compiled, before.compiled + 1);
    CHECK_EQ(afterCold.stored, before.stored + 1);
    CHECK(std::filesystem::exists(file));

    fh::render::ProgramBuild warm("warm", kVertex, kFragment);
    CHECK(warm.fromCache());
    CHECK(warm.ready());
    const GLuint restored = warm.finish();
    CHECK(restored != 0);
    GLint linked = 0;
    glGetProgramiv(restored, GL_LINK_STATUS, &linked);
    CHECK(linked != 0);
    glDeleteProgram(restored);
    const fh::render::ProgramCacheStats afterWarm = fh::render::programCacheStats();
    CHECK_EQ(afterWarm.fromCache, afterCold.fromCache + 1);
    CHECK_EQ(afterWarm.compiled, afterCold.compiled);
}

TEST(refusedBinariesFallBackToCompiling)
{
    if (!window()) {
        return;
    }
    // Sources of their own, so this test's entry is not the other test's.
    const std::string vertex = std::string(kVertex) + "// refused\n";
    const std::filesystem::path file = fh::render::programCacheFile(vertex, kFragment);
    if (file.empty()) {
        std::fprintf(stderr, "skipped: the driver has no program binary formats\n");
        return;
    }
    glDeleteProgram(fh::render::linkProgram("first", vertex, kFragment));
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
    const fh::render::ProgramCacheStats before = fh::render::programCacheStats();
    fh::render::ProgramBuild spoiled("spoiled", vertex, kFragment);
    CHECK(!spoiled.fromCache());
    const GLuint program = spoiled.finish();
    CHECK(program != 0);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked != 0);
    glDeleteProgram(program);
    const fh::render::ProgramCacheStats after = fh::render::programCacheStats();
    CHECK_EQ(after.rejected, before.rejected + 1);
    CHECK_EQ(after.compiled, before.compiled + 1);
    CHECK_EQ(after.stored, before.stored + 1);

    // The rewritten entry loads again.
    fh::render::ProgramBuild again("again", vertex, kFragment);
    CHECK(again.fromCache());
    glDeleteProgram(again.finish());

    // A cut-short entry is not handed to the driver at all.
    std::filesystem::resize_file(file, 20);
    fh::render::ProgramBuild truncated("truncated", vertex, kFragment);
    CHECK(!truncated.fromCache());
    glDeleteProgram(truncated.finish());
    CHECK_EQ(fh::render::programCacheStats().rejected, after.rejected + 1);
}

TEST(brokenSourcesThrowWithTheirName)
{
    if (!window()) {
        return;
    }
    const std::string broken = "#version 450 core\nvoid main() { this is not glsl }\n";
    bool threw = false;
    try {
        glDeleteProgram(fh::render::linkProgram("broken one", kVertex, broken));
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).starts_with("broken one fragment shader: "));
    }
    CHECK(threw);
    const std::filesystem::path file = fh::render::programCacheFile(kVertex, broken);
    CHECK(file.empty() || !std::filesystem::exists(file));
}

} // namespace

int main(int argc, char** argv)
{
    // Never open a window on the desktop, and never touch the real cache.
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    setenv("SDL_AUDIO_DRIVER", "dummy", 0);
    cacheHome() = std::filesystem::temp_directory_path() / "fh1-tst-render-cache";
    std::filesystem::remove_all(cacheHome());
    setenv("XDG_CACHE_HOME", cacheHome().c_str(), 1);
    const int result = fh::test::runAllTests(argc > 1 ? argv[1] : "");
    std::filesystem::remove_all(cacheHome());
    return result;
}
