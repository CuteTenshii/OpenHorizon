#pragma once

#include "Gl.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace openhorizon::render {

/// Linked GLSL programs, cached between runs as driver binaries in the
/// Cache user folder's `shaders`, keyed by both sources and GL_VENDOR,
/// GL_RENDERER and GL_VERSION. The driver may still refuse a binary at any
/// time; the program is then compiled and the entry rewritten.
///
/// With GL_KHR/ARB_parallel_shader_compile, compiles run on the driver's
/// threads. Everything here runs on the GL context's thread.

/// The cache folder; empty when the environment names no user cache folder.
std::filesystem::path programCacheDirectory();

/// 128-bit FNV-1a as 32 hex digits; each input is length-prefixed so
/// different inputs never run together into the same bytes.
std::string programCacheKey(std::string_view driver, std::string_view vertex, std::string_view fragment);

/// What the cache has done since the process started.
struct ProgramCacheStats {
    /// Programs restored from a cached binary.
    std::size_t fromCache = 0;
    /// Programs compiled and linked from source.
    std::size_t compiled = 0;
    /// Cached binaries that were unreadable or that the driver refused.
    std::size_t rejected = 0;
    /// Binaries written to the cache.
    std::size_t stored = 0;
    /// GL thread time only, cache reads and writes included.
    std::chrono::nanoseconds time{0};
};

ProgramCacheStats programCacheStats();

/// Whether compiles run on the driver's threads (see above).
bool parallelShaderCompile();

/// Empty when the cache is off (no cache folder, or no binary formats).
std::filesystem::path programCacheFile(std::string_view vertex, std::string_view fragment);

/// One program being built, in the background where the driver can.
class ProgramBuild {
public:
    /// `name` prefixes error messages.
    ProgramBuild(std::string_view name, std::string_view vertex, std::string_view fragment);
    ~ProgramBuild();
    ProgramBuild(ProgramBuild&& other) noexcept;
    ProgramBuild& operator=(ProgramBuild&& other) noexcept;
    ProgramBuild(const ProgramBuild&) = delete;
    ProgramBuild& operator=(const ProgramBuild&) = delete;

    /// True when finish() would not wait.
    bool ready() const;
    /// Throws std::runtime_error with the driver's log on a compile or link
    /// failure.
    GLuint finish();
    bool fromCache() const { return m_fromCache; }

private:
    void release();

    std::string m_name;
    std::filesystem::path m_cacheFile;
    GLuint m_program = 0;
    GLuint m_vertex = 0;
    GLuint m_fragment = 0;
    bool m_fromCache = false;
};

/// Builds a program and waits for it: ProgramBuild(...).finish().
GLuint linkProgram(std::string_view name, std::string_view vertex, std::string_view fragment);

} // namespace openhorizon::render
