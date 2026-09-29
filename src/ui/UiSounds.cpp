#include "UiSounds.h"

#include "audio/AudioLibrary.h"
#include "audio/EventPlayer.h"
#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>

namespace fh::ui {

namespace {

constexpr const char* kUiSoundsPath = "media/audio/ui/ui4audio.xml";
/// The project of the UIInGame sound groups, as the executable names it.
constexpr const char* kUiProject = "Game:\\Media\\Audio\\UI\\UIInGame\\UIInGame.fev";
constexpr std::string_view kUiProjectPrefix = "UIInGame/";

bool sameName(std::string_view a, std::string_view b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

} // namespace

UiSoundTable UiSoundTable::parse(std::string_view xml, const std::string& source)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", source);
    }
    const pugi::xml_node defaults = document.child("ui4audio").child("defaults");
    if (!defaults) {
        throw DataError("{}: no ui4audio defaults", source);
    }
    UiSoundTable table;
    for (const pugi::xml_node node : defaults.children("event")) {
        const std::string_view group = node.attribute("group").as_string();
        if (!group.starts_with(kUiProjectPrefix)) {
            continue;
        }
        UiSound sound;
        sound.name = node.attribute("play").as_string();
        sound.event = std::string(group.substr(kUiProjectPrefix.size())) + "/" + node.attribute("cue").as_string();
        sound.looping = node.attribute("isLooping").as_bool();
        table.m_sounds.push_back(std::move(sound));
    }
    return table;
}

UiSoundTable UiSoundTable::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kUiSoundsPath);
    if (!text) {
        throw DataError("{} is missing", kUiSoundsPath);
    }
    return parse(*text, kUiSoundsPath);
}

const UiSound* UiSoundTable::find(std::string_view name) const
{
    const auto it = std::find_if(
        m_sounds.begin(), m_sounds.end(), [&](const UiSound& sound) { return sameName(sound.name, name); });
    return it == m_sounds.end() ? nullptr : &*it;
}

UiSounds::UiSounds(const Vfs& vfs, audio::Mixer& mixer, audio::AudioLibrary& library, audio::BusId bus)
    : m_mixer(mixer)
    , m_library(library)
    , m_bus(bus)
    , m_table(UiSoundTable::load(vfs))
{
    try {
        m_project = m_library.project(kUiProject);
    } catch (const DataError& e) {
        logError("ui sounds: {}", e.what());
    }
}

UiSounds::~UiSounds() = default;

void UiSounds::prune()
{
    std::erase_if(m_playing, [](const Playing& playing) {
        return !playing.instance->isStarted() || (!playing.looping && !playing.instance->isPlaying());
    });
}

void UiSounds::play(std::string_view name)
{
    m_played.emplace_back(name);
    const UiSound* sound = m_table.find(name);
    const audio::FevEvent* event = sound && m_project ? m_project->findEvent(sound->event) : nullptr;
    if (!event) {
        if (std::find(m_missing.begin(), m_missing.end(), name) == m_missing.end()) {
            m_missing.emplace_back(name);
            logWarning("ui sounds: no event for {}", name);
        }
        return;
    }
    prune();
    std::vector<Playing*> same;
    for (Playing& playing : m_playing) {
        if (&playing.instance->event() == event) {
            same.push_back(&playing);
        }
    }
    if (!same.empty() && same.size() >= std::max<std::uint32_t>(event->maxPlaybacks, 1)) {
        audio::EventInstance& oldest = *same.front()->instance;
        oldest.stop(0.0F);
        oldest.start();
        // The restarted instance is now the newest.
        auto it = std::find_if(
            m_playing.begin(), m_playing.end(), [&](const Playing& playing) { return &playing == same.front(); });
        std::rotate(it, it + 1, m_playing.end());
        return;
    }
    try {
        audio::EventSettings settings;
        settings.bus = m_bus;
        auto instance
            = std::make_unique<audio::EventInstance>(m_mixer, m_library, kUiProject, m_project, *event, settings);
        instance->start();
        m_playing.push_back({std::string(name), sound->looping, std::move(instance)});
    } catch (const DataError& e) {
        logError("ui sounds: {}: {}", name, e.what());
    }
}

void UiSounds::stop(std::string_view name)
{
    for (Playing& playing : m_playing) {
        if (sameName(playing.name, name)) {
            playing.instance->stop();
        }
    }
    prune();
}

void UiSounds::setParameter(std::string_view name, std::string_view parameter, float value)
{
    for (Playing& playing : m_playing) {
        if (sameName(playing.name, name)) {
            playing.instance->setParameter(parameter, value);
        }
    }
}

bool UiSounds::isPlaying(std::string_view name) const
{
    return std::any_of(m_playing.begin(), m_playing.end(), [&](const Playing& playing) {
        return sameName(playing.name, name) && playing.instance->isStarted()
            && (playing.looping || playing.instance->isPlaying());
    });
}

} // namespace fh::ui
