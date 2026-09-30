#include "RadioSequencer.h"

#include "base/Error.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace openhorizon::radio {

namespace {

/// The game compares these with the dialogue stream's mean square
/// (0x82c9a1e0 multiplies the sum of 256 squares by 1/256, 0x82c9a1ec
/// compares with the double 0.001).
constexpr double kSpeakingThreshold = 0.001;

/// Share of the SongStart..IdentStart span kept clear at each end when a
/// station starts mid-track (the double 0.1 at 0x8207e200).
constexpr double kMidTrackMargin = 0.1;

/// The radio's flow mode (radio system +120). The constructor sets 1 and
/// nothing traced changes it; the branches for 0 are the game's but never
/// run.
constexpr int kFlowMode = 1;

} // namespace

RadioSequencer::RadioSequencer(std::shared_ptr<const RadioCatalog> catalog, std::uint32_t seed)
    : m_catalog(std::move(catalog))
    , m_random(seed)
{
    if (!m_catalog || m_catalog->system.stations.empty()) {
        throw DataError("the radio needs at least one station");
    }
    link();
    m_offStation = m_catalog->system.offStation();
    m_first3d = m_offStation + 1;
}

float RadioSequencer::fadeRate(float seconds) const
{
    // 0x82c9ceb8: a time of zero or less becomes FLT_MAX, an instant fade.
    return seconds > 0.0F ? 1.0F / seconds : std::numeric_limits<float>::max();
}

void RadioSequencer::link()
{
    const RadioCatalog& c = *m_catalog;
    m_stations.clear();
    m_stations.reserve(c.system.stations.size());
    for (const Station& station : c.system.stations) {
        StationState state;
        state.station = &station;
        std::map<std::string, std::size_t> byName;
        for (const MusicTrack& track : station.playlist.tracks) {
            LinkedTrack linked;
            linked.track = &track;
            if (const auto info = c.musicInfo.find(track.fmodName); info != c.musicInfo.end()) {
                linked.sampleIndex = info->second.soundBankIndex;
                linked.eventStart = static_cast<float>(info->second.offsetEventStart);
                linked.songStart = static_cast<float>(info->second.offsetSongStart);
                linked.identStart = static_cast<float>(info->second.offsetIdentStart);
                linked.syncIdentStart = info->second.syncpointIndexIdentStart;
            }
            byName.emplace(track.fmodName, state.playlist.tracks.size());
            for (std::size_t t = 0; t < state.playlist.totals.size(); ++t) {
                state.playlist.totals[t] = track.likelihood[t] + state.playlist.totals[t];
            }
            state.playlist.tracks.push_back(linked);
        }
        for (const auto& [name, index] : byName) {
            state.playlist.order.push_back(index);
        }
        state.playlist.noRepeat = std::max<int>(static_cast<int>(station.playlist.tracks.size()) - 1, 0);

        const auto linkLines = [&](const VoiceList& list, bool ident) {
            LineList out;
            out.noRepeat = list.noRepeat;
            for (const VoiceLine& line : list.lines) {
                LinkedLine linked;
                linked.line = &line;
                if (const auto info = c.voiceInfo.find(line.fmodName); info != c.voiceInfo.end()) {
                    linked.sampleIndex = info->second.soundBankIndex;
                    if (ident) {
                        linked.syncStartNextTrack = info->second.syncpointStartNextTrack;
                    } else {
                        linked.lengthMs = static_cast<float>(info->second.fileLength);
                    }
                }
                out.lines.push_back(linked);
            }
            return out;
        };
        state.idents = linkLines(station.idents, true);
        state.regular = linkLines(station.regular, false);
        m_stations.push_back(std::move(state));
    }
}

const SampleTiming* RadioSequencer::timing(RadioBank bank, int sampleIndex) const
{
    const std::vector<SampleTiming>& samples = bank == RadioBank::Music ? m_catalog->musicSamples
                                                                        : m_catalog->voiceSamples;
    if (sampleIndex < 0 || static_cast<std::size_t>(sampleIndex) >= samples.size()) {
        return nullptr;
    }
    const SampleTiming& t = samples[static_cast<std::size_t>(sampleIndex)];
    return t.sampleRate > 0 ? &t : nullptr;
}

