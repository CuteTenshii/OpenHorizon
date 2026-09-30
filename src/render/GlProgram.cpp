#include "GlProgram.h"

#include "base/Host.h"
#include "base/Log.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace openhorizon::render {

namespace {

/// Bumped whenever the entry layout or what the key covers changes, so old
/// entries stop matching.
constexpr std::uint32_t kCacheVersion = 1;
constexpr std::array<char, 8> kMagic{'F', 'H', '1', 'P', 'R', 'O', 'G', '\0'};

/// The count glMaxShaderCompilerThreadsKHR takes for "as many threads as
/// the implementation likes" (0xFFFFFFFF in both extensions' specifications).
constexpr GLuint kAllCompilerThreads = 0xFFFFFFFFU;

/// A 128-bit FNV-1a state, in two halves since standard C++ has no
/// 128-bit integer.
struct Hash128 {
    std::uint64_t high = 0x6c62272e07bb0142ULL;
    std::uint64_t low = 0x62b821756295c58dULL;
};

/// Multiplies by the 128-bit FNV prime, 2^88 + 0x13b: the 0x13b part limb
/// by limb with carries, then the 2^88 part, which only reaches the high
/// half as the low half shifted up by 24.
void multiplyByPrime(Hash128& hash)
{
    constexpr std::uint64_t kSmall = 0x13bULL;
    const std::array<std::uint64_t, 4> limbs{
        hash.low & 0xffffffffULL, hash.low >> 32U, hash.high & 0xffffffffULL, hash.high >> 32U};
    std::array<std::uint64_t, 4> product{};
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < limbs.size(); ++i) {
        const std::uint64_t value = limbs[i] * kSmall + carry;
        product[i] = value & 0xffffffffULL;
        carry = value >> 32U;
    }
    const std::uint64_t shifted = hash.low << 24U;
    hash.low = product[0] | (product[1] << 32U);
    hash.high = (product[2] | (product[3] << 32U)) + shifted;
}

void hashBytes(Hash128& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash.low ^= bytes[i];
        multiplyByPrime(hash);
    }
}

void hashPart(Hash128& hash, std::string_view part)
{
    const auto length = static_cast<std::uint64_t>(part.size());
    std::array<unsigned char, 8> prefix{};
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        prefix[i] = static_cast<unsigned char>(length >> (8U * i));
    }
    hashBytes(hash, prefix.data(), prefix.size());
    hashBytes(hash, part.data(), part.size());
}

std::string glString(GLenum name)
{
    const GLubyte* text = glGetString(name);
    return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
}

bool hasExtension(std::string_view name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const GLubyte* extension = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (extension && name == reinterpret_cast<const char*>(extension)) {
            return true;
        }
    }
    return false;
}

struct CacheState {
    std::string driver;
    std::filesystem::path directory;
    bool parallel = false;
    ProgramCacheStats stats;
};

CacheState makeState()
{
    CacheState state;
    state.driver = std::format("{}\n{}\n{}", glString(GL_VENDOR), glString(GL_RENDERER), glString(GL_VERSION));
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    if (formats > 0) {
        state.directory = programCacheDirectory();
    }
    // Some window systems hand out an address for any name, so the
    // extension string decides rather than the pointer.
    if (glMaxShaderCompilerThreadsKHR && hasExtension("GL_KHR_parallel_shader_compile")) {
        glMaxShaderCompilerThreadsKHR(kAllCompilerThreads);
        state.parallel = true;
    } else if (glMaxShaderCompilerThreadsARB && hasExtension("GL_ARB_parallel_shader_compile")) {
        glMaxShaderCompilerThreadsARB(kAllCompilerThreads);
        state.parallel = true;
    }
    if (!state.directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(state.directory, error);
        if (error) {
            logWarning("shaders: no program cache in {}: {}", pathToUtf8(state.directory), error.message());
            state.directory.clear();
        }
    }
    logInfo("shaders: program cache {}, parallel compile {}",
        state.directory.empty() ? std::string("off") : pathToUtf8(state.directory), state.parallel ? "on" : "off");
    return state;
}

CacheState& cacheState()
{
    static CacheState state = makeState();
    return state;
}

