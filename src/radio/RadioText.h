#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::radio {

/// The radio's localised text, from media/stringtables/<language>/.
///
/// Verified: the executable names the texts IDS_RadioStation_Option_1,
/// _Option_2, _Option_3 and _Option_Off (the radio station setting) and
/// IDS_RadioOff (the in-game HUD), and their keys (stringIdKey) find
/// "HORIZON BASS ARENA", "HORIZON PULSE", "HORIZON ROCKS" and "OFF" in
/// myprofile.str and "RADIO OFF" in ingame.str. Inferred: that option N is
/// the Nth station in name order (Radio1, Radio2, Radio3), which the
/// stations' music agrees with (the loading tips describe Bass Arena as the
/// electronic station, Pulse as indie and Rocks as rock, matching Radio1,
/// Radio2 and Radio3's playlists). The track names and artists are not
/// localised; they come from RadioSystem.xml.
class RadioText {
public:
    /// `language` is a string table folder ("en", "de", ...). Missing tables
    /// leave the texts empty rather than failing.
    RadioText(const Vfs& vfs, std::string_view language, std::size_t tunableStations, std::size_t offStation);

    /// The display name of a tuneable station (0..offStation), empty when
    /// the text is missing or the index is not tuneable.
    const std::string& stationName(std::size_t station) const;
    /// "RADIO OFF", for the HUD.
    const std::string& radioOff() const { return m_radioOff; }

private:
    std::vector<std::string> m_names;
    std::string m_radioOff;
    std::string m_empty;
};

} // namespace fh::radio
