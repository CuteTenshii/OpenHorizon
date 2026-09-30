#pragma once

#include "Profile.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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
/// encoding was not traced). It keeps the ForzaProfile file's content, the
/// bags as the schema lays them out and the game state, in a compact binary
/// file of its own, ForzaProfile.bin. Little-endian throughout; "varint" is
/// an unsigned LEB128 number (seven bits a byte, low bits first), "zigzag"
/// a signed one mapped onto it (0, -1, 1, -2 as 0, 1, 2, 3), "text" a
/// varint byte count and that many bytes of UTF-8, "name" a varint index
/// into the string table:
///
///     "FH1P"              magic
///     u16                 format version (kFormatVersion)
///     varint count, text  string table: every property and field name and
///                         state type name, once, in order of first use
///     property list       the bags
///     varint, varint      the game state tree's version, then the byte
///                         size of the rest of the tree, so that a tree of
///                         another version can be skipped
///     varint              count of state groups; each group:
///       name, zigzag      the states' type and version
///       varint, varint    state count, byte size of the states
///       per state         text name, then the body for the type:
///         CRoadDiscovery 2     varint node count, u32 words (bit i of
///                              word k for node 32k + i)
///         CSpeedCameraState 1  u8 flags (1 activity discovered,
///                              2 discovered), f32 personal best
///         CFlyerState 1        u8 flags (1 activity discovered,
///                              2 collected)
///         CGasStationState 1   u8 flags (1 discovered)
///         any other type       property list (the state's fields)
///     u32                 CRC-32 of every byte before it
///
/// A property list is a varint count and, per property, its name, a u8
/// PropertyType, and its value: Bool a u8 0 or 1, unsigned integers a
/// varint, signed ones zigzag, Float32 and Float64 their IEEE bits (4 and 8
/// bytes), both string types text, a bag a nested property list.
///
/// Measured with the disc's schema when this layout replaced format
/// version 1, an XML file: a new profile took 3429 bytes against 13751 as
/// XML, a played one (12036 road nodes, 22 speed traps, 100 discount signs,
/// 10 outposts) 6755 against 44581.
///
/// A known state type saved with another version is skipped through its
/// group's byte size; a file whose checksum does not match (truncated or
/// damaged) is not read at all.
///
/// That XML file, ForzaProfile.xml, is not read: a folder holding only it
/// starts a new profile, and the file is left where it is.
///
/// Only ForzaProfile is kept: PlayerDatabase (garage, event and race
/// results), VersionFlags and GameplayLog hold nothing this engine has yet.
class ProfileStore {
public:
    /// This engine's file format version; 1 was an XML file.
    static constexpr int kFormatVersion = 2;
    static constexpr std::string_view kFileName = "ForzaProfile.bin";
    static constexpr std::string_view kMagic = "FH1P";

    /// `profile` in the Data user folder: `~/.local/share/fh1-native/profile`
    /// on Linux, `%LOCALAPPDATA%\fh1-native\profile` on Windows (see
    /// fh::UserFolder); one profile per user of the machine.
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
        /// The file is not a profile (no magic), is truncated or
        /// damaged (its checksum does not match), or does not have the
        /// layout above; moved aside by the first save as well.
        Unreadable,
    };
    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        Profile profile;
    };

    /// Reads the profile over the schema's defaults. A property the schema
    /// has with another type, or a value that does not fit its type, keeps
    /// the default; one the schema does not have is kept as read. A state of
    /// a known type saved with another version is dropped, as the game's
    /// serializers fail for it; states of other types are kept.
    LoadResult load(const ProfileSchema& schema);

    /// Writes the profile to a temporary file in the same folder, flushes it
    /// to disk and renames it over the profile, so a crash leaves the old or
    /// the new profile, never a mix. A file that load() could not read is
    /// first renamed to "<file>.unreadable". Returns false, logging why, when
    /// it cannot write; the old profile is then untouched.
    bool save(const Profile& profile);

    /// The bytes save() writes.
    static std::string serialize(const Profile& profile);

private:
    LoadStatus loadBinary(const ProfileSchema& schema, Profile& profile) const;

    std::filesystem::path m_directory;
    bool m_setAside = false;
};

/// CRC-32 of `data` (the reflected polynomial 0xEDB88320 of zlib and PNG).
std::uint32_t crc32(std::string_view data);

} // namespace fh::profile
