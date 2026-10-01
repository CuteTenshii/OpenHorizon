#pragma once

#include "Mixer.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

class AudioLibrary;
class EventInstance;
class FevProject;

/// Game path of the tyre event project and of its surface table.
inline constexpr std::string_view kTiresProjectPath = "media/audio/cars/tires/tires.fev";
inline constexpr std::string_view kTiresXmlPath = "media/audio/cars/tires/tires.xml";

/// One `<SurfaceMap>` of Tires.xml: a surfaceTypes.xml name's event group (with the project name,
/// "Tires/Tires/LOD1/OnRoad") and its SurfaceIndex value.
struct TyreSurface {
    std::string physicsSurface;
    std::string lod1EventGroup;
    std::string lod2EventGroup;
    int surfaceIndex = 0;
};

/// The surface table of Tires.xml, in file order.
struct TyreSurfaceMap {
    std::vector<TyreSurface> surfaces;

    /// Throws DataError if it is not XML or has no `<Tires>` root.
    static TyreSurfaceMap parse(std::string_view xml, const std::string& source = std::string(kTiresXmlPath));
    /// Reads kTiresXmlPath. Throws DataError if it is missing or bad.
    static TyreSurfaceMap load(const Vfs& vfs);

    /// Compared without case.
    std::optional<std::size_t> find(std::string_view physicsSurface) const;
};

/// The user properties of Tires.fev's "Tires/Tires" group (0x82547CA8). Rates are units per second.
struct TyreSmoothing {
    float combinedSlipRate = 0.0F;
    float slipAngleRate = 0.0F;
    float slipRatioRate = 0.0F;
    float normLoadRate = 0.0F;
    float tireRpmRate = 0.0F;
    /// Seconds a wheel must be off the ground before its sounds stop.
    float timeUntilLeavesGround = 0.0F;

    /// Throws DataError if the group or any property is missing.
    static TyreSmoothing fromProject(const FevProject& project);
};

/// Slips are normalised, 1 at the tyre's peak grip (inferred; see TyreSound).
struct TyreInput {
    bool onGround = false;
    /// A surfaceTypes.xml name; empty when the ground has none.
    std::string_view surface;
    /// Sign ignored.
    float slipAngle = 0.0F;
    /// Positive while the wheel spins, negative while it locks.
    float slipRatio = 0.0F;
    float combinedSlip = 0.0F;
    /// Vertical load, newtons.
    float load = 0.0F;
    /// Wheel angular speed, rad/s (sign ignored).
    float spin = 0.0F;
    /// World space.
    glm::vec3 position{0.0F};
};

/// The car as a whole, for the conversions the game makes per wheel.
struct TyreCarInput {
    /// Kilograms.
    float mass = 1.0F;
    /// The drivetrain turns backwards; the game negates SlipRatio then.
    bool reversing = false;
    /// Where the single pair of LOD2 events plays when spatial.
    glm::vec3 position{0.0F};
};

struct TyreSoundOptions {
    /// LOD1: roll and skid events per wheel. LOD2: one pair, fed by the wheel slipping most.
    bool perWheel = true;
    /// Otherwise the events play centred.
    bool spatial = false;
    float volume = 1.0F;
    BusId bus = kMasterBus;
    std::uint32_t seed = 1;
};

/// Tires.fev's rolling and skidding sounds (car audio update 0x82C83278, tyre events 0x82C81FF0).
/// Verified: per wheel, rpm 0..20000, load * 4 / (mass * 9.80665) 0..3, |slip angle| 0..5, slip ratio -5..5
/// (negated in reverse), combined slip 0..5 and the surface (0x82514DB8), each smoothed by TyreSmoothing;
/// airborne past TimeUntilTireLeavesGround, raw values and no surface. Roll_XX and Skid_XX run while
/// grounded; nothing sets TireTemperature. LOD2: the wheel with the most combined slip drives one pair.
/// Inferred: slips are normalised to the peak (curves span -5..5; writer of CCarDynamics wheel +0x208,
/// +0x1F0, +0x260 not found); the player's car is LOD1 (flag +0x215, meaning not traced).
/// Not reproduced: SurfaceChanges.xml's one-shots and the empty HeavyBrakingLayer event.
class TyreSound {
public:
    static constexpr std::size_t kWheels = 4;

    /// Slow: decodes every sample. Throws DataError if Tires.fev or Tires.xml is missing or malformed.
    TyreSound(Mixer& mixer, std::shared_ptr<AudioLibrary> library, TyreSoundOptions options = {});
    ~TyreSound();
    TyreSound(const TyreSound&) = delete;
    TyreSound& operator=(const TyreSound&) = delete;

    /// Wheels in CarPhysics order.
    void update(std::span<const TyreInput, kWheels> wheels, const TyreCarInput& car, float dt);
    void setVolume(float volume);

    const TyreSmoothing& smoothing() const { return m_smoothing; }
    const TyreSurfaceMap& surfaces() const { return m_surfaces; }
    /// Whether any of the events is sounding, for tools and tests.
    bool isPlaying() const;

    /// The wheel's smoothed parameters, as last sent to its events.
    struct Parameters {
        float tireRpm = 0.0F;
        float combinedSlip = 0.0F;
        float slipAngle = 0.0F;
        float slipRatio = 0.0F;
        float normLoad = 0.0F;
        /// Index into surfaces(), or -1 off the ground or on an unlisted surface.
        int surface = -1;
        bool grounded = false;
    };
    const Parameters& parameters(std::size_t wheel) const { return m_wheels.at(wheel).smoothed; }

private:
    /// The Roll and Skid events of one event group, created on first use.
    struct EventPair {
        std::unique_ptr<EventInstance> roll;
        std::unique_ptr<EventInstance> skid;
        /// So a missing event warns once.
        bool created = false;
    };
    struct Wheel {
        Parameters smoothed;
        float airborneTime = 0.0F;
        /// Kept while airborne, so landing on the same surface is no change.
        int previousSurface = -1;
        /// Per Tires.xml entry, since the game keeps events per surface.
        std::vector<EventPair> events;
    };

    std::unique_ptr<EventInstance> makeEvent(const std::string& path);
    EventPair& eventsFor(Wheel& wheel, int surface, std::size_t wheelIndex);
    EventPair& lod2EventsFor(int surface);
    void smooth(Wheel& wheel, const TyreInput& input, const TyreCarInput& car, float dt) const;
    void updatePerWheel(std::span<const TyreInput, kWheels> wheels);
    void updateSingle(const TyreCarInput& car);
    void feed(EventInstance& event, const Parameters& parameters, bool combined) const;
    static void stop(EventPair& pair);

    Mixer& m_mixer;
    std::shared_ptr<AudioLibrary> m_library;
    std::shared_ptr<const FevProject> m_project;
    TyreSoundOptions m_options;
    TyreSurfaceMap m_surfaces;
    TyreSmoothing m_smoothing;
    std::array<Wheel, kWheels> m_wheels;
    std::vector<EventPair> m_lod2Events;
    int m_lod2PreviousSurface = -1;
    std::uint32_t m_nextSeed = 1;
};

} // namespace openhorizon::audio
