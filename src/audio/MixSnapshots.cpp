#include "MixSnapshots.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>

namespace openhorizon::audio {

namespace {

constexpr const char* kSnapshotsPath = "media/audio/audiomixersnapshots.xml";

constexpr std::array<std::string_view, kMixCategoryCount> kCategoryNames = {
    "FocusCar",
    "OppCar",
    "Transmission",
    "SuperCharger",
    "Turbo",
    "EngineDamage",
    "Backfire",
    "Suspension",
    "CarWind",
    "Tire",
    "Collision",
    "AmbientStatic",
    "AmbientDynamic",
    "GameMusic",
    "HomespaceAmbientDynamic",
    "HomespaceMusic",
    "MenuMusic",
    "GameUI",
    "VO",
    "SatNav",
    "DJModifier",
    "RallyCoDriver",
};

constexpr std::array<std::string_view, kMixGroupCount> kGroupNames = {
    "Location",
    "Speed",
    "Gameplay",
    "Impact",
    "SpeedZones",
    "VO",
    "Reverb",
    "Radio",
    "SatNav",
    "RallyCoDriver",
};

constexpr std::array<std::string_view, kMixSnapshotCount> kSnapshotNames = {
    "FreeRoam",
    "Race",
    "UIExterior",
    "UIInterior",
    "Loading",
    "PreEvent",
    "Paused",
    "PausedMultiplayer",
    "Rewind",
    "Video",
    "FadeOut",
    "FadeOutQuick",
    "RaceEncounter",
    "FadeOutWristbandReward",
    "Barnfind",
    "Crash",
    "TopSpeed",
    "LowSpeed",
    "DefaultLocation",
    "MainTown",
    "Mountains",
    "RedRock",
    "Festival",
    "AverageSpeedEnter",
    "AverageSpeedExit",
    "SpeedCam",
    "VOPlaying",
    "VONotPlaying",
    "VOPlayingOnUIExterior",
    "VOPlayingInstant",
    "InTunnel",
    "NotInTunnel",
    "OpeningCutscene",
    "OpeningCutsceneIntroCar",
    "RadioDJSpeaking",
    "RadioFestivalUpdate",
    "RadioNormal",
    "SatNavPlaying",
    "SatNavNotPlaying",
    "RallyCoDriverPlaying",
    "RallyCoDriverNotPlaying",
    "RaceRally",
    "LoadingRally",
};

/// The snapshots apply() does not save as the one to go back to (0x82C6C910).
bool transientGameplay(MixSnapshotId id)
{
    return id == MixSnapshotId::Paused || id == MixSnapshotId::PausedMultiplayer || id == MixSnapshotId::Loading
        || id == MixSnapshotId::Rewind;
}

} // namespace

std::string_view mixCategoryName(MixCategory category)
{
    return kCategoryNames[static_cast<std::size_t>(category)];
}

std::string_view mixSnapshotName(MixSnapshotId snapshot)
{
    return kSnapshotNames[static_cast<std::size_t>(snapshot)];
}

std::optional<MixSnapshotId> findMixSnapshot(std::string_view name)
{
    // The lookup (0x82C6F088) compares whole names with case.
    for (std::size_t i = 0; i < kSnapshotNames.size(); ++i) {
        if (kSnapshotNames[i] == name) {
            return static_cast<MixSnapshotId>(i);
        }
    }
    return std::nullopt;
}

MixSnapshots MixSnapshots::parse(std::string_view xml, const std::string& source)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", source);
    }
    const pugi::xml_node root = document.child("MixSnapshots");
    if (!root) {
        throw DataError("{}: no MixSnapshots element", source);
    }
    MixSnapshots out;
    for (std::size_t s = 0; s < kMixSnapshotCount; ++s) {
        const pugi::xml_node node = root.child(std::string(kSnapshotNames[s]).c_str());
        if (!node) {
            continue;
        }
        MixSnapshot& snapshot = out.m_snapshots[s];
        snapshot.present = true;
        const std::string_view group = node.attribute("group").as_string();
        for (std::size_t g = 0; g < kGroupNames.size(); ++g) {
            if (kGroupNames[g] == group) {
                snapshot.group = static_cast<MixGroup>(g);
            }
        }
        for (std::size_t c = 0; c < kMixCategoryCount; ++c) {
            const pugi::xml_node element = node.child(std::string(kCategoryNames[c]).c_str());
            if (!element) {
                continue;
            }
            MixSetting& setting = snapshot.settings[c];
            setting.active = element.attribute("active").as_bool();
            setting.volume = element.attribute("destvolume").as_float(1.0F);
            setting.fadeTime = element.attribute("fadetime").as_float();
            setting.returnTime = element.attribute("returntime").as_float();
            setting.returnFadeTime = element.attribute("returnfadetime").as_float();
        }
    }
    return out;
}

MixSnapshots MixSnapshots::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kSnapshotsPath);
    if (!text) {
        throw DataError("{} is missing", kSnapshotsPath);
    }
    return parse(*text, kSnapshotsPath);
}

