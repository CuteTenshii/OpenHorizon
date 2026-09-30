#pragma once

#include "Mixer.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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

/// One event collisiondata.xml names: the FMOD project, which is the first
/// segment of the entry's Group ("Collisions/Collisions/Asphalt/CarAluminum"
/// is project Collisions), and the event's path inside it (the rest of the
/// group, then the Event attribute).
struct CollisionEventRef {
    std::string project;
    std::string event;

    bool empty() const { return event.empty(); }
};

/// media/audio/collisions/collisiondata.xml as the collision sound
/// manager's loader (0x82C94770, vtable slot 7 of 0x8214E954) reads it.
///
/// Verified in the executable:
/// - the root's MSBetweenDiffCollisions is how long, in milliseconds, a
///   pair of bodies stays in contact after its last report (the
///   constructor, 0x82C932F0, sets 250 before the file is read; the disc's
///   file has 75);
/// - a CollisionEvent (0x82C93660) names two surfaces by their
///   surfaceTypes.xml names and is skipped unless both are in the list the
///   loader is given; its children are matched by name (0x82C928A0):
///   Impact3D, ImpactLFE and Scrape3D fill three tables indexed by the
///   surface pair, each entry stored for both orders, a later entry
///   replacing an earlier one; Whoosh3D and other children go nowhere;
/// - a BreakEvent (0x82C93918) names a smashable and is skipped unless the
///   smashable is in the loader's list; its Impact3D and Whoosh3D children
///   fill two tables by smashable.
///
/// Inferred: the surface list the loader gets is surfaceTypes.xml's (its
/// names are the ones the file uses, and a car's PhysicsMaterialID indexes
/// it); smashable names are matched without case (the file writes them in
/// lower case, CollObjs.xml's PhysicsType in mixed case).
class CollisionSoundTable {
public:
    enum class Slot : std::uint8_t { Impact3D, ImpactLFE, Scrape3D };
    static constexpr std::size_t kSlotCount = 3;
    /// The constructor's value, kept when the file does not give one.
    static constexpr std::uint32_t kDefaultMsBetweenDiffCollisions = 250;

    /// `surfaceNames` are the surface types in id order. Throws DataError
    /// naming `source` if the text is not XML with a CollisionData root.
    static CollisionSoundTable parse(
        std::string_view xml, std::span<const std::string> surfaceNames, const std::string& source);
    /// Reads media/audio/collisions/collisiondata.xml. Throws DataError if
    /// it is missing or malformed.
    static CollisionSoundTable load(const Vfs& vfs, std::span<const std::string> surfaceNames);

    std::uint32_t msBetweenDiffCollisions() const { return m_msBetweenDiffCollisions; }
    std::size_t surfaceCount() const { return m_surfaceCount; }
    /// The event for a pair of surface ids, in either order; null when the
    /// file has none or an id is past the list.
    const CollisionEventRef* event(std::size_t surfaceA, std::size_t surfaceB, Slot slot) const;
    /// The Impact3D of the smashable's BreakEvent, or null.
    const CollisionEventRef* breakEvent(std::string_view smashable) const;
    /// How many surface pairs (counted once per order) have an event in
    /// the slot.
    std::size_t pairCount(Slot slot) const;
    std::size_t breakEventCount() const { return m_breaks.size(); }

private:
    std::size_t index(std::size_t surfaceA, std::size_t surfaceB) const { return surfaceA * m_surfaceCount + surfaceB; }

    std::uint32_t m_msBetweenDiffCollisions = kDefaultMsBetweenDiffCollisions;
    std::size_t m_surfaceCount = 0;
    std::vector<std::vector<CollisionEventRef>> m_slots;
    /// By lower-case smashable name.
    std::map<std::string, CollisionEventRef, std::less<>> m_breaks;
};

/// What the game's physics tells the collision sound manager about one
/// contact (the record its slot 9, 0x82C93C90, reads).
struct CollisionReport {
    /// The two bodies, as ids the caller keeps stable while they touch; a
    /// contact continues while the same two bodies touch through the same
    /// surfaces, in either order.
    std::uint32_t bodyA = 0;
    std::uint32_t bodyB = 0;
    /// surfaceTypes.xml ids of the two bodies' surfaces at the contact.
    std::size_t surfaceA = 0;
    std::size_t surfaceB = 0;
    glm::vec3 position{0.0F};
    /// How fast the bodies meet at the contact, metres per second.
    glm::vec3 relativeVelocity{0.0F};
    /// The record's momentum vector, whose length becomes MomentumDiff.
    /// What the game puts there was not traced; only the detached car
    /// part events follow MomentumDiff, and no caller here fills it.
    glm::vec3 momentum{0.0F};
    /// The contact broke a smashable (BreakState 1).
    bool broke = false;
    /// The car's speed, metres per second (CarSpeedMph).
    float carSpeed = 0.0F;
    /// Neither body is the player's car (AIOnlyCollision 1).
    bool aiOnly = false;
};

