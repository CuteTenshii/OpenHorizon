#pragma once

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::audio {

class AudioLibrary;
class EventInstance;
class FevProject;
class Mixer;

/// Game path of the tyre event project and of its surface table.
inline constexpr std::string_view kTiresProjectPath = "media/audio/cars/tires/tires.fev";
inline constexpr std::string_view kTiresXmlPath = "media/audio/cars/tires/tires.xml";

/// One `<SurfaceMap>` of Tires.xml: which event group a physics surface
/// (a surfaceTypes.xml name) plays its tyre sounds from, and the value the
/// events' SurfaceIndex parameter takes on it. Event group paths start with
/// the project name ("Tires/Tires/LOD1/OnRoad").
struct TyreSurface {
    std::string physicsSurface;
    std::string lod1EventGroup;
    std::string lod2EventGroup;
    int surfaceIndex = 0;
};

/// The surface table of Tires.xml, in file order.
struct TyreSurfaceMap {
    std::vector<TyreSurface> surfaces;

    /// Parses the file's text. Throws DataError if it is not XML or has no
    /// `<Tires>` root.
    static TyreSurfaceMap parse(std::string_view xml, const std::string& source = std::string(kTiresXmlPath));
    /// Reads kTiresXmlPath. Throws DataError if it is missing or bad.
    static TyreSurfaceMap load(const Vfs& vfs);

    /// The entry for a physics surface name, compared without case.
    std::optional<std::size_t> find(std::string_view physicsSurface) const;
};

/// The tyre parameter smoothing the game reads from the user properties of
/// Tires.fev's "Tires" event group (section "Tires/Tires" in the executable,
/// 0x82547CA8). Rates are parameter units per second.
struct TyreSmoothing {
    float combinedSlipRate = 0.0F;
    float slipAngleRate = 0.0F;
    float slipRatioRate = 0.0F;
    float normLoadRate = 0.0F;
    float tireRpmRate = 0.0F;
    /// Seconds a wheel must be off the ground before its sounds stop.
    float timeUntilLeavesGround = 0.0F;

    /// Reads the six properties. Throws DataError if the group or any of
    /// them is missing.
    static TyreSmoothing fromProject(const FevProject& project);
};

/// One wheel as the tyre sounds hear it. The slips are the tyre model's
/// normalised values, 1 at the tyre's peak grip; see TyreSound for what is
/// verified about them.
struct TyreInput {
    bool onGround = false;
    /// The surfaceTypes.xml name of the ground under the wheel; empty when
    /// the ground has no surface type.
    std::string_view surface;
    /// Slip angle over the peak slip angle; the sign is ignored.
    float slipAngle = 0.0F;
    /// Slip ratio over the peak slip ratio: positive while the wheel spins
    /// faster than the ground passes, negative while it locks.
    float slipRatio = 0.0F;
    /// Combined slip over the peak.
    float combinedSlip = 0.0F;
    /// Vertical load, newtons.
    float load = 0.0F;
    /// Wheel angular speed, rad/s (sign ignored).
    float spin = 0.0F;
    /// Wheel centre in the world, where its sounds play when spatial.
    glm::vec3 position{0.0F};
};

/// The car as a whole, for the conversions the game makes per wheel.
struct TyreCarInput {
    /// Kilograms; the Load parameter is load over a quarter of the car's
    /// weight.
    float mass = 1.0F;
    /// The drivetrain turns backwards. The game negates SlipRatio then, so
    /// that spinning and locking keep their meaning in reverse.
    bool reversing = false;
    /// Where the single pair of LOD2 events plays when spatial.
    glm::vec3 position{0.0F};
};

struct TyreSoundOptions {
    /// LOD1: a roll and a skid event per wheel, each fed its own wheel's
    /// slips. LOD2: one roll and one skid event, fed by the wheel slipping
    /// most, as the game plays cars other than the one in focus.
    bool perWheel = true;
    /// Place the events in the world (at TyreInput::position for LOD1,
    /// TyreCarInput::position for LOD2); otherwise they play centred.
    bool spatial = false;
    float volume = 1.0F;
    std::uint32_t seed = 1;
};

/// The rolling and skidding sounds of a car's tyres, as the game drives
/// Tires.fev (car audio update 0x82C83278, tyre events 0x82C81FF0).
///
/// Verified in the executable: every frame the game gathers per wheel the
/// wheel's angular speed as rpm (clamped to 0..20000), its load times 4 over
/// mass times 9.80665 (clamped 0..3), the absolute slip angle (0..5), the
/// slip ratio (-5..5, negated while the drivetrain turns backwards), the
/// combined slip (0..5) and the surface under it (0x82514DB8). Each value
/// then moves towards its target no faster than the Tires group's rate
/// (TyreSmoothing). A wheel off the ground keeps its last values and
/// surface until it has been airborne for TimeUntilTireLeavesGround, then
/// takes the raw values and loses its surface. On a surface, a wheel's
/// Roll_XX and Skid_XX events of the surface's Tires.xml group are started
/// if they are not playing and receive TireRPM, SlipAngle, SlipRatio, Load
/// and SurfaceIndex (the surface's Tires.xml index); nothing sets
/// TireTemperature, which stays at its minimum. When the surface changes,
/// the old surface's events stop and the new ones start; off the ground
/// they stop. Skid events therefore run all the time a wheel is on the
/// ground and fall silent through their own volume curves at low slip. In
/// LOD2 the wheel with the largest combined slip (among grounded wheels
/// with any slip) drives one Roll and one Skid event with TireRPM,
/// CombinedSlip, Load and SurfaceIndex; with no such wheel they stop.
///
/// Inferred, not verified: that the game's slip angle, slip ratio and
/// combined slip are normalised to the tyre's peak. The writer of those
/// wheel fields (CCarDynamics wheel +0x208, +0x1F0, +0x260) was not found;
/// the evidence is the events' curves, which are drawn over -5..5 of slip
/// ratio with road noise fading out beyond +-1 and heavy braking starting at
/// -0.5, a range a raw slip ratio (never below -1) could not use, and the
/// physics' "NormSlipAngle"/"NormSlipRatio" telemetry names. Which cars the
/// game plays in LOD1 is decided by a flag of the car audio input (+0x215)
/// whose meaning was not traced; the player's car in LOD1 and the others in
/// LOD2 is inferred. Not reproduced: the surface-change one-shots of
/// SurfaceChanges.xml (their TireVelocity input is a vector length the
/// VMX128 code computes from a wheel vector that was not identified) and the
/// empty HeavyBrakingLayer event.
class TyreSound {
public:
    static constexpr std::size_t kWheels = 4;

    /// Loads Tires.fev and Tires.xml and decodes every sample the events
    /// can play. Throws DataError if either file is missing or malformed.
    TyreSound(Mixer& mixer, std::shared_ptr<AudioLibrary> library, TyreSoundOptions options = {});
    ~TyreSound();
    TyreSound(const TyreSound&) = delete;
    TyreSound& operator=(const TyreSound&) = delete;

    /// Advances by `dt` seconds with the wheels in CarPhysics order: front
    /// left, front right, rear left, rear right.
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
        /// Index into surfaces(), or -1 off the ground or on a surface
        /// Tires.xml does not list.
        int surface = -1;
        bool grounded = false;
    };
    const Parameters& parameters(std::size_t wheel) const { return m_wheels.at(wheel).smoothed; }

private:
    /// The Roll and Skid events of one event group, created on first use.
    struct EventPair {
        std::unique_ptr<EventInstance> roll;
        std::unique_ptr<EventInstance> skid;
        /// Set once creation was tried, so a missing event warns once.
        bool created = false;
    };
    struct Wheel {
        Parameters smoothed;
        float airborneTime = 0.0F;
        /// The surface the wheel's events were last started for; kept
        /// while the wheel is off the ground, so landing on the same
        /// surface does not count as a change.
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

} // namespace fh::audio