const MusicTrack* RadioSequencer::currentTrack() const
{
    if (m_currentTrack < 0) {
        return nullptr;
    }
    return m_stations[m_current].playlist.tracks[static_cast<std::size_t>(m_currentTrack)].track;
}

std::vector<RadioEvent> RadioSequencer::takeEvents()
{
    return std::exchange(m_events, {});
}

void RadioSequencer::emit(RadioEvent::Kind kind, std::string name, float startMs)
{
    RadioEvent e;
    e.kind = kind;
    e.frame = m_frame;
    e.station = m_current;
    e.name = std::move(name);
    e.startMs = startMs;
    m_events.push_back(std::move(e));
}

int RadioSequencer::pickTrack(std::size_t station)
{
    // 0x82c96000.
    Playlist& p = m_stations[station].playlist;
    const auto tod = static_cast<std::size_t>(m_timeOfDay);
    const float r = m_random.range(0.0F, p.totals[tod]);
    float sum = 0.0F;
    int picked = -1;
    int fallback = -1;
    int fallbackCounter = p.noRepeat;
    for (const std::size_t index : p.order) {
        const LinkedTrack& t = p.tracks[index];
        if (t.counter <= 0) {
            const float weight = t.track->likelihood[tod];
            if (picked < 0 && r <= weight + sum) {
                picked = static_cast<int>(index);
            }
            sum = weight + sum;
        } else if (t.counter < fallbackCounter) {
            fallbackCounter = t.counter;
            fallback = static_cast<int>(index);
        }
    }
    return picked >= 0 ? picked : fallback;
}

void RadioSequencer::trackStarted(std::size_t station, int track)
{
    // 0x82c96138: every other track moves one pick closer to playing again,
    // and the totals are rebuilt from the tracks free to play.
    Playlist& p = m_stations[station].playlist;
    p.totals = {};
    for (const std::size_t index : p.order) {
        LinkedTrack& t = p.tracks[index];
        const int counter = t.counter - 1;
        if (counter > 0) {
            t.counter = counter;
            continue;
        }
        if (static_cast<int>(index) != track) {
            for (std::size_t tod = 0; tod < p.totals.size(); ++tod) {
                p.totals[tod] = t.track->likelihood[tod] + p.totals[tod];
            }
        }
        t.counter = 0;
    }
}

RadioSequencer::LinkedLine* RadioSequencer::pickIdent(LineList& list)
{
    // 0x82c957e0.
    const int available = static_cast<int>(list.lines.size()) - list.blocked;
    const int last = available - 1;
    const int choice = last > 0 ? m_random.integer(0, last) : 0;
    LinkedLine* picked = nullptr;
    int free = 0;
    for (LinkedLine& line : list.lines) {
        if (line.counter <= 0) {
            if (free == choice) {
                picked = &line;
            }
            ++free;
            continue;
        }
        int counter = line.counter - 1;
        if (counter <= 0) {
            counter = 0;
            --list.blocked;
        }
        line.counter = counter;
    }
    if (picked && picked->counter <= 0) {
        picked->counter = list.noRepeat;
        ++list.blocked;
    }
    return picked;
}

RadioSequencer::LinkedLine* RadioSequencer::pickDialogue(LineList& list)
{
    // 0x82c95cd0: as pickIdent, except that a counter of -1 marks a line
    // switched off by its group and never counts down.
    const int available = static_cast<int>(list.lines.size()) - list.blocked;
    const int last = available - 1;
    const int choice = last > 0 ? m_random.integer(0, last) : 0;
    LinkedLine* picked = nullptr;
    int free = 0;
    for (LinkedLine& line : list.lines) {
        if (line.counter == 0) {
            if (free == choice) {
                picked = &line;
            }
            ++free;
            continue;
        }
        if (line.counter == -1) {
            continue;
        }
        int counter = line.counter - 1;
        if (counter <= 0) {
            counter = 0;
            --list.blocked;
        }
        line.counter = counter;
    }
    if (picked && picked->counter == 0) {
        picked->counter = list.noRepeat;
        ++list.blocked;
    }
    return picked;
}

