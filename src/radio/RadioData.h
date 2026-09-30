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

/// The time-of-day slots a music track has a likelihood for. The numbering
/// is the order of the four floats in the game's track record (verified in
/// the MusicTrack parser at 0x82c94a78: Daytime at +88, Morning +92, Evening
/// +96, Night +100) and the index the track picker takes (0x82c96000). Which
/// game hour maps to which slot is set by the game and was not traced; every
/// track on the disc has 1 for all four, so the choice makes no difference to
/// the data as shipped.
enum class TimeOfDay : int { Daytime = 0, Morning = 1, Evening = 2, Night = 3 };

/// `System` in RadioSystem.xml. Defaults are the values the game's parser
/// starts from before reading the attributes (verified at 0x82c9cb20 and in
/// the radio system constructor at 0x82c9b2b8). Times are in seconds, as in
/// the file; the game turns each fade time into a rate of 1/time per second,
/// with zero meaning instant.
struct RadioSettings {
    std::string fsbMusic = "Radio_Music";
    std::string fsbVO = "Radio_VO";
    /// Music tracks that start between two regular DJ lines.
    int maxTracksBetweenDJ = 2;
    /// The radio level's fade in when free roam starts the radio.
    float levelFadeUpTime = 2.0F;
    /// The same fade when a race starts the radio.
    float levelFadeUpTimeForEvent = 2.0F;
    float levelFadeDownTime = 3.0F;
    /// Music ducking under DJ speech: the gain falls to duckValue over
    /// duckFadeDownTime and comes back over duckFadeUpTime.
    float duckFadeUpTime = 1.0F;
    float duckFadeDownTime = 0.5F;
    float duckValue = 0.7F;
    /// The fade of the whole radio to silence when the off station is chosen
    /// and back when a station is (vfunc at vtable +128, 0x82c94f30).
    float scaleFadeUpTime = 0.3F;
    float scaleFadeDownTime = 0.3F;
    float masterLevel = 1.0F;
    float musicLevel = 1.0F;
    float dialogueLevel = 1.0F;
    float identLevel = 1.0F;
    /// How long the HUD waits before showing a new track, in free roam and in
    /// races.
    float hudNewTrackDisplayDelayFreeRoam = 0.0F;
    float hudNewTrackDisplayDelayRace = 0.0F;
    float satNavDuckLevel = 1.0F;
    /// Cross-fade between stations when a station change asks for a fade.
    /// The game picks the "user" time only when a flag is set that the code
    /// traced never sets (radio system +356), so the normal time applies.
    float stationChangeFadeTimeNormal = 0.5F;
    float stationChangeFadeTimeUser = 0.25F;
};

/// One `MusicTrack`. `name` and `artist` are the text the HUD shows; the file
/// has them in English only (there is no string table reference).
struct MusicTrack {
    std::string name;
    std::string artist;
    /// The sample in the music bank, through RadioSoundbankInfo_Music.xml.
    std::string fmodName;
    /// Indexed by TimeOfDay; a missing attribute is 0, the track record's
    /// initial value (0x82c958d8).
    std::array<float, 4> likelihood{};
};

/// A station's `Playlist`. The file's `noRepeat` is read and then discarded
/// by the game, which counts the tracks instead and keeps a track from
/// repeating until every other track has played (verified at 0x82c9ad50:
/// the attribute is stored, overwritten with 0, incremented per track and
/// decremented once at the end). `noRepeatAttribute` keeps the file's value
/// for tools only.
struct Playlist {
    int noRepeatAttribute = 2;
    /// In file order. The game files them in a map keyed by fmodName, so
    /// a second track with the same fmodName is ignored for picking, though
    /// its likelihoods still count towards the totals.
    std::vector<MusicTrack> tracks;
};

/// An `Ident`, `SimpleDialogue` or `SpecialDialogue` entry.
struct VoiceLine {
    std::string fmodName;
    /// `group` of a regular DJ line. Group 0 always plays; the game switches
    /// other groups off (street race hub hints once the hub is found).
    int group = 0;
    /// `gameplayEvent` of a special or immediate line.
    std::string gameplayEvent;
};

