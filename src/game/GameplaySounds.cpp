#include "GameplaySounds.h"

#include "audio/AudioLibrary.h"
#include "audio/EventPlayer.h"
#include "audio/Fev.h"
#include "base/Log.h"
#include "collision/SurfaceTable.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>

namespace fh {

namespace {

constexpr const char* kUiSoundsPath = "media/audio/ui/ui4audio.xml";
constexpr const char* kGeneralProject = "Game:\\Media\\Audio\\GameModes\\General.fev";
constexpr std::string_view kGeneralPrefix = "General/";
constexpr std::string_view kSpeedTrapFlash = "SpeedTrapFlash";
/// The MaterialTypeID the car loader falls back to (0x82CB5500 takes r16,
/// which holds 1 there).
constexpr int kFallbackMaterialType = 1;

/// The General project's event ui4audio.xml plays for `name`, or empty.
std::string generalEvent(const Vfs& vfs, std::string_view name)
{
    const std::optional<std::string> text = vfs.readText(kUiSoundsPath);
    if (!text) {
        return {};
    }
    pugi::xml_document document;
    if (!document.load_buffer(text->data(), text->size())) {
        return {};
    }
    for (const pugi::xml_node node : document.child("ui4audio").child("defaults").children("event")) {
        const std::string_view group = node.attribute("group").as_string();
        if (iequals(node.attribute("play").as_string(), name) && group.starts_with(kGeneralPrefix)) {
            return std::string(group.substr(kGeneralPrefix.size())) + "/" + node.attribute("cue").as_string();
        }
    }
    return {};
}

std::vector<std::string> surfaceNames(const collision::SurfaceTable& surfaces)
{
    std::vector<std::string> names;
    names.reserve(surfaces.surfaces().size());
    for (std::size_t id = 0; id < surfaces.surfaces().size(); ++id) {
        names.push_back(surfaces.nameOf(id));
    }
    return names;
}

std::optional<std::size_t> physicsMaterial(const Database& database, int materialType)
{
    const std::optional<Row> row = database.queryOne(
        "SELECT PhysicsMaterialID FROM List_MaterialType WHERE MaterialTypeID = ?", {std::to_string(materialType)});
    if (!row) {
        return std::nullopt;
    }
    const std::int64_t id = row->integer("PhysicsMaterialID", -1);
    if (id < 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(id);
}

} // namespace

GameplaySounds::GameplaySounds(const Vfs& vfs, const Database& database, audio::Mixer& mixer,
    audio::AudioLibrary& library, const collision::SurfaceTable& surfaces, Buses buses)
    : m_database(database)
    , m_mixer(mixer)
    , m_library(library)
    , m_buses(buses)
{
    const std::vector<std::string> names = surfaceNames(surfaces);
    m_collisions = std::make_unique<audio::CollisionSounds>(
        mixer, library, audio::CollisionSoundTable::load(vfs, names), buses.collision);
    m_fallbackSurface = physicsMaterial(database, kFallbackMaterialType).value_or(0);
    m_flashEvent = generalEvent(vfs, kSpeedTrapFlash);
    try {
        m_general = library.project(kGeneralProject);
    } catch (const std::exception& e) {
        logWarning("gameplay sounds: {}", e.what());
    }
    if (m_flashEvent.empty() || !m_general || !m_general->findEvent(m_flashEvent)) {
        logWarning("gameplay sounds: no event for {}", kSpeedTrapFlash);
        m_flashEvent.clear();
    }
}

GameplaySounds::~GameplaySounds() = default;

std::size_t GameplaySounds::carSurface(std::string_view mediaName)
{
    if (const auto it = m_carSurfaces.find(mediaName); it != m_carSurfaces.end()) {
        return it->second;
    }
    std::size_t surface = m_fallbackSurface;
    const std::optional<Row> row
        = m_database.queryOne("SELECT MaterialTypeID FROM Data_Car WHERE MediaName = ?", {std::string(mediaName)});
    const std::int64_t material = row ? row->integer("MaterialTypeID", 0) : 0;
    if (material >= 1 && material <= 2) {
        surface = physicsMaterial(m_database, static_cast<int>(material)).value_or(m_fallbackSurface);
    } else {
        logWarning("gameplay sounds: {} has no body material; taking material {}", mediaName, kFallbackMaterialType);
    }
    m_carSurfaces.emplace(std::string(mediaName), surface);
    return surface;
}

void GameplaySounds::advance(float seconds)
{
    m_clockMs += static_cast<double>(seconds) * 1000.0;
}

void GameplaySounds::report(const audio::CollisionReport& report)
{
    m_collisions->report(report, static_cast<std::uint32_t>(m_clockMs));
}

void GameplaySounds::smashed(std::string_view type, const glm::vec3& position, float speed)
{
    m_collisions->playBreak(type, position, speed);
}

void GameplaySounds::speedTrapFlash()
{
    ++m_flashes;
    const audio::FevEvent* event = m_flashEvent.empty() ? nullptr : m_general->findEvent(m_flashEvent);
    if (!event) {
        return;
    }
    std::erase_if(m_flashPlaying, [](const auto& instance) { return !instance->isPlaying(); });
    // At most the event's max playbacks at once, a new flash taking the
    // oldest one's place, as ui::UiSounds treats the other UI sounds.
    while (!m_flashPlaying.empty() && m_flashPlaying.size() >= std::max<std::uint32_t>(event->maxPlaybacks, 1)) {
        m_flashPlaying.front()->stop(0.0F);
        m_flashPlaying.erase(m_flashPlaying.begin());
    }
    audio::EventSettings settings;
    settings.bus = m_buses.ui;
    auto instance
        = std::make_unique<audio::EventInstance>(m_mixer, m_library, kGeneralProject, m_general, *event, settings);
    instance->start();
    m_flashPlaying.push_back(std::move(instance));
}

bool GameplaySounds::speedTrapFlashPlaying() const
{
    return std::any_of(
        m_flashPlaying.begin(), m_flashPlaying.end(), [](const auto& instance) { return instance->isPlaying(); });
}

void GameplaySounds::update()
{
    m_collisions->update(static_cast<std::uint32_t>(m_clockMs));
}

void GameplaySounds::stopAll()
{
    m_collisions->stopAll();
    m_flashPlaying.clear();
}

} // namespace fh