std::uint64_t RadioSequencer::outputFramesTo(const RadioVoice& voice, std::uint32_t frame) const
{
    if (frame <= voice.startFrame) {
        return 0;
    }
    const std::uint64_t span = frame - voice.startFrame;
    const auto rate = static_cast<std::uint64_t>(voice.sampleRate);
    return (span * static_cast<std::uint64_t>(kOutputRate) + rate - 1) / rate;
}

std::uint32_t RadioSequencer::positionOf(const RadioVoice& voice) const
{
    const std::uint64_t moved = voice.elapsed * static_cast<std::uint64_t>(voice.sampleRate)
        / static_cast<std::uint64_t>(kOutputRate);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(voice.startFrame + moved, voice.length));
}

void RadioSequencer::startVoice(RadioChannel channel, RadioBank bank, int sampleIndex, std::uint32_t startFrame,
    const std::string& fmodName)
{
    const auto c = static_cast<std::size_t>(channel);
    RadioVoice& v = m_voices[c];
    v = RadioVoice{};
    const SampleTiming* t = timing(bank, sampleIndex);
    if (!t) {
        return;
    }
    v.active = true;
    v.bank = bank;
    v.sampleIndex = sampleIndex;
    v.sampleRate = t->sampleRate;
    v.length = t->frames;
    v.startFrame = std::min(startFrame, t->frames);
    v.instance = m_nextInstance++;
    v.fmodName = fmodName;
    std::size_t next = 0;
    while (next < t->syncPoints.size() && t->syncPoints[next] < v.startFrame) {
        ++next;
    }
    m_nextSync[c] = next;
}

void RadioSequencer::stopVoice(RadioChannel channel)
{
    m_voices[static_cast<std::size_t>(channel)] = RadioVoice{};
}

void RadioSequencer::stopAllVoices()
{
    // 0x82c94d50.
    stopVoice(RadioChannel::Music);
    stopVoice(RadioChannel::Dialogue);
    stopVoice(RadioChannel::Ident);
    m_hadMusic = false;
    m_pendingDialogue = nullptr;
    m_currentIdent = nullptr;
    emit(RadioEvent::Kind::Stopped);
}

void RadioSequencer::playTrack(int track, StartMode mode, float argument)
{
    // 0x82c97ac8.
    StationState& station = m_stations[m_current];
    if (track < 0 || static_cast<std::size_t>(track) >= station.playlist.tracks.size()) {
        return;
    }
    if (track != station.lastStarted) {
        station.lastStarted = track;
        trackStarted(m_current, track);
    }
    LinkedTrack& t = station.playlist.tracks[static_cast<std::size_t>(track)];
    t.counter = station.playlist.noRepeat;
    stopVoice(RadioChannel::Music);

    float offset = 0.0F;
    switch (mode) {
    case StartMode::Beginning:
        break;
    case StartMode::SongStart:
        offset = t.songStart;
        break;
    case StartMode::BeforeEventStart:
        offset = std::fma(-argument, 1000.0F, t.eventStart);
        break;
    case StartMode::Random: {
        const double span = static_cast<double>(t.identStart - t.songStart);
        const float end = t.identStart - t.songStart + t.songStart;
        const auto low = static_cast<float>(std::fma(span, kMidTrackMargin, static_cast<double>(t.songStart)));
        const auto high = static_cast<float>(-std::fma(span, kMidTrackMargin, -static_cast<double>(end)));
        offset = m_random.range(low, high);
        break;
    }
    case StartMode::Offset:
        offset = argument;
        break;
    case StartMode::UnderDialogue: {
        offset = t.songStart;
        const float intro = t.eventStart - t.songStart;
        if (m_dialogueLengthMs > intro) {
            offset = t.eventStart - m_dialogueLengthMs;
            if (offset < 0.0F) {
                offset = 0.0F;
            }
        }
        break;
    }
    }
    if (m_current != m_offStation) {
        station.resumeOffsetSeconds = offset * 0.001F;
        station.resumeClock = m_clock;
    }
    // FMOD is given the position in whole milliseconds (fctiwz at 0x82c97cec).
    const auto milliseconds = static_cast<std::int64_t>(offset);
    std::uint32_t startFrame = 0;
    if (const SampleTiming* timingOfTrack = timing(RadioBank::Music, t.sampleIndex); timingOfTrack
        && milliseconds > 0) {
        startFrame = static_cast<std::uint32_t>(std::min<std::int64_t>(
            milliseconds * timingOfTrack->sampleRate / 1000, timingOfTrack->frames));
    }
    startVoice(RadioChannel::Music, RadioBank::Music, t.sampleIndex, startFrame, t.track->fmodName);
    m_currentTrack = track;
    if (!m_is3d) {
        ++m_tracksSinceDialogue;
    }
    RadioEvent e;
    e.kind = RadioEvent::Kind::TrackStarted;
    e.frame = m_frame;
    e.station = m_current;
    e.name = t.track->fmodName;
    e.title = t.track->name;
    e.artist = t.track->artist;
    e.startMs = static_cast<float>(milliseconds);
    m_events.push_back(std::move(e));
    pause(m_current == m_offStation);
}

