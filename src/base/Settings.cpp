#include "Settings.h"

#include "Bytes.h"
#include "Log.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>

namespace fh {

namespace {

std::string trim(const std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

std::filesystem::path Settings::defaultPath()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "fh1-native" / "settings.ini";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "fh1-native" / "settings.ini";
    }
    return std::filesystem::path("fh1-native-settings.ini");
}

Settings Settings::load()
{
    Settings settings;
    settings.m_path = defaultPath();
    std::ifstream in(settings.m_path);
    std::string line;
    while (std::getline(in, line)) {
        const std::string content = trim(line.substr(0, line.find('#')));
        const std::size_t equals = content.find('=');
        if (content.empty() || equals == std::string::npos) {
            continue;
        }
        settings.m_values[trim(content.substr(0, equals))] = trim(content.substr(equals + 1));
    }
    return settings;
}

std::optional<std::string> Settings::get(const std::string& key) const
{
    const auto it = m_values.find(key);
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string Settings::get(const std::string& key, const std::string& fallback) const
{
    return get(key).value_or(fallback);
}

float Settings::getFloat(const std::string& key, float fallback) const
{
    const std::optional<std::string> value = get(key);
    if (!value) {
        return fallback;
    }
    char* end = nullptr;
    const float parsed = std::strtof(value->c_str(), &end);
    return end != value->c_str() ? parsed : fallback;
}

bool Settings::getBool(const std::string& key, bool fallback) const
{
    const std::optional<std::string> value = get(key);
    if (!value) {
        return fallback;
    }
    const std::string lower = toLower(*value);
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        return false;
    }
    return fallback;
}

bool Settings::save() const
{
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    std::ofstream out(m_path);
    if (!out) {
        logWarning("settings: cannot write {}", m_path.string());
        return false;
    }
    // Sorted, so the file stays stable between saves.
    const std::map<std::string, std::string> sorted(m_values.begin(), m_values.end());
    for (const auto& [key, value] : sorted) {
        out << key << " = " << value << '\n';
    }
    return static_cast<bool>(out);
}

} // namespace fh
