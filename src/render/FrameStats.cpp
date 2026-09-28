#include "FrameStats.h"

#include "base/Log.h"

#include <algorithm>
#include <format>
#include <string>

namespace fh::render {

namespace {

constexpr std::array kPartNames{"simulation", "physics", "streaming", "world", "cars", "other drawing", "present"};
static_assert(kPartNames.size() == static_cast<std::size_t>(FrameStats::Part::Count));

} // namespace

FrameStats::FrameStats()
    : m_intervalStart(Clock::now())
    , m_frameStart(m_intervalStart)
    , m_last(m_intervalStart)
{
    glGenQueries(static_cast<GLsizei>(m_queries.size()), m_queries.data());
}

FrameStats::~FrameStats()
{
    glDeleteQueries(static_cast<GLsizei>(m_queries.size()), m_queries.data());
}

void FrameStats::beginFrame()
{
    m_frameStart = Clock::now();
    m_last = m_frameStart;
    // Loading happens between construction and the first frame.
    if (m_frames == 0 && m_seconds == decltype(m_seconds){}) {
        m_intervalStart = m_frameStart;
    }
    collectGpu();
    // A query still in flight after a full ring is dropped rather than
    // waited for; the average only loses a sample.
    if (!m_pending[m_query]) {
        glBeginQuery(GL_TIME_ELAPSED, m_queries[m_query]);
    }
}

void FrameStats::mark(Part part)
{
    const Clock::time_point now = Clock::now();
    m_seconds[static_cast<std::size_t>(part)] += std::chrono::duration<double>(now - m_last).count();
    m_last = now;
}

void FrameStats::endGpuFrame()
{
    if (!m_pending[m_query]) {
        glEndQuery(GL_TIME_ELAPSED);
        m_pending[m_query] = true;
    }
    m_query = (m_query + 1) % m_queries.size();
}

void FrameStats::collectGpu()
{
    for (std::size_t i = 0; i < m_queries.size(); ++i) {
        if (!m_pending[i]) {
            continue;
        }
        GLint available = 0;
        glGetQueryObjectiv(m_queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) {
            continue;
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(m_queries[i], GL_QUERY_RESULT, &nanoseconds);
        m_gpuSeconds += static_cast<double>(nanoseconds) * 1e-9;
        ++m_gpuFrames;
        m_pending[i] = false;
    }
}

void FrameStats::endFrame(int physicsSteps, std::size_t vehicles)
{
    const Clock::time_point now = Clock::now();
    m_worstFrame = std::max(m_worstFrame, std::chrono::duration<double>(now - m_frameStart).count());
    ++m_frames;
    m_physicsSteps += physicsSteps;
    m_vehicles = std::max(m_vehicles, vehicles);
    const double elapsed = std::chrono::duration<double>(now - m_intervalStart).count();
    if (elapsed < kInterval) {
        return;
    }
    const auto perFrame = [&](double seconds) { return seconds * 1000.0 / static_cast<double>(m_frames); };
    std::string parts;
    for (std::size_t i = 0; i < m_seconds.size(); ++i) {
        parts += std::format("{}{} {:.1f}", parts.empty() ? "" : ", ", kPartNames[i], perFrame(m_seconds[i]));
    }
    const double gpu = m_gpuFrames ? m_gpuSeconds * 1000.0 / static_cast<double>(m_gpuFrames) : 0.0;
    logInfo("perf: {:.0f} fps, worst frame {:.0f} ms, GPU {:.1f} ms; CPU ms per frame: {}; "
            "{:.1f} physics steps per frame for {} cars",
        static_cast<double>(m_frames) / elapsed, m_worstFrame * 1000.0, gpu, parts,
        static_cast<double>(m_physicsSteps) / static_cast<double>(m_frames), m_vehicles);
    m_intervalStart = now;
    m_seconds = {};
    m_worstFrame = 0.0;
    m_gpuSeconds = 0.0;
    m_gpuFrames = 0;
    m_frames = 0;
    m_physicsSteps = 0;
    m_vehicles = 0;
}

} // namespace fh::render