void RadioSequencer::playIdent()
{
    // 0x82c96678.
    stopVoice(RadioChannel::Ident);
    m_currentIdent = nullptr;
    std::size_t source = m_current;
    if ((!m_identsEnabled || kFlowMode == 0) && !m_is3d) {
        source = m_offStation;
    }
    LinkedLine* ident = pickIdent(m_stations[source].idents);
    if (!ident) {
        return;
    }
    startVoice(RadioChannel::Ident, RadioBank::Voice, ident->sampleIndex, 0, ident->line->fmodName);
    if (m_voices[static_cast<std::size_t>(RadioChannel::Ident)].active) {
        m_currentIdent = ident;
        emit(RadioEvent::Kind::IdentStarted, ident->line->fmodName);
    }
}

void RadioSequencer::playDialogue()
{
    // 0x82c987d0; only the regular line of the game's four kinds exists here.
    if (kFlowMode == 0) {
        playIdent();
        return;
    }
    stopVoice(RadioChannel::Dialogue);
    if (!m_pendingDialogue) {
        return;
    }
    startVoice(RadioChannel::Dialogue, RadioBank::Voice, m_pendingDialogue->sampleIndex, 0,
        m_pendingDialogue->line->fmodName);
    if (m_voices[static_cast<std::size_t>(RadioChannel::Dialogue)].active) {
        emit(RadioEvent::Kind::DialogueStarted, m_pendingDialogue->line->fmodName);
    }
}

void RadioSequencer::pause(bool paused)
{
    // vtable +208 (0x82c94be0) pauses or resumes all four streams.
    if (paused == m_paused) {
        return;
    }
    m_paused = paused;
    emit(paused ? RadioEvent::Kind::Paused : RadioEvent::Kind::Resumed);
}

void RadioSequencer::setScaleTarget(float target)
{
    m_scaleTarget = target;
}