/// A list the game picks from at random without repeating the last
/// `noRepeat` picks (Idents and the DJ lists).
struct VoiceList {
    /// The game's default when the attribute is missing (station constructor
    /// at 0x82c9af50).
    int noRepeat = 2;
    std::vector<VoiceLine> lines;
};

/// One `RadioStation`. The attribute defaults are the station constructor's
/// (0x82c9af50).
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

/// Media/Audio/Radio/RadioSystem.xml.
///
/// Stations are sorted by name, the order of the game's station map (a
/// std::map keyed by the name, verified: station changes step through it by
/// index at 0x82c96f20). The stations a player can tune through run from the
/// first to the off station; the ones after it are the positional stations
/// the game starts itself (the station after the off station is the first
/// "3D" one, 0x82c9d5d4). The game finds the off station by its name,
/// "Radio4_Silent" (0x82c9d598), not by the isOffStation attribute, which
/// only its "is the radio playing" query reads.
struct RadioSystemData {
    RadioSettings settings;
    std::vector<Station> stations;
    std::vector<VoiceLine> festivalUpdates;

    static constexpr std::string_view kOffStationName = "Radio4_Silent";

    /// The index of the station named kOffStationName. Throws DataError if
    /// there is none.
    std::size_t offStation() const;
    std::optional<std::size_t> findStation(std::string_view name) const;
};

/// Parses RadioSystem.xml. Attribute names are matched without case: the
/// file spells the track sample attribute both `fmodname` and `fmodName`,
/// and the game plays both kinds of track (inferred: with case-sensitive
/// names three stations would have no playable tracks). Throws DataError
/// naming `source` if the document is not a RadioSystem.
RadioSystemData parseRadioSystem(std::string_view xml, const std::string& source);

/// One `Bank` of RadioSoundbankInfo_Music.xml or _VO_<lang>.xml, which the
/// game reads at run time to find a named sound's sample and its cue points.
/// The game generates the file from the banks' sync points
/// ("generateradiosoundbankinfo", 0x82c9b910): every offset is a sync point
/// position read through FMOD in milliseconds (FMOD_TIMEUNIT_MS), and
/// FileLength is the sample's length in milliseconds (verified: the values
/// match the FSB sync points, truncated to whole milliseconds).
struct SoundBankInfo {
    /// The sample's index in the bank.
    int soundBankIndex = -1;
    /// Music: where the song's intro gives way (a DJ line over the intro
    /// must end here), and where a race wants the music to be at the start.
    std::uint32_t offsetEventStart = 0;
    /// Music: where the station ident starts over the track's end.
    std::uint32_t offsetIdentStart = 0;
    /// Music: where a track starts when it follows another.
    std::uint32_t offsetSongStart = 0;
    /// Idents: where the next track starts.
    std::uint32_t offsetStartNextTrack = 0;
    std::uint32_t fileLength = 0;
    /// Index of the IdentStart sync point in the sample, -1 if none.
    int syncpointIndexIdentStart = -1;
    /// Index of the StartNextTrack sync point in the sample, -1 if none.
    int syncpointStartNextTrack = -1;
};

using SoundBankInfoMap = std::map<std::string, SoundBankInfo, std::less<>>;

/// Parses a RadioSoundbankInfo_*.xml. Throws DataError naming `source` if it
/// is not a SoundBanks document.
SoundBankInfoMap parseSoundBankInfo(std::string_view xml, const std::string& source);

/// The 16-bit key the game's string tables file a text ID such as
/// "IDS_RadioOff" under: starting from 0xFFFF, each character (lower-cased)
/// is XORed in and the value is rotated left by 7 bits (verified: the hash
/// at 0x82dbe378, which the ID resolver 0x82d69028 calls with 16 bits and
/// case folding; IDS_RadioOff gives 19192, the key of "RADIO OFF" in
/// ingame.str).
std::uint16_t stringIdKey(std::string_view id);

} // namespace openhorizon::radio
