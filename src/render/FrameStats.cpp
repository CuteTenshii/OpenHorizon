#include "FrameStats.h"

#include "base/Log.h"

#include <algorithm>
#include <format>
#include <string>

namespace openhorizon::render {

namespace {

constexpr std::array kPartNames{"simulation", "physics", "streaming", "shadows", "environment cube", "world",
    "animated objects", "crowd", "cars", "sky", "glass", "driving line", "particles", "tonemap", "HUD", "menus",
    "present"};
static_assert(kPartNames.size() == FrameStats::kParts);

bool drawing(std::size_t part)
{
    return part >= static_cast<std::size_t>(FrameStats::Part::DrawShadows);
}

} // namespace

const char* FrameStats::partName(Part part)
{
    return kPartNames[static_cast<std::size_t>(part)];
}

FrameStats::FrameStats()
    : m_intervalStart(Clock::now())
    , m_frameStart(m_intervalStart)
    , m_last(m_intervalStart)
{
    for (GpuFrame& frame : m_gpu) {
        glGenQueries(static_cast<GLsizei>(frame.queries.size()), frame.queries.data());
    }
}

FrameStats::~FrameStats()
{
    for (GpuFrame& frame : m_gpu) {
        glDeleteQueries(static_cast<GLsizei>(frame.queries.size()), frame.queries.data());
    }
}

void FrameStats::beginFrame()
{
    m_frameStart = Clock::now();
    m_last = m_frameStart;
    // Loading happens between construction and the first frame.
    if (m_frames == 0 && m_seconds == PartTimes{}) {
        m_intervalStart = m_frameStart;
    }
    m_frame = {};
    m_wait = {};
    collectGpu();
    // A frame whose slot still has queries in flight after a full ring is
    // not timed on the GPU rather than waited for; the average only loses
    // a sample.
    GpuFrame& gpu = m_gpu[m_gpuSlot];
    if (!gpu.pending) {
        gpu.marks = 0;
        glQueryCounter(gpu.queries[0], GL_TIMESTAMP);
    }
}

void FrameStats::mark(Part part)
{
    if (m_serialised) {
        const Clock::time_point submitted = Clock::now();
        glFinish();
        m_wait[static_cast<std::size_t>(part)] += std::chrono::duration<double>(Clock::now() - submitted).count();
    }
    const Clock::time_point now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - m_last).count();
    m_seconds[static_cast<std::size_t>(part)] += seconds;
    m_frame[static_cast<std::size_t>(part)] += seconds;
    m_last = now;
    timestamp(part);
}

void FrameStats::timestamp(Part part)
{
    GpuFrame& gpu = m_gpu[m_gpuSlot];
    if (gpu.pending || gpu.marks == kMarks) {
        return;
    }
    gpu.parts[gpu.marks] = part;
    ++gpu.marks;
    glQueryCounter(gpu.queries[gpu.marks], GL_TIMESTAMP);
}

void FrameStats::endGpuFrame()
{
    // What follows the last mark (fitting the picture into the window) is
    // the present's GPU work.
    timestamp(Part::Present);
    GpuFrame& gpu = m_gpu[m_gpuSlot];
    if (!gpu.pending) {
        gpu.pending = true;
        m_gpuSlot = (m_gpuSlot + 1) % m_gpu.size();
    }
}

void FrameStats::collectGpu()
{
    // Oldest first, so the last frame read is the newest.
    for (std::size_t k = 0; k < m_gpu.size(); ++k) {
        GpuFrame& gpu = m_gpu[(m_gpuSlot + k) % m_gpu.size()];
        if (!gpu.pending) {
            continue;
        }
        // Timestamps complete in order, so the last one being available
        // means every one is, and none of the reads below waits.
        GLint available = 0;
        glGetQueryObjectiv(gpu.queries[gpu.marks], GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) {
            continue;
        }
        std::array<GLuint64, kMarks + 1> stamps{};
        for (std::size_t i = 0; i <= gpu.marks; ++i) {
            glGetQueryObjectui64v(gpu.queries[i], GL_QUERY_RESULT, &stamps[i]);
        }
        m_gpuFrame = {};
        for (std::size_t i = 0; i < gpu.marks; ++i) {
            const double seconds = static_cast<double>(stamps[i + 1] - stamps[i]) * 1e-9;
            m_gpuFrame[static_cast<std::size_t>(gpu.parts[i])] += seconds;
        }
        for (std::size_t part = 0; part < kParts; ++part) {
            m_gpuSeconds[part] += m_gpuFrame[part];
        }
        ++m_gpuFrames;
        gpu.pending = false;
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
    const auto perFrame = [](double seconds, std::size_t frames) {
        return frames ? seconds * 1000.0 / static_cast<double>(frames) : 0.0;
    };
    std::string cpu;
    std::string gpu;
    double gpuTotal = 0.0;
    for (std::size_t i = 0; i < kParts; ++i) {
        cpu += std::format("{}{} {:.1f}", cpu.empty() ? "" : ", ", kPartNames[i], perFrame(m_seconds[i], m_frames));
        if (drawing(i)) {
            const double ms = perFrame(m_gpuSeconds[i], m_gpuFrames);
            gpuTotal += ms;
            gpu += std::format("{}{} {:.1f}", gpu.empty() ? "" : ", ", kPartNames[i], ms);
        }
    }
    logInfo("perf: {:.0f} fps, worst frame {:.0f} ms; CPU ms per frame: {}; GPU ms per frame {:.1f} "
            "(including waits for the CPU's commands): {}; {:.1f} physics steps per frame for {} cars",
        static_cast<double>(m_frames) / elapsed, m_worstFrame * 1000.0, cpu, gpuTotal, gpu,
        static_cast<double>(m_physicsSteps) / static_cast<double>(m_frames), m_vehicles);
    m_intervalStart = now;
    m_seconds = {};
    m_worstFrame = 0.0;
    m_gpuSeconds = {};
    m_gpuFrames = 0;
    m_frames = 0;
    m_physicsSteps = 0;
    m_vehicles = 0;
}

} // namespace openhorizon::render
