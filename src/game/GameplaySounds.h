#pragma once

#include "audio/CollisionSounds.h"
#include "audio/Mixer.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Database;
class Vfs;

namespace audio {
class AudioLibrary;
class EventInstance;
class FevProject;
} // namespace audio

namespace collision {
class SurfaceTable;
}

/// The sounds of cars against the world and each other: contacts, props
/// breaking, and the speed trap camera's shutter.
/// Verified: a car's surface is List_MaterialType's PhysicsMaterialID for
/// its MaterialTypeID, 1 outside 1..2 (0x82CB54DC-0x82CB558C, car +0x2CF0).
/// A counted speed trap pass plays UI sound "SpeedTrapFlash" (0x8296CF94,
/// 0x824A3770), from General.fev (0x8200D5B4).
class GameplaySounds {
public:
    struct Buses {
        audio::BusId collision = audio::kMasterBus;
        audio::BusId ui = audio::kMasterBus;
    };

    /// Throws DataError if collisiondata.xml cannot be read.
    GameplaySounds(const Vfs& vfs, const Database& database, audio::Mixer& mixer, audio::AudioLibrary& library,
        const collision::SurfaceTable& surfaces, Buses buses);
    ~GameplaySounds();
    GameplaySounds(const GameplaySounds&) = delete;
    GameplaySounds& operator=(const GameplaySounds&) = delete;

    /// The fallback material's surface when the car is not in the database.
    std::size_t carSurface(std::string_view mediaName);

    /// Moves the contact clock on by one physics step.
    void advance(float seconds);
    /// A contact during the current physics step.
    void report(const audio::CollisionReport& report);
    /// A smashable of CollObjs.xml type `type` broke at `position`, hit at
    /// `speed` metres per second.
    void smashed(std::string_view type, const glm::vec3& position, float speed);
    /// A speed trap pass counted.
    void speedTrapFlash();
    /// Ends contacts that stopped and drops finished sounds; once a frame.
    void update();
    void stopAll();

    const audio::CollisionSounds& collisions() const { return *m_collisions; }
    /// Speed trap flashes started so far.
    std::size_t speedTrapFlashes() const { return m_flashes; }
    /// Whether a speed trap flash is sounding.
    bool speedTrapFlashPlaying() const;

private:
    const Database& m_database;
    audio::Mixer& m_mixer;
    audio::AudioLibrary& m_library;
    Buses m_buses;
    std::unique_ptr<audio::CollisionSounds> m_collisions;
    std::map<std::string, std::size_t, std::less<>> m_carSurfaces;
    std::size_t m_fallbackSurface = 0;
    /// Milliseconds of physics time, the clock contacts are timed on.
    double m_clockMs = 0.0;
    /// Empty when ui4audio.xml names none.
    std::string m_flashEvent;
    std::shared_ptr<const audio::FevProject> m_general;
    std::vector<std::unique_ptr<audio::EventInstance>> m_flashPlaying;
    std::size_t m_flashes = 0;
};

} // namespace openhorizon