/// The collision sound manager (vtable 0x8214E954): impacts, their
/// low-frequency thump and scrapes, per contact between two bodies, played
/// from the events CollisionSoundTable names.
///
/// Verified in the executable:
/// - report() (0x82C93C90) looks the pair up among the contacts in
///   progress. A contact already in progress takes the report's time, its
///   impact event takes the report's position and its scrape event the
///   position and the parameters below except BreakState. A new contact
///   plays the pair's Impact3D and ImpactLFE events once and starts its
///   Scrape3D event, each at the manager's volume and the report's position
///   with MomentumDiff (the length of the momentum vector, clamped to
///   0..299), RelativeVelocity (the length of the relative velocity times
///   2.2369, miles per hour), BreakState, CarSpeedMph and AIOnlyCollision;
///   the contact is recorded whether or not the pair has any event.
/// - update() (0x82C93098) gives every contact's events the manager's
///   volume and ends each contact whose last report is more than
///   MSBetweenDiffCollisions old, stopping its scrape; impacts play out.
/// - playBreak() (0x82C94498) plays a smashable's BreakEvent Impact3D once
///   with MomentumDiff and RelativeVelocity (the first component of the
///   vector it is given times 2.2369). Here that is the speed of the hit;
///   MomentumDiff, whose source was not traced, stays 0 (no BreakEvent's
///   event follows it).
///
/// Inferred: the manager's volume, which the game passes to update(), is
/// the Collision category of the mixer snapshots; here the events play on
/// the bus given and the volume stays 1. An event plays at most its max
/// playbacks at once, a new one taking the place of the oldest (FMOD
/// Designer's default behaviour, as UiSounds takes it). Missing: the
/// scrapes' Time parameter, which FMOD runs by itself at its velocity
/// (0.2 per second over 0..5); how FMOD wraps or holds it at the end was
/// not found, so it stays at 0 (the curve's first value, full volume). The
/// BreakEvents' Whoosh3D, which another caller plays (slot 10, 0x82C946F0,
/// only hands its names out).
class CollisionSounds {
public:
    /// One event started, with the parameters it started with, for tests
    /// and tools.
    struct Started {
        std::string event;
        CollisionSoundTable::Slot slot = CollisionSoundTable::Slot::Impact3D;
        float relativeVelocityMph = 0.0F;
        float carSpeedMph = 0.0F;
        float momentumDiff = 0.0F;
        bool broke = false;
        bool aiOnly = false;
        glm::vec3 position{0.0F};
    };

    CollisionSounds(Mixer& mixer, AudioLibrary& library, CollisionSoundTable table, BusId bus);
    ~CollisionSounds();
    CollisionSounds(const CollisionSounds&) = delete;
    CollisionSounds& operator=(const CollisionSounds&) = delete;

    const CollisionSoundTable& table() const { return m_table; }

    /// A contact reported at `nowMs` on the caller's millisecond clock.
    void report(const CollisionReport& report, std::uint32_t nowMs);
    /// Ends contacts not reported for more than MSBetweenDiffCollisions and
    /// drops events that finished sounding.
    void update(std::uint32_t nowMs);
    /// A smashable broke at `position`, hit at `speed` metres per second.
    void playBreak(std::string_view smashable, const glm::vec3& position, float speed);
    /// Stops everything, as leaving the world does.
    void stopAll();

    /// Contacts in progress.
    std::size_t contactCount() const { return m_contacts.size(); }
    /// Scrape events started and not yet stopped.
    std::size_t scrapesPlaying() const;
    /// Every event started, in order.
    const std::vector<Started>& started() const { return m_started; }

private:
    struct Contact {
        std::uint32_t bodyA = 0;
        std::uint32_t bodyB = 0;
        std::size_t surfaceA = 0;
        std::size_t surfaceB = 0;
        std::uint32_t lastMs = 0;
        /// Owned by m_instances; null once the event was dropped.
        EventInstance* impact = nullptr;
        EventInstance* lfe = nullptr;
        EventInstance* scrape = nullptr;
    };

    EventInstance* start(const CollisionEventRef& ref, CollisionSoundTable::Slot slot, const glm::vec3& position,
        float momentumDiff, float relativeVelocityMph, bool broke, float carSpeedMph, bool aiOnly);
    /// Stops and drops the oldest instances of `event` until fewer than
    /// `maxPlaybacks` remain.
    void makeRoom(const void* event, std::uint32_t maxPlaybacks);
    /// Drops the instance at `index`, clearing the contacts' pointers to it.
    void drop(std::size_t index);

    Mixer& m_mixer;
    AudioLibrary& m_library;
    CollisionSoundTable m_table;
    BusId m_bus;
    std::vector<Contact> m_contacts;
    /// Every event instance, oldest first.
    std::vector<std::unique_ptr<EventInstance>> m_instances;
    std::vector<Started> m_started;
    /// Events that could not be found or loaded, logged once each.
    std::vector<std::string> m_missing;
    std::uint32_t m_seed = 1;
};

} // namespace openhorizon::audio
