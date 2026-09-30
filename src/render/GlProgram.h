#pragma once

#include "Gl.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace openhorizon::render {

/// Linked GLSL programs, kept between runs as the driver's own binaries
/// (glGetProgramBinary) so a second start skips compiling.
///
/// The cache lives in `shaders` in the Cache user folder:
/// `~/.cache/fh1-native/shaders` on Linux (under `$XDG_CACHE_HOME` when that
/// is an absolute path), `%LOCALAPPDATA%\fh1-native\cache\shaders` on
/// Windows (see openhorizon::UserFolder). An entry is keyed by the two sources and
/// the driver's GL_VENDOR, GL_RENDERER and GL_VERSION, so a driver update or
/// a changed shader never loads a stale binary. When the driver refuses a
/// binary anyway (glProgramBinary leaves the program unlinked, which the
/// specification allows at any time), the program is compiled from source
/// and the entry rewritten.
///
/// Where the driver has GL_KHR_parallel_shader_compile or
/// GL_ARB_parallel_shader_compile, compiles run on its own threads: a
/// ProgramBuild starts the work and returns, ready() polls it without
/// blocking, and finish() waits only for what is still running. Several
/// builds started together therefore compile at once.
///
/// Everything here runs on the thread that owns the GL context; the cache
/// reads the driver's strings and the environment on first use.

/// The cache folder; empty when the environment names no user cache folder.
std::filesystem::path programCacheDirectory();

/// The cache key of a program: 32 hex digits of a 128-bit FNV-1a hash over
/// the driver description and both sources, each prefixed with its length
/// so no two different inputs run together into the same bytes.
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
    /// Time the GL thread spent building programs, cache reads and writes
    /// included; with parallel compiles, not the driver's own threads.
    std::chrono::nanoseconds time{0};
};

ProgramCacheStats programCacheStats();

/// Whether compiles run on the driver's threads (see above).
bool parallelShaderCompile();

/// The file that holds the binary for these sources with the current
/// driver; empty when the cache is off (no cache folder, or a driver with
/// no binary formats).
std::filesystem::path programCacheFile(std::string_view vertex, std::string_view fragment);

/// One program being built: restored from the cache at once, or compiled,
/// in the background where the driver can.
class ProgramBuild {
public:
    /// `name` prefixes error messages.
    ProgramBuild(std::string_view name, std::string_view vertex, std::string_view fragment);
    ~ProgramBuild();
    ProgramBuild(ProgramBuild&& other) noexcept;
    ProgramBuild& operator=(ProgramBuild&& other) noexcept;
    ProgramBuild(const ProgramBuild&) = delete;
    ProgramBuild& operator=(const ProgramBuild&) = delete;

    /// True when finish() would not wait: the program came from the cache,
    /// the driver's threads are done with it, or the driver compiles on
    /// this thread anyway.
    bool ready() const;
    /// Waits for the program if needed and hands it over. Throws
    /// std::runtime_error with the driver's log, prefixed by the name, when
    /// a stage fails to compile or the program to link.
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