void RadioSequencer::startFlow(std::string_view trackName, float seconds, bool immediate)
{
    // 0x82c99560, without the positional-station branches.
    const RadioSettings& settings = m_catalog->system.settings;
    if (immediate) {
        m_fadeOnChange = false;
    }
    if (trackName.empty()) {
        m_hudDelay = settings.hudNewTrackDisplayDelayFreeRoam;
        StationState& station = m_stations[m_current];
        int track = station.selected;
        StartMode mode = StartMode::Random;
        float offset = 0.0F;
        if (track >= 0) {
            const LinkedTrack& t = station.playlist.tracks[static_cast<std::size_t>(track)];
            const float span = t.identStart - t.songStart;
            const auto limit = static_cast<float>(
                -std::fma(static_cast<double>(span), kMidTrackMargin, -static_cast<double>(span + t.songStart)))
                * 0.001F;
            const float position = m_clock - station.resumeClock + station.resumeOffsetSeconds;
            offset = position < limit ? position * 1000.0F : 0.0F;
            if (offset > 0.0F) {
                mode = StartMode::Offset;
            } else {
                track = pickTrack(m_current);
                station.selected = track;
                mode = StartMode::SongStart;
            }
        } else {
            track = pickTrack(m_current);
            station.selected = track;
        }
        if (track >= 0) {
            playTrack(track, mode, offset);
            m_hudShown = false;
            m_levelTarget = 1.0F;
        }
        m_levelUpRate = fadeRate(settings.levelFadeUpTime);
    } else {
        m_hudDelay = settings.hudNewTrackDisplayDelayRace;
        const bool radioOff = m_offStation == m_lastTunable;
        std::size_t station = m_current;
        int track = -1;
        if (trackName == "Random") {
            station = m_playerStation;
            track = pickTrack(station);
            m_stations[station].selected = track;
        } else {
            for (std::size_t s = 0; s < m_stations.size() && track < 0; ++s) {
                station = s;
                for (const std::size_t index : m_stations[s].playlist.order) {
                    if (m_stations[s].playlist.tracks[index].track->fmodName == trackName) {
                        track = static_cast<int>(index);
                        break;
                    }
                }
            }
        }
        if (track < 0) {
            station = 0;
            for (const std::size_t index : m_stations[0].playlist.order) {
                if (m_stations[0].playlist.tracks[index].track->fmodName == "R1_Everyday") {
                    track = static_cast<int>(index);
                    break;
                }
            }
        }
        if (track >= 0 && !radioOff) {
            if (station != m_current) {
                m_current = station;
                emit(RadioEvent::Kind::StationChanged);
            }
            playTrack(track, StartMode::BeforeEventStart, seconds);
            m_hudShown = false;
            m_levelTarget = 1.0F;
        }
        m_levelUpRate = fadeRate(settings.levelFadeUpTimeForEvent);
    }
    m_flowStarted = true;
    if (m_appliedVolume == 0.0F || m_current == m_offStation) {
        pause(true);
    }
    if (!immediate) {
        m_fadeState = 3;
    } else {
        m_hudDelay = 0.0F;
    }
}

void RadioSequencer::startFreeRoam()
{
    startFlow({}, 0.0F, !m_fadeOnChange);
}

void RadioSequencer::startForEvent(std::string_view trackName, float secondsToEventStart)
{
    startFlow(trackName.empty() ? std::string_view("Random") : trackName, secondsToEventStart, !m_fadeOnChange);
}

void RadioSequencer::stopNow()
{
    // vtable +60 (0x82c967f8) with no delay.
    m_levelTarget = 0.0F;
    m_currentTrack = -1;
    m_flowStarted = false;
}

void RadioSequencer::stop(float delaySeconds)
{
    m_stopTimer = delaySeconds;
    if (delaySeconds == 0.0F) {
        stopNow();
    }
}

void RadioSequencer::switchStation(std::size_t index, bool restart)
{
    // 0x82c989e8.
    m_current = index;
    m_is3d = index >= m_first3d;
    if (!m_is3d) {
        m_lastTunable = index;
    }
    emit(RadioEvent::Kind::StationChanged);
    if (restart) {
        startFlow({}, 0.0F, !m_fadeOnChange);
    } else if (m_fadeOnChange) {
        m_fadeState = 3;
    }
    stopVoice(RadioChannel::Ident);
    m_currentIdent = nullptr;
    stopVoice(RadioChannel::Dialogue);
    m_pendingDialogue = nullptr;
    setScaleTarget(m_current == m_offStation ? 0.0F : 1.0F);
    m_hudShown = false;
}

void RadioSequencer::requestStation(std::size_t index, bool restart)
{
    // 0x82c99a98 (vtable +124), never forced past the lock.
    if (m_fadeState != 0 && m_fadeState != 3) {
        return;
    }
    if (m_switchingLocked || index >= m_stations.size() || index == m_current) {
        return;
    }
    if (m_fadeOnChange) {
        m_pendingStation = index;
        m_pendingRestart = restart;
        if (m_fadeState == 3) {
            m_fadeState = 2;
            switchStation(index, restart);
            m_hudShown = m_is3d;
        } else {
            m_fadeState = 1;
        }
        return;
    }
    switchStation(index, restart);
    m_hudShown = m_is3d;
}

