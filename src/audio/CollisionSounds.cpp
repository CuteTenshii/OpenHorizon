#include "CollisionSounds.h"

#include "AudioLibrary.h"
#include "EventPlayer.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <glm/geometric.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

namespace openhorizon::audio {

namespace {

constexpr const char* kCollisionDataPath = "media/audio/collisions/collisiondata.xml";

/// The projects collisiondata.xml's groups name that this manager plays, by
/// the name that starts a group, and the path the executable loads each
/// from (its project list, 0x8200D830 for Collisions). AMB_Default, which
/// only the BreakEvents' Whoosh3D names, is not played here.
constexpr std::array<std::pair<std::string_view, const char*>, 1> kProjects = {{
    {"Collisions", "Game:\\Media\\Audio\\Collisions\\Collisions.fev"},
}};

/// Metres per second to miles per hour, the executable's constant
/// (0x82005AE8) that both report() and playBreak() multiply by.
constexpr float kMphPerMetrePerSecond = 2.2369F;
/// report() and playBreak() clamp the momentum's length to 0..299
/// (0x8214EA10).
constexpr float kMaxMomentumDiff = 299.0F;

std::string lower(std::string_view text)
{
    std::string out(text);
    std::transform(
        out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// The slot a child element fills (0x82C928A0), or nothing for Whoosh3D and
/// anything else.
std::optional<CollisionSoundTable::Slot> slotOf(std::string_view element)
{
    if (element == "Impact3D") {
        return CollisionSoundTable::Slot::Impact3D;
    }
    if (element == "ImpactLFE") {
        return CollisionSoundTable::Slot::ImpactLFE;
    }
    if (element == "Scrape3D") {
        return CollisionSoundTable::Slot::Scrape3D;
    }
    return std::nullopt;
}

CollisionEventRef eventRef(const pugi::xml_node& node)
{
    const std::string_view group = node.attribute("Group").as_string();
    const std::string_view event = node.attribute("Event").as_string();
    CollisionEventRef ref;
    const std::size_t slash = group.find('/');
    if (slash == std::string_view::npos || event.empty()) {
        return ref;
    }
    ref.project = std::string(group.substr(0, slash));
    ref.event = std::string(group.substr(slash + 1)) + "/" + std::string(event);
    return ref;
}

const char* projectPath(std::string_view project)
{
    for (const auto& [name, path] : kProjects) {
        if (iequals(name, project)) {
            return path;
        }
    }
    return nullptr;
}

const char* slotName(CollisionSoundTable::Slot slot)
{
    switch (slot) {
    case CollisionSoundTable::Slot::Impact3D:
        return "Impact3D";
    case CollisionSoundTable::Slot::ImpactLFE:
        return "ImpactLFE";
    case CollisionSoundTable::Slot::Scrape3D:
        return "Scrape3D";
    }
    return "";
}

} // namespace

CollisionSoundTable CollisionSoundTable::parse(
    std::string_view xml, std::span<const std::string> surfaceNames, const std::string& source)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", source);
    }
    const pugi::xml_node root = document.child("CollisionData");
    if (!root) {
        throw DataError("{}: no CollisionData root", source);
    }
    CollisionSoundTable table;
    table.m_surfaceCount = surfaceNames.size();
    table.m_slots.assign(kSlotCount, std::vector<CollisionEventRef>(table.m_surfaceCount * table.m_surfaceCount));
    if (const pugi::xml_attribute ms = root.attribute("MSBetweenDiffCollisions")) {
        table.m_msBetweenDiffCollisions = ms.as_uint();
    }
    const auto surfaceId = [&](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < surfaceNames.size(); ++i) {
            if (surfaceNames[i] == name) {
                return i;
            }
        }
        return std::nullopt;
    };
    for (const pugi::xml_node node : root.children()) {
        const std::string_view name = node.name();
        if (name == "CollisionEvent") {
            const std::optional<std::size_t> a = surfaceId(node.attribute("Surface1").as_string());
            const std::optional<std::size_t> b = surfaceId(node.attribute("Surface2").as_string());
            if (!a || !b) {
                continue;
            }
            for (const pugi::xml_node child : node.children()) {
                const std::optional<Slot> slot = slotOf(child.name());
                if (!slot) {
                    continue;
                }
                const CollisionEventRef ref = eventRef(child);
                auto& entries = table.m_slots[static_cast<std::size_t>(*slot)];
                entries[table.index(*a, *b)] = ref;
                entries[table.index(*b, *a)] = ref;
            }
        } else if (name == "BreakEvent") {
            const pugi::xml_node impact = node.child("Impact3D");
            if (impact) {
                table.m_breaks[lower(node.attribute("Smashable").as_string())] = eventRef(impact);
            }
        }
    }
    return table;
}