struct Clock {
    explicit Clock(CacheState& cache)
        : m_cache(cache)
    {
    }
    ~Clock() { m_cache.stats.time += std::chrono::steady_clock::now() - m_start; }
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;

private:
    CacheState& m_cache;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

struct CachedBinary {
    GLenum format = 0;
    std::vector<char> data;
};

template <typename T> void writeValue(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T> bool readValue(std::ifstream& in, T& value)
{
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

/// The entry's binary, or nothing when the file is missing, from another
/// version or key, or cut short.
std::optional<CachedBinary> readEntry(const std::filesystem::path& path, std::string_view key)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::array<char, kMagic.size()> magic{};
    std::uint32_t version = 0;
    std::uint32_t keyLength = 0;
    if (!in.read(magic.data(), magic.size()) || magic != kMagic || !readValue(in, version) || version != kCacheVersion
        || !readValue(in, keyLength) || keyLength != key.size()) {
        return std::nullopt;
    }
    std::string storedKey(keyLength, '\0');
    CachedBinary binary;
    std::uint64_t length = 0;
    if (!in.read(storedKey.data(), keyLength) || storedKey != key || !readValue(in, binary.format)
        || !readValue(in, length) || length == 0
        || length > static_cast<std::uint64_t>(std::numeric_limits<GLsizei>::max())) {
        return std::nullopt;
    }
    binary.data.resize(static_cast<std::size_t>(length));
    if (!in.read(binary.data.data(), static_cast<std::streamsize>(length))
        || in.peek() != std::ifstream::traits_type::eof()) {
        return std::nullopt;
    }
    return binary;
}

/// Writes through a temporary file renamed into place, so a crash or a
/// second process never leaves half an entry under the real name.
bool writeEntry(const std::filesystem::path& path, std::string_view key, const CachedBinary& binary)
{
    std::filesystem::path temporary = path;
    temporary += std::format(".{}.tmp", processId());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(kMagic.data(), kMagic.size());
        writeValue(out, kCacheVersion);
        writeValue(out, static_cast<std::uint32_t>(key.size()));
        out.write(key.data(), static_cast<std::streamsize>(key.size()));
        writeValue(out, binary.format);
        writeValue(out, static_cast<std::uint64_t>(binary.data.size()));
        out.write(binary.data.data(), static_cast<std::streamsize>(binary.data.size()));
        if (!out) {
            out.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::string shaderLog(GLuint shader)
{
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    return log;
}

std::string programLog(GLuint program)
{
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    return log;
}

GLuint startCompile(GLenum stage, std::string_view source)
{
    const GLuint shader = glCreateShader(stage);
    const char* text = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    return shader;
}

} // namespace

std::filesystem::path programCacheDirectory()
{
    const std::filesystem::path folder = userFolder(UserFolder::Cache);
    return folder.empty() ? folder : folder / "shaders";
}

std::string programCacheKey(std::string_view driver, std::string_view vertex, std::string_view fragment)
{
    Hash128 hash;
    const std::uint32_t version = kCacheVersion;
    hashBytes(hash, &version, sizeof(version));
    hashPart(hash, driver);
    hashPart(hash, vertex);
    hashPart(hash, fragment);
    return std::format("{:016x}{:016x}", hash.high, hash.low);
}

ProgramCacheStats programCacheStats()
{
    return cacheState().stats;
}

bool parallelShaderCompile()
{
    return cacheState().parallel;
}

std::filesystem::path programCacheFile(std::string_view vertex, std::string_view fragment)
{
    const CacheState& cache = cacheState();
    if (cache.directory.empty()) {
        return {};
    }
    return cache.directory / (programCacheKey(cache.driver, vertex, fragment) + ".bin");
}

ProgramBuild::ProgramBuild(std::string_view name, std::string_view vertex, std::string_view fragment)
    : m_name(name)
{
    CacheState& cache = cacheState();
    const Clock clock(cache);
    m_cacheFile = programCacheFile(vertex, fragment);
    if (!m_cacheFile.empty()) {
        const std::string key = m_cacheFile.stem().string();
        if (const std::optional<CachedBinary> binary = readEntry(m_cacheFile, key)) {
            m_program = glCreateProgram();
            glProgramBinary(m_program, binary->format, binary->data.data(), static_cast<GLsizei>(binary->data.size()));
            GLint linked = 0;
            glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
            if (linked) {
                m_fromCache = true;
                ++cache.stats.fromCache;
                return;
            }
            glDeleteProgram(m_program);
            m_program = 0;
            ++cache.stats.rejected;
            logDebug("shaders: {}: the driver refused its cached binary", m_name);
        } else if (std::filesystem::exists(m_cacheFile)) {
            ++cache.stats.rejected;
            logDebug("shaders: {}: unreadable cache entry", m_name);
        }
    }
    m_vertex = startCompile(GL_VERTEX_SHADER, vertex);
    m_fragment = startCompile(GL_FRAGMENT_SHADER, fragment);
    m_program = glCreateProgram();
    glAttachShader(m_program, m_vertex);
    glAttachShader(m_program, m_fragment);
    if (!m_cacheFile.empty()) {
        glProgramParameteri(m_program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    }
    // Linking a program whose stages failed only fails the link; the
    // stages' own logs are read in finish().
    glLinkProgram(m_program);
}

ProgramBuild::~ProgramBuild()
{
    release();
}

ProgramBuild::ProgramBuild(ProgramBuild&& other) noexcept
    : m_name(std::move(other.m_name))
    , m_cacheFile(std::move(other.m_cacheFile))
    , m_program(std::exchange(other.m_program, 0))
    , m_vertex(std::exchange(other.m_vertex, 0))
    , m_fragment(std::exchange(other.m_fragment, 0))
    , m_fromCache(other.m_fromCache)
{
}

ProgramBuild& ProgramBuild::operator=(ProgramBuild&& other) noexcept
{
    if (this != &other) {
        release();
        m_name = std::move(other.m_name);
        m_cacheFile = std::move(other.m_cacheFile);
        m_program = std::exchange(other.m_program, 0);
        m_vertex = std::exchange(other.m_vertex, 0);
        m_fragment = std::exchange(other.m_fragment, 0);
        m_fromCache = other.m_fromCache;
    }
    return *this;
}

void ProgramBuild::release()
{
    if (m_vertex) {
        glDeleteShader(m_vertex);
        m_vertex = 0;
    }
    if (m_fragment) {
        glDeleteShader(m_fragment);
        m_fragment = 0;
    }
    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
}

bool ProgramBuild::ready() const
{
    if (m_fromCache || !m_program || !cacheState().parallel) {
        return true;
    }
    GLint complete = 0;
    glGetProgramiv(m_program, GL_COMPLETION_STATUS_KHR, &complete);
    return complete != 0;
}

GLuint ProgramBuild::finish()
{
    if (!m_program) {
        throw std::runtime_error(m_name + " program: already handed over");
    }
    if (m_fromCache) {
        return std::exchange(m_program, 0);
    }
    CacheState& cache = cacheState();
    const Clock clock(cache);
    for (const auto& [shader, stage] : {std::pair(m_vertex, "vertex"), std::pair(m_fragment, "fragment")}) {
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            const std::string log = shaderLog(shader);
            release();
            throw std::runtime_error(std::format("{} {} shader: {}", m_name, stage, log));
        }
    }
    GLint linked = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        const std::string log = programLog(m_program);
        release();
        throw std::runtime_error(std::format("{} program: {}", m_name, log));
    }
    glDetachShader(m_program, m_vertex);
    glDetachShader(m_program, m_fragment);
    glDeleteShader(m_vertex);
    glDeleteShader(m_fragment);
    m_vertex = 0;
    m_fragment = 0;
    ++cache.stats.compiled;
    if (!m_cacheFile.empty()) {
        GLint length = 0;
        glGetProgramiv(m_program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length > 0) {
            CachedBinary binary;
            binary.data.resize(static_cast<std::size_t>(length));
            GLsizei written = 0;
            glGetProgramBinary(m_program, length, &written, &binary.format, binary.data.data());
            binary.data.resize(static_cast<std::size_t>(std::max(written, 0)));
            if (!binary.data.empty() && writeEntry(m_cacheFile, m_cacheFile.stem().string(), binary)) {
                ++cache.stats.stored;
            }
        }
    }
    return std::exchange(m_program, 0);
}

GLuint linkProgram(std::string_view name, std::string_view vertex, std::string_view fragment)
{
    return ProgramBuild(name, vertex, fragment).finish();
}

} // namespace openhorizon::render