void RadioSequencer::selectStation(std::size_t index, bool fade)
{
    // 0x82c97000 (vtable +120).
    if (index >= m_stations.size()) {
        return;
    }
    m_fadeOnChange = fade;
    const bool same = index == m_current && !m_is3d;
    requestStation(index, m_flowStarted && !same);
}

void RadioSequencer::setPlayerStation(std::size_t index)
{
    if (index < m_stations.size()) {
        m_playerStation = index;
    }
}

void RadioSequencer::nextStation()
{
    // 0x82c96f20 with the "previous" flag clear stops at the off station;
    // players remember the D-pad cycling round to the first station, so
    // stepping wraps.
    if (m_switchingLocked) {
        return;
    }
    requestStation(m_current >= m_offStation ? 0 : m_current + 1, true);
}

void RadioSequencer::previousStation()
{
    if (m_switchingLocked) {
        return;
    }
    requestStation(m_current == 0 ? m_offStation : m_current - 1, true);
}

void RadioSequencer::setTimeOfDay(TimeOfDay timeOfDay)
{
    m_timeOfDay = std::clamp(static_cast<int>(timeOfDay), 0, 3);
}

void RadioSequencer::setUserVolume(float volume)
{
    m_userVolume = std::clamp(volume, 0.0F, 1.0F);
}

void RadioSequencer::setPaused(bool paused)
{
    pause(paused);
}

void RadioSequencer::setStationIdentsEnabled(bool enabled)
{
    m_identsEnabled = enabled;
}

void RadioSequencer::setDjDialogueEnabled(bool enabled)
{
    m_dialogueDisabled = !enabled;
}

void RadioSequencer::setStationSwitchingLocked(bool locked)
{
    m_switchingLocked = locked;
}

void RadioSequencer::setDialogueGroupEnabled(int group, bool enabled)
{
    // 0x82c96498 over the tuneable stations, 0x82c95c60 per list.
    for (std::size_t s = 0; s < m_offStation && s < m_stations.size(); ++s) {
        LineList& list = m_stations[s].regular;
        for (LinkedLine& line : list.lines) {
            if (line.line->group != group) {
                continue;
            }
            const bool isEnabled = line.counter != -1;
            if (enabled != isEnabled) {
                list.blocked += enabled ? -1 : 1;
            }
            line.counter = enabled ? 0 : -1;
        }
    }
}

void RadioSequencer::onMusicSync(int index)
{
    // 0x82c97220, sync point branch.
    if (m_current == m_offStation || m_currentTrack < 0) {
        return;
    }
    const LinkedTrack& t = m_stations[m_current].playlist.tracks[static_cast<std::size_t>(m_currentTrack)];
    if (index != t.syncIdentStart) {
        return;
    }
    if (m_current >= m_first3d) {
        playIdent();
        return;
    }
    if (kFlowMode != 1) {
        return;
    }
    playIdent();
    if (m_dialogueDisabled || m_tracksSinceDialogue < m_catalog->system.settings.maxTracksBetweenDJ) {
        return;
    }
    // 0x82c96210.
    m_pendingDialogue = pickDialogue(m_stations[m_current].regular);
    m_dialogueLengthMs = m_pendingDialogue ? m_pendingDialogue->lengthMs : 0.0F;
    m_dialoguePending = true;
}

void RadioSequencer::onMusicEnd()
{
    // 0x82c97220, end branch.
    const RadioVoice& music = voice(RadioChannel::Music);
    const bool hadSound = music.active;
    emit(RadioEvent::Kind::TrackEnded, music.fmodName);
    m_currentTrack = -1;
    stopVoice(RadioChannel::Music);
    if (hadSound && !voice(RadioChannel::Ident).active && (kFlowMode == 0 || m_current == m_offStation)) {
        playIdent();
    }
}