CollisionSoundTable CollisionSoundTable::load(const Vfs& vfs, std::span<const std::string> surfaceNames)
{
    const std::optional<std::string> text = vfs.readText(kCollisionDataPath);
    if (!text) {
        throw DataError("{} is missing", kCollisionDataPath);
    }
    return parse(*text, surfaceNames, kCollisionDataPath);
}

const CollisionEventRef* CollisionSoundTable::event(std::size_t surfaceA, std::size_t surfaceB, Slot slot) const
{
    if (surfaceA >= m_surfaceCount || surfaceB >= m_surfaceCount) {
        return nullptr;
    }
    const CollisionEventRef& ref = m_slots[static_cast<std::size_t>(slot)][index(surfaceA, surfaceB)];
    return ref.empty() ? nullptr : &ref;
}

const CollisionEventRef* CollisionSoundTable::breakEvent(std::string_view smashable) const
{
    const auto it = m_breaks.find(lower(smashable));
    return it == m_breaks.end() || it->second.empty() ? nullptr : &it->second;
}

std::size_t CollisionSoundTable::pairCount(Slot slot) const
{
    const auto& entries = m_slots[static_cast<std::size_t>(slot)];
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [](const CollisionEventRef& ref) { return !ref.empty(); }));
}

CollisionSounds::CollisionSounds(Mixer& mixer, AudioLibrary& library, CollisionSoundTable table, BusId bus)
    : m_mixer(mixer)
    , m_library(library)
    , m_table(std::move(table))
    , m_bus(bus)
{
}

CollisionSounds::~CollisionSounds() = default;

EventInstance* CollisionSounds::start(const CollisionEventRef& ref, CollisionSoundTable::Slot slot,
    const glm::vec3& position, float momentumDiff, float relativeVelocityMph, bool broke, float carSpeedMph,
    bool aiOnly)
{
    const std::string key = ref.project + "/" + ref.event;
    if (std::find(m_missing.begin(), m_missing.end(), key) != m_missing.end()) {
        return nullptr;
    }
    const char* path = projectPath(ref.project);
    std::shared_ptr<const FevProject> project;
    const FevEvent* event = nullptr;
    if (path) {
        try {
            project = m_library.project(path);
            event = project->findEvent(ref.event);
        } catch (const std::exception& e) {
            logWarning("audio: collision sounds: {}: {}", path, e.what());
        }
    }
    if (!event) {
        logWarning("audio: collision sounds: no event {} ({})", key, slotName(slot));
        m_missing.push_back(key);
        return nullptr;
    }
    makeRoom(event, std::max<std::uint32_t>(event->maxPlaybacks, 1));
    EventSettings settings;
    settings.bus = m_bus;
    settings.spatial = true;
    settings.position = position;
    settings.seed = m_seed++;
    auto instance = std::make_unique<EventInstance>(m_mixer, m_library, path, project, *event, settings);
    instance->setParameter("MomentumDiff", momentumDiff);
    instance->setParameter("RelativeVelocity", relativeVelocityMph);
    instance->setParameter("BreakState", broke ? 1.0F : 0.0F);
    instance->setParameter("CarSpeedMph", carSpeedMph);
    instance->setParameter("AIOnlyCollision", aiOnly ? 1.0F : 0.0F);
    instance->start();
    m_started.push_back(
        Started{ref.event, slot, relativeVelocityMph, carSpeedMph, momentumDiff, broke, aiOnly, position});
    m_instances.push_back(std::move(instance));
    return m_instances.back().get();
}

void CollisionSounds::drop(std::size_t index)
{
    const EventInstance* gone = m_instances[index].get();
    for (Contact& contact : m_contacts) {
        for (EventInstance** slot : {&contact.impact, &contact.lfe, &contact.scrape}) {
            if (*slot == gone) {
                *slot = nullptr;
            }
        }
    }
    m_instances.erase(m_instances.begin() + static_cast<std::ptrdiff_t>(index));
}

