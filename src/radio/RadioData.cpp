#include "RadioData.h"

#include "base/Bytes.h"
#include "base/Error.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>

namespace openhorizon::radio {

namespace {

pugi::xml_attribute attribute(const pugi::xml_node& node, std::string_view name)
{
    for (pugi::xml_attribute a = node.first_attribute(); a; a = a.next_attribute()) {
        if (iequals(a.name(), name)) {
            return a;
        }
    }
    return {};
}

std::string text(const pugi::xml_node& node, std::string_view name)
{
    const pugi::xml_attribute a = attribute(node, name);
    return a ? a.value() : "";
}

float number(const pugi::xml_node& node, std::string_view name, float fallback)
{
    const pugi::xml_attribute a = attribute(node, name);
    return a ? a.as_float(fallback) : fallback;
}

int integer(const pugi::xml_node& node, std::string_view name, int fallback)
{
    const pugi::xml_attribute a = attribute(node, name);
    return a ? a.as_int(fallback) : fallback;
}

bool flag(const pugi::xml_node& node, std::string_view name, bool fallback)
{
    const pugi::xml_attribute a = attribute(node, name);
    return a ? a.as_bool(fallback) : fallback;
}

pugi::xml_document load(std::string_view xml, const std::string& source)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw DataError("{}: not valid XML: {} at offset {}", source, result.description(), result.offset);
    }
    return doc;
}

VoiceList voiceList(const pugi::xml_node& node, const char* element)
{
    VoiceList list;
    list.noRepeat = integer(node, "noRepeat", list.noRepeat);
    for (const pugi::xml_node line : node.children(element)) {
        VoiceLine v;
        v.fmodName = text(line, "fmodName");
        v.group = integer(line, "group", 0);
        list.lines.push_back(std::move(v));
    }
    return list;
}

std::vector<VoiceLine> specialLines(const pugi::xml_node& node, const char* element)
{
    std::vector<VoiceLine> lines;
    for (const pugi::xml_node line : node.children(element)) {
        VoiceLine v;
        v.fmodName = text(line, "fmodName");
        v.gameplayEvent = text(line, "gameplayEvent");
        lines.push_back(std::move(v));
    }
    return lines;
}

Station station(const pugi::xml_node& node)
{
    Station s;
    s.name = text(node, "name");
    s.isOffStation = flag(node, "isOffStation", false);
    s.reverbWetLevel = number(node, "reverbWetLevel", s.reverbWetLevel);
    s.volume = number(node, "volume", s.volume);
    s.panLevel3d = number(node, "panLevel3d", s.panLevel3d);
    s.musicLoops = flag(node, "musicLoops", false);
    for (const pugi::xml_node child : node.children()) {
        const std::string_view name = child.name();
        if (name == "Playlist") {
            s.playlist.noRepeatAttribute = integer(child, "noRepeat", s.playlist.noRepeatAttribute);
            for (const pugi::xml_node track : child.children("MusicTrack")) {
                MusicTrack t;
                t.name = text(track, "name");
                t.artist = text(track, "artist");
                t.fmodName = text(track, "fmodName");
                t.likelihood[static_cast<int>(TimeOfDay::Daytime)] = number(track, "likelihoodDaytime", 0.0F);
                t.likelihood[static_cast<int>(TimeOfDay::Morning)] = number(track, "likelihoodMorning", 0.0F);
                t.likelihood[static_cast<int>(TimeOfDay::Evening)] = number(track, "likelihoodEvening", 0.0F);
                t.likelihood[static_cast<int>(TimeOfDay::Night)] = number(track, "likelihoodNight", 0.0F);
                s.playlist.tracks.push_back(std::move(t));
            }
        } else if (name == "Idents") {
            s.idents = voiceList(child, "Ident");
        } else if (name == "DJBookendDialogueIn") {
            s.bookendIn = voiceList(child, "SimpleDialogue");
        } else if (name == "DJBookendDialogueOut") {
            s.bookendOut = voiceList(child, "SimpleDialogue");
        } else if (name == "DJRegularDialogue") {
            s.regular = voiceList(child, "SimpleDialogue");
        } else if (name == "DJFestivalUpdateLeadIn") {
            s.festivalLeadIn = voiceList(child, "SimpleDialogue");
        } else if (name == "DJFestivalUpdateLeadOut") {
            s.festivalLeadOut = voiceList(child, "SimpleDialogue");
        } else if (name == "DJSpecialDialogue") {
            s.special = specialLines(child, "SpecialDialogue");
        } else if (name == "DJImmediateDialogue") {
            s.immediate = specialLines(child, "SpecialDialogue");
        }
    }
    return s;
}

