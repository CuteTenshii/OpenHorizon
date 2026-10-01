#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::radio {

/// From media/stringtables/<language>/. Verified: IDS_RadioStation_Option_1..3, _Option_Off (myprofile.str) and
/// IDS_RadioOff (ingame.str). Inferred: option N is the Nth station in name order, as the playlists agree.
class RadioText {
public:
    /// `language` is a string table folder ("en"). Missing tables leave the texts empty.
    RadioText(const Vfs& vfs, std::string_view language, std::size_t tunableStations, std::size_t offStation);

    /// Empty when the text is missing or the index is not tuneable.
    const std::string& stationName(std::size_t station) const;
    /// For the HUD.
    const std::string& radioOff() const { return m_radioOff; }

private:
    std::vector<std::string> m_names;
    std::string m_radioOff;
    std::string m_empty;
};

} // namespace openhorizon::radio
