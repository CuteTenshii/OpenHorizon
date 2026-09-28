#pragma once

#include "render/Gl.h"

#include <array>
#include <chrono>
#include <cstddef>

namespace fh::render {

/// Where frame time goes, logged every few seconds: CPU time per part of the
/// frame, and GPU time from timer queries. A player's log then shows whether
/// a slow frame rate comes from the GPU or from which part of the CPU work.
class FrameStats {
public:
    enum class Part : std::size_t {
        Simulation,
        Physics,
        Streaming,
        DrawWorld,
        DrawCars,
        DrawOther,
        /// Swapping buffers: waiting for vsync, or for the GPU to catch up.
        Present,
        Count,
    };

    /// Needs a current GL context.
    FrameStats();
    ~FrameStats();
    FrameStats(const FrameStats&) = delete;
    FrameStats& operator=(const FrameStats&) = delete;

    void beginFrame();
    /// Charges the time since the last mark (or the frame's start) to `part`.
    void mark(Part part);
    /// Ends the GPU timing of this frame; call right before presenting.
    void endGpuFrame();
    /// Closes the frame; logs a summary once `kInterval` seconds have passed.
    void endFrame(int physicsSteps, std::size_t vehicles);

private:
    using Clock = std::chrono::steady_clock;
    static constexpr double kInterval = 5.0;
    // Enough queries in flight that reading a result never waits on the GPU.
    static constexpr std::size_t kQueries = 4;

    void collectGpu();

    std::array<GLuint, kQueries> m_queries{};
    std::array<bool, kQueries> m_pending{};
    std::size_t m_query = 0;
    Clock::time_point m_intervalStart;
    Clock::time_point m_frameStart;
    Clock::time_point m_last;
    std::array<double, static_cast<std::size_t>(Part::Count)> m_seconds{};
    double m_worstFrame = 0.0;
    double m_gpuSeconds = 0.0;
    std::size_t m_gpuFrames = 0;
    std::size_t m_frames = 0;
    long m_physicsSteps = 0;
    std::size_t m_vehicles = 0;
};

} // namespace fh::render
