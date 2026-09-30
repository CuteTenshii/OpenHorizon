#pragma once

#include "render/Gl.h"

#include <array>
#include <chrono>
#include <cstddef>

namespace fh::render {

/// Where frame time goes, logged every few seconds: CPU time per part of the
/// frame, and GPU time per drawing part from timestamp queries. A player's
/// log then shows whether a slow frame rate comes from the GPU or from which
/// part of the CPU work.
///
/// The GPU time of a part runs from the GPU passing the previous mark to it
/// passing this one, so it includes any time the GPU sat idle waiting for
/// the CPU to send that part's commands: a part whose GPU time is close to
/// its CPU time is limited by the CPU, and only one whose GPU time is well
/// above its CPU time is limited by the GPU. The simulation parts are left
/// out of the GPU total, which the previous single elapsed-time query
/// counted as GPU time although the GPU is idle through them.
class FrameStats {
public:
    enum class Part : std::size_t {
        Simulation,
        Physics,
        Streaming,
        /// The sun shadows: their maps, the screen depth and the mask.
        DrawShadows,
        /// The dynamic environment cube the cars reflect: its faces and
        /// their mip chain (EnvironmentCube).
        DrawEnvironmentCube,
        DrawWorld,
        /// The animated objects: posing them and drawing them.
        DrawAnimated,
        /// The crowds: their models and sprites (CrowdRenderer).
        DrawCrowd,
        DrawCars,
        /// The sky and the sun, moon and stars drawn with it.
        DrawSky,
        DrawGlass,
        DrawDrivingLine,
        DrawParticles,
        /// The world-space overlay and the tonemap into the 16:9 picture.
        Tonemap,
        /// The fade and the HUD, minimap included.
        DrawHud,
        /// The pause menu, the full map and the loading screen.
        DrawMenus,
        /// Fitting the picture into the window and swapping buffers: waiting
        /// for vsync, or for the GPU to catch up.
        Present,
        Count,
    };
    static constexpr std::size_t kParts = static_cast<std::size_t>(Part::Count);
    using PartTimes = std::array<double, kParts>;

    /// Needs a current GL context.
    FrameStats();
    ~FrameStats();
    FrameStats(const FrameStats&) = delete;
    FrameStats& operator=(const FrameStats&) = delete;

    void beginFrame();
    /// Charges the time since the last mark (or the frame's start) to `part`.
    void mark(Part part);
    /// Ends the GPU timing of this frame; call right after the last GL call
    /// before the buffers are swapped.
    void endGpuFrame();
    /// Closes the frame; logs a summary once `kInterval` seconds have passed.
    void endFrame(int physicsSteps, std::size_t vehicles);

    /// CPU seconds per part of the frame in progress, or of the last one
    /// until the next beginFrame().
    const PartTimes& frameSeconds() const { return m_frame; }
    /// GPU seconds per part of the most recent frame whose queries have
    /// completed (a few frames old); zero until one has.
    const PartTimes& gpuFrameSeconds() const { return m_gpuFrame; }
    /// For profiling only: waits for the GPU at every mark, so each part's
    /// CPU time includes finishing its GPU work, and records how long each
    /// wait took (waitSeconds()): the GPU time a part needs beyond sending
    /// its commands. It serialises CPU and GPU, so frames get slower.
    void setSerialised(bool serialised) { m_serialised = serialised; }
    /// With setSerialised(true), the seconds the last frame waited at the
    /// end of each part for the GPU to finish it.
    const PartTimes& waitSeconds() const { return m_wait; }

    static const char* partName(Part part);

private:
    using Clock = std::chrono::steady_clock;
    static constexpr double kInterval = 5.0;
    // Enough frames in flight that reading a result never waits on the GPU.
    static constexpr std::size_t kFrames = 4;
    // A frame marks each part at most a few times (the simulation three).
    static constexpr std::size_t kMarks = 24;

    struct GpuFrame {
        std::array<GLuint, kMarks + 1> queries{};
        std::array<Part, kMarks> parts{};
        std::size_t marks = 0;
        bool pending = false;
    };

    void timestamp(Part part);
    void collectGpu();

    std::array<GpuFrame, kFrames> m_gpu{};
    std::size_t m_gpuSlot = 0;
    Clock::time_point m_intervalStart;
    Clock::time_point m_frameStart;
    Clock::time_point m_last;
    PartTimes m_seconds{};
    PartTimes m_frame{};
    PartTimes m_gpuSeconds{};
    PartTimes m_gpuFrame{};
    PartTimes m_wait{};
    double m_worstFrame = 0.0;
    std::size_t m_gpuFrames = 0;
    std::size_t m_frames = 0;
    long m_physicsSteps = 0;
    std::size_t m_vehicles = 0;
    bool m_serialised = false;
};

} // namespace fh::render