void RadioSequencer::onIdentSync(int index)
{
    // 0x82c99cc0, sync point branch.
    if (!m_currentIdent || index != m_currentIdent->syncStartNextTrack) {
        return;
    }
    stopVoice(RadioChannel::Music);
    StationState& station = m_stations[m_current];
    const RadioSettings& settings = m_catalog->system.settings;
    if (m_dialoguePending && !m_dialogueDisabled) {
        m_dialoguePending = false;
        m_tracksSinceDialogue = 0;
        playDialogue();
        if (kFlowMode == 1) {
            const int track = pickTrack(m_current);
            station.selected = track;
            if (track >= 0) {
                m_hudDelay = settings.hudNewTrackDisplayDelayFreeRoam;
                playTrack(track, StartMode::UnderDialogue, 0.0F);
            }
        }
        return;
    }
    int track = -1;
    if (m_is3d && station.station->musicLoops) {
        track = station.lastStarted;
    } else {
        track = pickTrack(m_current);
        station.selected = track;
    }
    if (track >= 0) {
        m_hudDelay = settings.hudNewTrackDisplayDelayFreeRoam;
        playTrack(track, StartMode::SongStart, 0.0F);
        m_hudShown = false;
    }
}

void RadioSequencer::onIdentEnd()
{
    stopVoice(RadioChannel::Ident);
    m_currentIdent = nullptr;
}

void RadioSequencer::onDialogueEnd()
{
    // 0x82c981d8, end branch, for a regular line.
    emit(RadioEvent::Kind::DialogueEnded, voice(RadioChannel::Dialogue).fmodName);
    stopVoice(RadioChannel::Dialogue);
    m_pendingDialogue = nullptr;
    // The HUD shows the track again once the DJ has finished.
    m_hudShown = false;
}

std::size_t RadioSequencer::framesUntilEvent() const
{
    if (m_paused) {
        return kNoEvent;
    }
    std::uint64_t soonest = kNoEvent;
    for (std::size_t c = 0; c < kRadioChannelCount; ++c) {
        const RadioVoice& v = m_voices[c];
        if (!v.active) {
            continue;
        }
        const SampleTiming* t = timing(v.bank, v.sampleIndex);
        std::uint32_t target = v.length;
        if (t && m_nextSync[c] < t->syncPoints.size()) {
            target = std::min(target, t->syncPoints[m_nextSync[c]]);
        }
        const std::uint64_t at = outputFramesTo(v, target);
        soonest = std::min(soonest, at > v.elapsed ? at - v.elapsed : 0);
    }
    return static_cast<std::size_t>(soonest);
}

void RadioSequencer::advance(std::size_t frames)
{
    if (!m_paused) {
        for (RadioVoice& v : m_voices) {
            if (v.active) {
                v.elapsed += frames;
            }
        }
    }
    m_frame += frames;
    if (m_paused) {
        return;
    }
    // Fire everything due now; handlers may start voices, which have no
    // events at their first frame unless a sync point sits there.
    for (bool fired = true; fired;) {
        fired = false;
        for (std::size_t c = 0; c < kRadioChannelCount && !fired; ++c) {
            const RadioVoice& v = m_voices[c];
            if (!v.active) {
                continue;
            }
            const auto channel = static_cast<RadioChannel>(c);
            const SampleTiming* t = timing(v.bank, v.sampleIndex);
            if (t && m_nextSync[c] < t->syncPoints.size()
                && v.elapsed >= outputFramesTo(v, t->syncPoints[m_nextSync[c]])) {
                const int index = static_cast<int>(m_nextSync[c]++);
                if (channel == RadioChannel::Music) {
                    onMusicSync(index);
                } else if (channel == RadioChannel::Ident) {
                    onIdentSync(index);
                }
                fired = true;
            } else if (v.elapsed >= outputFramesTo(v, v.length)) {
                if (channel == RadioChannel::Music) {
                    onMusicEnd();
                } else if (channel == RadioChannel::Ident) {
                    onIdentEnd();
                } else {
                    onDialogueEnd();
                }
                fired = true;
            }
        }
    }
}

