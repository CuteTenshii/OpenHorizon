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

/// One event collisiondata.xml names: the FMOD project (the Group's first
/// segment) and the event's path inside it (the rest of the Group, then Event).
struct CollisionEventRef {
    std::string project;
    std::string event;

    bool empty() const { return event.empty(); }
};

/// media/audio/collisions/collisiondata.xml as its loader (0x82C94770, slot 7 of vtable 0x8214E954) reads it.
/// Verified:
/// - MSBetweenDiffCollisions: ms a contact lasts after its last report (constructor 0x82C932F0 sets 250; disc 75);
/// - CollisionEvent (0x82C93660), skipped unless both surfaces are listed: children matched by name (0x82C928A0),
///   Impact3D, ImpactLFE and Scrape3D fill per-pair tables in both orders, later wins; Whoosh3D goes nowhere;
/// - BreakEvent (0x82C93918), skipped unless the smashable is listed: Impact3D and Whoosh3D by smashable.
/// Inferred: the surface list is surfaceTypes.xml's; smashable names match without case.
class CollisionSoundTable {
public:
    enum class Slot : std::uint8_t { Impact3D, ImpactLFE, Scrape3D };
    static constexpr std::size_t kSlotCount = 3;
    /// The constructor's value, kept when the file does not give one.
    static constexpr std::uint32_t kDefaultMsBetweenDiffCollisions = 250;

    /// `surfaceNames` are the surface types in id order. Throws DataError naming `source` on bad XML.
    static CollisionSoundTable parse(
        std::string_view xml, std::span<const std::string> surfaceNames, const std::string& source);
    /// Throws DataError if the file is missing or malformed.
    static CollisionSoundTable load(const Vfs& vfs, std::span<const std::string> surfaceNames);

    std::uint32_t msBetweenDiffCollisions() const { return m_msBetweenDiffCollisions; }
    std::size_t surfaceCount() const { return m_surfaceCount; }
    /// Either order; null when the file has none or an id is past the list.
    const CollisionEventRef* event(std::size_t surfaceA, std::size_t surfaceB, Slot slot) const;
    /// The Impact3D of the smashable's BreakEvent, or null.
    const CollisionEventRef* breakEvent(std::string_view smashable) const;
    /// Surface pairs with an event in the slot, counted once per order.
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

/// One contact, as the record slot 9 (report(), 0x82C93C90) reads.
struct CollisionReport {
    /// Ids the caller keeps stable while the bodies touch.
    std::uint32_t bodyA = 0;
    std::uint32_t bodyB = 0;
    /// surfaceTypes.xml ids of the two bodies' surfaces at the contact.
    std::size_t surfaceA = 0;
    std::size_t surfaceB = 0;
    glm::vec3 position{0.0F};
    /// Metres per second; only its part along `normal` is heard.
    glm::vec3 relativeVelocity{0.0F};
    /// The contact's unit normal.
    glm::vec3 normal{0.0F};
    /// Its length becomes MomentumDiff. What the game puts there was not traced.
    glm::vec3 momentum{0.0F};
    /// The contact broke a smashable (BreakState 1).
    bool broke = false;
    /// The faster car's speed in metres per second, which the game hands to CarSpeedMph unconverted.
    float carSpeed = 0.0F;
    /// Neither body is the player's car (AIOnlyCollision 1).
    bool aiOnly = false;
};

/// The collision sound manager (vtable 0x8214E954): impacts, low-frequency thumps and scrapes per contact.
/// Verified:
/// - report() (0x82C93C90): a new contact plays Impact3D and ImpactLFE once and starts Scrape3D, with
///   MomentumDiff (momentum length clamped 0..299), RelativeVelocity (m/s times 2.2369, mph), BreakState,
///   CarSpeedMph (as given) and AIOnlyCollision; a contact in progress updates its events except BreakState;
/// - the contact handler (0x824B8B30) gives a contact with a car |normal . relative velocity| (0x824B8D14) and
///   the larger of the cars' speeds (0x824B8ECC);
/// - update() (0x82C93098) ends contacts older than MSBetweenDiffCollisions, stopping scrapes;
/// - playBreak() (0x82C94498): RelativeVelocity is the vector's first component times 2.2369; MomentumDiff,
///   source not traced, stays 0.
/// Inferred: the volume is the snapshots' Collision category (stays 1 here); max playbacks steal the oldest;
/// the cars' speeds are in m/s, as 0x825E98D8 converts the same getter's result to mph.
/// Missing: the scrapes' Time parameter (FMOD runs it at 0.2/s over 0..5; stays 0 here) and the
/// BreakEvents' Whoosh3D (slot 10, 0x82C946F0, only hands its names out).
class CollisionSounds {
public:
    /// For tests and tools.
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
    /// Ends contacts not reported for more than MSBetweenDiffCollisions.
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
    /// Drops the oldest instances of `event` until fewer than `maxPlaybacks` remain.
    void makeRoom(const void* event, std::uint32_t maxPlaybacks);
    /// Also clears the contacts' pointers to it.
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
