#include "RadioText.h"

#include "RadioData.h"

#include "base/Log.h"
#include "data/StringTable.h"
#include "vfs/Vfs.h"

#include <format>

namespace openhorizon::radio {

namespace {

std::optional<StringTable> loadTable(const Vfs& vfs, std::string_view language, std::string_view name)
{
    const std::string path = std::format("media/stringtables/{}/{}.str", language, name);
    try {
        if (const std::optional<Bytes> data = vfs.read(path)) {
            return StringTable::parse(*data, path);
        }
        logWarning("radio: {} not found", path);
    } catch (const std::exception& e) {
        logWarning("radio: {}", e.what());
    }
    return std::nullopt;
}

std::string lookup(const std::optional<StringTable>& table, std::string_view id)
{
    return table ? table->get(stringIdKey(id)).value_or(std::string{}) : std::string{};
}

} // namespace

RadioText::RadioText(const Vfs& vfs, std::string_view language, std::size_t tunableStations, std::size_t offStation)
{
    const std::optional<StringTable> profile = loadTable(vfs, language, "myprofile");
    const std::optional<StringTable> inGame = loadTable(vfs, language, "ingame");
    m_names.resize(tunableStations);
    for (std::size_t i = 0; i < tunableStations; ++i) {
        const std::string id = i == offStation ? std::string("IDS_RadioStation_Option_Off")
                                               : std::format("IDS_RadioStation_Option_{}", i + 1);
        m_names[i] = lookup(profile, id);
    }
    m_radioOff = lookup(inGame, "IDS_RadioOff");
}

const std::string& RadioText::stationName(std::size_t station) const
{
    return station < m_names.size() ? m_names[station] : m_empty;
}

} // namespace openhorizon::radio