void RadioSequencer::update(float dt, float dialogueMeanSquare)
{
    // 0x82c9a088.
    const RadioSettings& settings = m_catalog->system.settings;
    if (m_stopTimer > 0.0F) {
        m_stopTimer -= dt;
        if (m_stopTimer <= 0.0F) {
            m_stopTimer = 0.0F;
            stopNow();
        }
    }
    const bool speaking = voice(RadioChannel::Dialogue).active && !m_paused
        && static_cast<double>(dialogueMeanSquare) > kSpeakingThreshold;

    if (m_level < m_levelTarget) {
        m_level = std::min(m_level + m_levelUpRate * dt, m_levelTarget);
    } else if (m_level > m_levelTarget) {
        m_level -= fadeRate(settings.levelFadeDownTime) * dt;
        if (m_level < m_levelTarget) {
            m_level = m_levelTarget;
            stopAllVoices();
        }
    }

    // 0x82c99b98: the station-change fade, when one was asked for.
    if (m_fadeOnChange) {
        const float rate = fadeRate(settings.stationChangeFadeTimeNormal);
        if (m_fadeState == 1) {
            if (m_stationFade > 0.0F) {
                m_stationFade = std::max(m_stationFade - rate * dt, 0.0F);
            }
            if (m_stationFade == 0.0F) {
                m_fadeState = 2;
                switchStation(m_pendingStation, m_pendingRestart);
            }
        }
        if (m_fadeState == 3) {
            if (m_stationFade < 1.0F) {
                m_stationFade = std::min(m_stationFade + rate * dt, 1.0F);
            }
            if (m_stationFade == 1.0F) {
                m_fadeState = 0;
            }
        }
    }

    // A flow whose streams have all ended starts the next track.
    if (m_flowStarted && m_hadMusic && !voice(RadioChannel::Music).active && !voice(RadioChannel::Ident).active
        && !voice(RadioChannel::Dialogue).active) {
        const int track = pickTrack(m_current);
        m_stations[m_current].selected = track;
        if (track >= 0) {
            playTrack(track, StartMode::SongStart, 0.0F);
        }
    }

    const float duckTarget = speaking ? settings.duckValue : 1.0F;
    if (m_duck < duckTarget) {
        m_duck = std::min(m_duck + fadeRate(settings.duckFadeUpTime) * dt, duckTarget);
    } else if (m_duck > duckTarget) {
        m_duck = std::max(m_duck - fadeRate(settings.duckFadeDownTime) * dt, duckTarget);
    }
    if (m_scale < m_scaleTarget) {
        m_scale = std::min(m_scale + fadeRate(settings.scaleFadeUpTime) * dt, m_scaleTarget);
    } else if (m_scale > m_scaleTarget) {
        m_scale = std::max(m_scale - fadeRate(settings.scaleFadeDownTime) * dt, m_scaleTarget);
    }

    // The radio pauses when its volume is turned down to nothing and resumes
    // when it is turned up again.
    const float previousVolume = m_appliedVolume;
    m_appliedVolume = m_userVolume;
    if (m_current != m_offStation) {
        if (m_appliedVolume == 0.0F && previousVolume > 0.0F && !m_paused) {
            pause(true);
        } else if (previousVolume == 0.0F && m_appliedVolume > 0.0F && m_paused) {
            pause(false);
        }
    }

    const float base = settings.masterLevel * m_stationFade * m_scale * m_level * m_appliedVolume;
    m_musicGain = settings.musicLevel * m_duck * base;
    m_dialogueGain = settings.dialogueLevel * base;
    m_identGain = settings.identLevel * base;

    if (voice(RadioChannel::Music).active) {
        m_hadMusic = true;
        if (!m_paused) {
            m_clock += dt;
        }
    }

    if (!m_hudShown && !m_is3d && m_appliedVolume > 0.0F) {
        m_hudShown = true;
        RadioEvent e;
        e.kind = RadioEvent::Kind::HudShow;
        e.frame = m_frame;
        e.station = m_current;
        e.delaySeconds = m_hudDelay;
        if (const MusicTrack* track = currentTrack(); track && m_current != m_offStation) {
            e.name = track->fmodName;
            e.title = track->name;
            e.artist = track->artist;
        }
        m_events.push_back(std::move(e));
    }
}

} // namespace openhorizon::radio
