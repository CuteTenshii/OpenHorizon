#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::radio {

/// Ordered as the track record's floats (verified at 0x82c94a78, picker 0x82c96000). The hour-to-slot mapping was
/// not traced; every track on the disc has 1 for all four.
enum class TimeOfDay : int { Daytime = 0, Morning = 1, Evening = 2, Night = 3 };

/// `System` in RadioSystem.xml. Defaults are the game's (verified at 0x82c9cb20, 0x82c9b2b8). Times in seconds;
/// a fade time of zero is instant.
struct RadioSettings {
    std::string fsbMusic = "Radio_Music";
    std::string fsbVO = "Radio_VO";
    /// Music tracks that start between two regular DJ lines.
    int maxTracksBetweenDJ = 2;
    /// When free roam starts the radio.
    float levelFadeUpTime = 2.0F;
    /// When a race starts the radio.
    float levelFadeUpTimeForEvent = 2.0F;
    float levelFadeDownTime = 3.0F;
    /// Music ducking under DJ speech.
    float duckFadeUpTime = 1.0F;
    float duckFadeDownTime = 0.5F;
    float duckValue = 0.7F;
    /// The whole radio's fade to and from the off station (0x82c94f30).
    float scaleFadeUpTime = 0.3F;
    float scaleFadeDownTime = 0.3F;
    float masterLevel = 1.0F;
    float musicLevel = 1.0F;
    float dialogueLevel = 1.0F;
    float identLevel = 1.0F;
    /// How long the HUD waits before showing a new track.
    float hudNewTrackDisplayDelayFreeRoam = 0.0F;
    float hudNewTrackDisplayDelayRace = 0.0F;
    float satNavDuckLevel = 1.0F;
    /// The "user" time needs a flag (radio system +356) nothing traced sets, so the normal time applies.
    float stationChangeFadeTimeNormal = 0.5F;
    float stationChangeFadeTimeUser = 0.25F;
};

/// `name` and `artist` are English only in the file, with no string table reference.
struct MusicTrack {
    std::string name;
    std::string artist;
    /// Through RadioSoundbankInfo_Music.xml.
    std::string fmodName;
    /// Indexed by TimeOfDay; a missing attribute is 0 (0x82c958d8).
    std::array<float, 4> likelihood{};
};

/// The game discards `noRepeat` and keeps a track from repeating until every other has played (verified at
/// 0x82c9ad50); `noRepeatAttribute` is for tools only.
struct Playlist {
    int noRepeatAttribute = 2;
    /// In file order. A repeated fmodName is ignored for picking but its likelihoods still count.
    std::vector<MusicTrack> tracks;
};

/// An `Ident`, `SimpleDialogue` or `SpecialDialogue` entry.
struct VoiceLine {
    std::string fmodName;
    /// Group 0 always plays; the game switches others off (hub hints once the hub is found).
    int group = 0;
    std::string gameplayEvent;
};

/// Random picks without repeating the last `noRepeat`.
struct VoiceList {
    /// The game's default (0x82c9af50).
    int noRepeat = 2;
    std::vector<VoiceLine> lines;
};

/// Defaults are the station constructor's (0x82c9af50).
struct Station {
    std::string name;
    bool isOffStation = false;
    /// For the positional stations heard in menus and at the festival.
    float reverbWetLevel = 0.0F;
    float volume = 1.0F;
    float panLevel3d = 1.0F;
    bool musicLoops = false;
    Playlist playlist;
    VoiceList idents;
    VoiceList bookendIn;
    VoiceList bookendOut;
    VoiceList regular;
    VoiceList festivalLeadIn;
    VoiceList festivalLeadOut;
    std::vector<VoiceLine> special;
    std::vector<VoiceLine> immediate;
};

/// Media/Audio/Radio/RadioSystem.xml. Stations sort by name, as the game's map (verified at 0x82c96f20); tuneable
/// ones run up to the off station, positional ones follow (0x82c9d5d4). The off station is found by name
/// (0x82c9d598), not by isOffStation.
struct RadioSystemData {
    RadioSettings settings;
    std::vector<Station> stations;
    std::vector<VoiceLine> festivalUpdates;

    static constexpr std::string_view kOffStationName = "Radio4_Silent";

    /// Throws DataError if there is none.
    std::size_t offStation() const;
    std::optional<std::size_t> findStation(std::string_view name) const;
};

/// Attribute names match without case: the file spells both `fmodname` and `fmodName` (inferred from three
/// stations otherwise having no tracks). Throws DataError if the document is not a RadioSystem.
RadioSystemData parseRadioSystem(std::string_view xml, const std::string& source);

/// One `Bank` of RadioSoundbankInfo_*.xml, generated from the banks' sync points (0x82c9b910). Offsets and
/// FileLength are in whole milliseconds (verified against the FSB sync points).
struct SoundBankInfo {
    int soundBankIndex = -1;
    /// Music: where the intro gives way (a DJ line over it must end here); where a race starts the music.
    std::uint32_t offsetEventStart = 0;
    /// Music: where the station ident starts over the track's end.
    std::uint32_t offsetIdentStart = 0;
    /// Music: where a track starts when it follows another.
    std::uint32_t offsetSongStart = 0;
    /// Idents: where the next track starts.
    std::uint32_t offsetStartNextTrack = 0;
    std::uint32_t fileLength = 0;
    /// -1 if none.
    int syncpointIndexIdentStart = -1;
    /// -1 if none.
    int syncpointStartNextTrack = -1;
};

using SoundBankInfoMap = std::map<std::string, SoundBankInfo, std::less<>>;

/// Throws DataError if it is not a SoundBanks document.
SoundBankInfoMap parseSoundBankInfo(std::string_view xml, const std::string& source);

/// The 16-bit key string tables file a text ID under: from 0xFFFF, XOR each lower-cased character, rotate left 7
/// (verified: hash 0x82dbe378 via resolver 0x82d69028; IDS_RadioOff gives 19192 in ingame.str).
std::uint16_t stringIdKey(std::string_view id);

} // namespace openhorizon::radio