std::uint32_t milliseconds(const pugi::xml_node& node, std::string_view name, const std::string& source)
{
    const int value = integer(node, name, 0);
    if (value < 0) {
        throw DataError("{}: {} is negative ({})", source, name, value);
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

std::size_t RadioSystemData::offStation() const
{
    if (const std::optional<std::size_t> index = findStation(kOffStationName)) {
        return *index;
    }
    throw DataError("the radio has no station named {}", kOffStationName);
}

std::optional<std::size_t> RadioSystemData::findStation(std::string_view name) const
{
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (stations[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

RadioSystemData parseRadioSystem(std::string_view xml, const std::string& source)
{
    // The file names an attribute "3dPanLevel", which XML forbids (names
    // cannot start with a digit) but the game's own parser takes; it is
    // renamed before a conforming parser sees it.
    std::string fixed(xml);
    for (std::size_t at = fixed.find(" 3dPanLevel="); at != std::string::npos;
         at = fixed.find(" 3dPanLevel=", at + 1)) {
        fixed.replace(at, 12, " panLevel3d=");
    }
    const pugi::xml_document doc = load(fixed, source);
    const pugi::xml_node root = doc.child("RadioSystem");
    if (!root) {
        throw DataError("{}: no RadioSystem element", source);
    }
    RadioSystemData data;
    RadioSettings& s = data.settings;
    if (const pugi::xml_node system = root.child("System")) {
        s.fsbMusic = text(system, "fsbMusic");
        s.fsbVO = text(system, "fsbVO");
        s.maxTracksBetweenDJ = integer(system, "maxTracksBetweenDJ", s.maxTracksBetweenDJ);
        s.levelFadeUpTime = number(system, "levelFadeUpTime", s.levelFadeUpTime);
        s.levelFadeUpTimeForEvent = number(system, "levelFadeUpTimeForEvent", s.levelFadeUpTimeForEvent);
        s.levelFadeDownTime = number(system, "levelFadeDownTime", s.levelFadeDownTime);
        s.duckFadeUpTime = number(system, "duckFadeUpTime", s.duckFadeUpTime);
        s.duckFadeDownTime = number(system, "duckFadeDownTime", s.duckFadeDownTime);
        s.duckValue = number(system, "duckValue", s.duckValue);
        s.scaleFadeUpTime = number(system, "scaleFadeUpTime", s.scaleFadeUpTime);
        s.scaleFadeDownTime = number(system, "scaleFadeDownTime", s.scaleFadeDownTime);
        s.masterLevel = number(system, "masterLevel", s.masterLevel);
        s.musicLevel = number(system, "musicLevel", s.musicLevel);
        s.dialogueLevel = number(system, "dialogueLevel", s.dialogueLevel);
        s.identLevel = number(system, "identLevel", s.identLevel);
        s.hudNewTrackDisplayDelayFreeRoam = number(system, "HUDNewTrackDisplayDelayFreeRoam",
            s.hudNewTrackDisplayDelayFreeRoam);
        s.hudNewTrackDisplayDelayRace = number(system, "HUDNewTrackDisplayDelayRace", s.hudNewTrackDisplayDelayRace);
        s.satNavDuckLevel = number(system, "satNavDuckLevel", s.satNavDuckLevel);
        s.stationChangeFadeTimeNormal = number(system, "stationChangeFadeTimeNormal", s.stationChangeFadeTimeNormal);
        s.stationChangeFadeTimeUser = number(system, "stationChangeFadeTimeUser", s.stationChangeFadeTimeUser);
    }
    for (const pugi::xml_node node : root.children("RadioStation")) {
        data.stations.push_back(station(node));
    }
    std::stable_sort(data.stations.begin(), data.stations.end(), [](const Station& a, const Station& b) {
        return a.name < b.name;
    });
    // The station map keeps the first of two stations with one name.
    data.stations.erase(std::unique(data.stations.begin(), data.stations.end(),
                            [](const Station& a, const Station& b) { return a.name == b.name; }),
        data.stations.end());
    if (const pugi::xml_node updates = root.child("FestivalUpdates")) {
        data.festivalUpdates = specialLines(updates, "FestivalUpdate");
    }
    return data;
}

SoundBankInfoMap parseSoundBankInfo(std::string_view xml, const std::string& source)
{
    const pugi::xml_document doc = load(xml, source);
    const pugi::xml_node root = doc.child("SoundBanks");
    if (!root) {
        throw DataError("{}: no SoundBanks element", source);
    }
    SoundBankInfoMap banks;
    for (const pugi::xml_node bank : root.children("Bank")) {
        const pugi::xml_node node = bank.child("SoundBankInfo");
        SoundBankInfo info;
        info.soundBankIndex = integer(node, "SoundBankIndex", -1);
        info.offsetEventStart = milliseconds(node, "OffsetEventStart", source);
        info.offsetIdentStart = milliseconds(node, "OffsetIdentStart", source);
        info.offsetSongStart = milliseconds(node, "OffsetSongStart", source);
        info.offsetStartNextTrack = milliseconds(node, "OffsetStartNextTrack", source);
        info.fileLength = milliseconds(node, "FileLength", source);
        info.syncpointIndexIdentStart = integer(node, "SyncpointIndexIdentStart", -1);
        info.syncpointStartNextTrack = integer(node, "SyncpointStartNextTrack", -1);
        banks.emplace(text(bank, "Name"), info);
    }
    return banks;
}

std::uint16_t stringIdKey(std::string_view id)
{
    constexpr unsigned kBits = 16;
    constexpr std::uint32_t kMask = (1U << kBits) - 1U;
    std::uint32_t key = kMask;
    for (const char raw : id) {
        const char c = (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
        // The game sign-extends the character before the XOR.
        key ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(c)));
        key = ((key >> (kBits - 7)) | (key << 7)) & kMask;
    }
    return static_cast<std::uint16_t>(key);
}

} // namespace openhorizon::radio
