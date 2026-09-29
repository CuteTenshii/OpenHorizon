#include "Flyers.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <format>
#include <utility>

namespace fh {

Flyers Flyers::load(const Vfs& vfs, std::string_view mode)
{
    const std::string path = std::format("media/gamemodes/{}/Flyers.xml", mode);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} not found", path);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", path, parsed.description());
    }
    std::vector<std::string> ids;
    for (const pugi::xml_node node : doc.child("ActivityManager").children("Activity")) {
        if (iequals(node.attribute("type").as_string(), "ActivityFlyers")) {
            ids.emplace_back(node.attribute("name").as_string());
        }
    }
    return fromIds(std::move(ids));
}

Flyers Flyers::fromIds(std::vector<std::string> ids)
{
    Flyers flyers;
    flyers.m_ids = std::move(ids);
    flyers.m_collected.assign(flyers.m_ids.size(), false);
    return flyers;
}

std::optional<std::size_t> Flyers::indexOf(std::string_view gameplayId) const
{
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        if (iequals(m_ids[i], gameplayId)) {
            return i;
        }
    }
    return std::nullopt;
}

bool Flyers::isCollected(std::string_view gameplayId) const
{
    const std::optional<std::size_t> index = indexOf(gameplayId);
    return index && m_collected[*index];
}

bool Flyers::collect(std::string_view gameplayId)
{
    const std::optional<std::size_t> index = indexOf(gameplayId);
    if (!index || m_collected[*index]) {
        return false;
    }
    m_collected[*index] = true;
    ++m_collectedCount;
    m_last = index;
    return true;
}

std::optional<std::string> Flyers::lastCollected() const
{
    if (!m_last) {
        return std::nullopt;
    }
    return m_ids[*m_last];
}

std::vector<std::string> Flyers::collectedIds() const
{
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        if (m_collected[i]) {
            ids.push_back(m_ids[i]);
        }
    }
    return ids;
}

void Flyers::restore(const std::vector<std::string>& ids)
{
    m_collected.assign(m_ids.size(), false);
    m_collectedCount = 0;
    m_last.reset();
    for (const std::string& id : ids) {
        const std::optional<std::size_t> index = indexOf(id);
        if (index && !m_collected[*index]) {
            m_collected[*index] = true;
            ++m_collectedCount;
        }
    }
}

} // namespace fh
