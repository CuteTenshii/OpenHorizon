#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace openhorizon {

/// The player's settings, from `settings.ini` in the Config user folder
/// (`~/.config/fh1-native` on Linux, `%APPDATA%\fh1-native` on Windows; see
/// UserFolder): `key = value` lines of UTF-8, `#` comments. Command-line
/// options override them.
class Settings {
public:
    /// Reads the settings file if it exists; a missing file gives defaults.
    static Settings load();
    static std::filesystem::path defaultPath();

    std::optional<std::string> get(const std::string& key) const;
    std::string get(const std::string& key, const std::string& fallback) const;
    float getFloat(const std::string& key, float fallback) const;
    bool getBool(const std::string& key, bool fallback) const;
    void set(const std::string& key, const std::string& value) { m_values[key] = value; }

    /// Writes all values back to the settings file, creating its folder.
    bool save() const;

private:
    std::filesystem::path m_path;
    std::unordered_map<std::string, std::string> m_values;
};

} // namespace openhorizon