void CollisionSounds::makeRoom(const void* event, std::uint32_t maxPlaybacks)
{
    std::size_t count = 0;
    for (const auto& instance : m_instances) {
        count += &instance->event() == event ? 1 : 0;
    }
    for (std::size_t i = 0; i < m_instances.size() && count >= maxPlaybacks;) {
        if (&m_instances[i]->event() == event) {
            m_instances[i]->stop();
            drop(i);
            --count;
        } else {
            ++i;
        }
    }
}

void CollisionSounds::report(const CollisionReport& report, std::uint32_t nowMs)
{
    const float momentumDiff = std::clamp(glm::length(report.momentum), 0.0F, kMaxMomentumDiff);
    const float relativeVelocityMph = glm::length(report.relativeVelocity) * kMphPerMetrePerSecond;
    const float carSpeedMph = report.carSpeed * kMphPerMetrePerSecond;
    const auto same = [&](const Contact& c) {
        return (c.bodyA == report.bodyA && c.bodyB == report.bodyB && c.surfaceA == report.surfaceA
                   && c.surfaceB == report.surfaceB)
            || (c.bodyA == report.bodyB && c.bodyB == report.bodyA && c.surfaceA == report.surfaceB
                && c.surfaceB == report.surfaceA);
    };
    if (const auto it = std::find_if(m_contacts.begin(), m_contacts.end(), same); it != m_contacts.end()) {
        it->lastMs = nowMs;
        if (it->impact) {
            it->impact->setPosition(report.position);
        }
        if (it->scrape) {
            it->scrape->setPosition(report.position);
            it->scrape->setParameter("MomentumDiff", momentumDiff);
            it->scrape->setParameter("RelativeVelocity", relativeVelocityMph);
            it->scrape->setParameter("CarSpeedMph", carSpeedMph);
            it->scrape->setParameter("AIOnlyCollision", report.aiOnly ? 1.0F : 0.0F);
        }
        return;
    }
    Contact contact;
    contact.bodyA = report.bodyA;
    contact.bodyB = report.bodyB;
    contact.surfaceA = report.surfaceA;
    contact.surfaceB = report.surfaceB;
    contact.lastMs = nowMs;
    using Slot = CollisionSoundTable::Slot;
    const auto play = [&](Slot slot) -> EventInstance* {
        const CollisionEventRef* ref = m_table.event(report.surfaceA, report.surfaceB, slot);
        return ref ? start(*ref, slot, report.position, momentumDiff, relativeVelocityMph, report.broke, carSpeedMph,
                         report.aiOnly)
                   : nullptr;
    };
    contact.impact = play(Slot::Impact3D);
    contact.lfe = play(Slot::ImpactLFE);
    contact.scrape = play(Slot::Scrape3D);
    m_contacts.push_back(contact);
}

void CollisionSounds::update(std::uint32_t nowMs)
{
    const std::uint32_t hold = m_table.msBetweenDiffCollisions();
    for (auto it = m_contacts.begin(); it != m_contacts.end();) {
        if (nowMs - it->lastMs > hold) {
            if (it->scrape) {
                it->scrape->stop();
            }
            it = m_contacts.erase(it);
        } else {
            ++it;
        }
    }
    for (std::size_t i = 0; i < m_instances.size();) {
        const EventInstance& instance = *m_instances[i];
        const bool scraping = std::any_of(
            m_contacts.begin(), m_contacts.end(), [&](const Contact& c) { return c.scrape == &instance; });
        if (!scraping && !instance.isPlaying()) {
            drop(i);
        } else {
            ++i;
        }
    }
}

void CollisionSounds::playBreak(std::string_view smashable, const glm::vec3& position, float speed)
{
    const CollisionEventRef* ref = m_table.breakEvent(smashable);
    if (!ref) {
        return;
    }
    start(*ref, CollisionSoundTable::Slot::Impact3D, position, 0.0F, speed * kMphPerMetrePerSecond, false, 0.0F, false);
}

void CollisionSounds::stopAll()
{
    for (const auto& instance : m_instances) {
        instance->stop();
    }
    m_contacts.clear();
    m_instances.clear();
}

std::size_t CollisionSounds::scrapesPlaying() const
{
    return static_cast<std::size_t>(std::count_if(m_contacts.begin(), m_contacts.end(),
        [](const Contact& c) { return c.scrape != nullptr && c.scrape->isStarted(); }));
}

} // namespace openhorizon::audio