SnapshotMixer::SnapshotMixer(const MixSnapshots& snapshots)
    : m_snapshots(snapshots)
{
    // The reset (0x82C73D70) sets every group's volumes to 1.
    for (auto& group : m_groups) {
        group.fill(1.0F);
    }
}

float& SnapshotMixer::group(MixGroup group, MixCategory category)
{
    return m_groups[static_cast<std::size_t>(group)][static_cast<std::size_t>(category)];
}

float SnapshotMixer::groupVolume(MixGroup group, MixCategory category) const
{
    return m_groups[static_cast<std::size_t>(group)][static_cast<std::size_t>(category)];
}

std::optional<MixSnapshotId> SnapshotMixer::current(MixGroup group) const
{
    return m_current[static_cast<std::size_t>(group)];
}

float SnapshotMixer::volume(MixCategory category) const
{
    float product = 1.0F;
    for (const auto& group : m_groups) {
        product *= group[static_cast<std::size_t>(category)];
    }
    return std::clamp(product, 0.0F, 1.0F);
}

void SnapshotMixer::apply(MixSnapshotId id)
{
    const MixSnapshot& snapshot = m_snapshots.snapshot(id);
    const std::optional<MixSnapshotId> gameplay = current(MixGroup::Gameplay);
    if (snapshot.group == MixGroup::Gameplay && gameplay && !transientGameplay(*gameplay)) {
        m_saved = gameplay;
    }
    if (!snapshot.group) {
        return;
    }
    const MixGroup group = *snapshot.group;
    const std::optional<MixSnapshotId> previous = current(group);
    for (std::size_t c = 0; c < kMixCategoryCount; ++c) {
        const MixSetting& setting = snapshot.settings[c];
        if (!setting.active) {
            continue;
        }
        // Without a return of its own, the fade takes the return fade time
        // of the snapshot it replaces in the group, when that has one.
        float fadeTime = setting.fadeTime;
        if (setting.returnTime <= 0.0F && previous) {
            const float previousReturn = m_snapshots.snapshot(*previous).settings[c].returnFadeTime;
            if (previousReturn > 0.0F) {
                fadeTime = previousReturn;
            }
        }
        startFade(static_cast<MixCategory>(c), group, setting, fadeTime);
    }
    m_current[static_cast<std::size_t>(group)] = id;
}

void SnapshotMixer::restore()
{
    if (m_saved) {
        apply(*m_saved);
    }
}

void SnapshotMixer::startFade(MixCategory category, MixGroup group, const MixSetting& setting, float fadeTime)
{
    // Every fade the game starts from a snapshot passes 1 as the level the
    // fader returns to (0x82C659E4).
    constexpr float kReturnLevel = 1.0F;
    const float volume = this->group(group, category);
    const auto running = std::find_if(m_faders.begin(), m_faders.end(),
        [&](const Fader& fader) { return fader.group == group && fader.category == category; });
    if (running != m_faders.end()) {
        running->from = kReturnLevel;
        running->current = volume;
        running->target = setting.volume;
        running->fadeTime = fadeTime;
        running->returnTime = setting.returnTime;
        running->returnFadeTime = setting.returnFadeTime;
        running->step = std::abs(kReturnLevel - setting.volume);
        if (running->step == 0.0F) {
            running->step = 1.0F;
        }
        return;
    }
    Fader fader;
    fader.group = group;
    fader.category = category;
    fader.current = volume;
    fader.from = kReturnLevel;
    fader.target = setting.volume;
    fader.step = std::abs(volume - setting.volume);
    fader.fadeTime = fadeTime;
    fader.returnTime = setting.returnTime;
    fader.returnFadeTime = setting.returnFadeTime;
    m_faders.push_back(fader);
}

void SnapshotMixer::update(float seconds)
{
    for (Fader& fader : m_faders) {
        if (fader.current != fader.target) {
            if (!fader.fadeTimer) {
                fader.fadeTimer = true;
            } else {
                const bool rising = fader.current < fader.target;
                if (fader.fadeTime != 0.0F) {
                    const float moved = seconds / fader.fadeTime * fader.step;
                    fader.current += rising ? moved : -moved;
                } else {
                    fader.current = fader.target;
                }
                if (rising ? fader.current >= fader.target : fader.current <= fader.target) {
                    fader.current = fader.target;
                    if (fader.returnTime == 0.0F) {
                        fader.fadeTimer = false;
                    }
                }
                group(fader.group, fader.category) = fader.current;
            }
        }
        if (fader.returnTime != 0.0F) {
            if (!fader.returnTimer) {
                fader.returnTimer = true;
                fader.returnElapsed = 0.0F;
            } else {
                fader.returnElapsed += seconds;
            }
            if (fader.returnElapsed > fader.returnTime) {
                fader.returnTimer = false;
                fader.fadeTimer = true;
                fader.returnTime = 0.0F;
                if (fader.current != fader.from) {
                    fader.target = fader.from;
                    fader.fadeTime = fader.returnFadeTime;
                } else {
                    fader.fadeTimer = false;
                }
            }
        }
    }
    std::erase_if(m_faders, [](const Fader& fader) { return !fader.fadeTimer && !fader.returnTimer; });
}

} // namespace openhorizon::audio
