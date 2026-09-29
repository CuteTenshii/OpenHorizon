#pragma once

#include "Profile.h"

#include <filesystem>
#include <string>

namespace fh::profile {

/// Where a player's profile is kept, and how it is read and written.
///
/// The console keeps a profile in a save container named "ForzaProfile"
/// holding four files, with their size limits in a table at 0x83305074
/// (read by the container setup at 0x8256597c): ForzaProfile (0xC800 bytes:
/// the property bags and the game state trees in one binary stream, see
/// GameState), PlayerDatabase (0x600000), VersionFlags (0x80) and
/// GameplayLog (0x100000). That PlayerDatabase holds the gamedb tables named
/// NewProfile_* (garage, purchased parts, event and race results), which the
/// game's queries read under a profile prefix ("%sCareer_EventResults",
/// prefix string at 0x820187a0), is inferred from its name and size.
///
/// This engine never reads or writes a console save, so it does not
/// reproduce that container or the stream's binary layout (the bags' own
/// encoding was not traced). It keeps the ForzaProfile file's content, laid
/// out as the schema lays out the bags, in one XML file:
///
///     <ForzaProfile version="1">
///       <Profile>  the bags, as ForzaProfile.sch writes them, with the
///                  player's values
///       <GameState version="2">
///         <State name type version>  one per state, fields as
///                                   <Property> elements; RoadDiscovery's
///                                   words as hexadecimal text
///
/// `version` on the root is this file format's version; a file with another
/// version is not read. Only ForzaProfile is kept: PlayerDatabase (garage,
/// event and race results), VersionFlags and GameplayLog hold nothing this
/// engine has yet.
class ProfileStore {
public:
    /// This engine's file format version.
    static constexpr int kFormatVersion = 1;
    static constexpr std::string_view kFileName = "ForzaProfile.xml";

    /// `$XDG_DATA_HOME/fh1-native/profile`, or
    /// `~/.local/share/fh1-native/profile` when XDG_DATA_HOME is not set:
    /// one profile per user of the machine.
    static std::filesystem::path defaultDirectory();

    explicit ProfileStore(std::filesystem::path directory);

    const std::filesystem::path& directory() const { return m_directory; }
    std::filesystem::path file() const { return m_directory / kFileName; }

    enum class LoadStatus {
        /// Read from the file.
        Loaded,
        /// There is no file yet: a new profile.
        Missing,
        /// The file has another format version; left as it is until the
        /// first save moves it aside.
        VersionMismatch,
        /// The file is not a profile (not XML, no root); moved aside by the
        /// first save as well.
        Unreadable,
    };
    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        Profile profile;
    };

    /// Reads the profile over the schema's defaults. A property the schema
    /// has with another type, or a value that does not parse, keeps the
    /// default; one the schema does not have is kept as read. A state of a
    /// known type saved with another version is dropped, as the game's
    /// serializers fail for it; states of other types are kept.
    LoadResult load(const ProfileSchema& schema);

    /// Writes the profile to a temporary file in the same folder, flushes it
    /// to disk and renames it over the profile, so a crash leaves the old or
    /// the new profile, never a mix. A file that load() could not read is
    /// first renamed to "<file>.unreadable". Returns false, logging why,
    /// when it cannot write; the old profile is then untouched.
    bool save(const Profile& profile);

    /// The XML save() writes.
    static std::string serialize(const Profile& profile);

private:
    std::filesystem::path m_directory;
    bool m_setAside = false;
};

} // namespace fh::profile
