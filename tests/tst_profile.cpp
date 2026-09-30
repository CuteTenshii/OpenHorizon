// The player's profile: the schema reader (ForzaProfile.sch) on synthetic
// XML and on the disc's schema when OPENHORIZON_GAME_DIR names it, and the profile
// store's file (round trip, atomic replacement, format version, damaged
// files). Every file
// goes to a folder under the temporary directory; XDG_DATA_HOME points
// there too, so the player's own profile is never touched.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "base/Host.h"
#include "profile/Profile.h"
#include "profile/ProfileSchema.h"
#include "profile/ProfileStore.h"
#include "vfs/Vfs.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using openhorizon::test::gameDir;

namespace {

using openhorizon::profile::FlyerState;
using openhorizon::profile::GasStationState;
using openhorizon::profile::OtherState;
using openhorizon::profile::Profile;
using openhorizon::profile::ProfileSchema;
using openhorizon::profile::ProfileStore;
using openhorizon::profile::PropertyType;
using openhorizon::profile::PropertyValue;
using openhorizon::profile::RoadDiscoveryState;
using openhorizon::profile::SpeedCameraState;
using namespace std::string_literals;

/// A fresh, empty folder for one test.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openhorizon-tst-profile" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

/// A schema shaped like the game's, with one property of every type.
constexpr std::string_view kSchema = R"(<Profile>
  <Property name="Main" type="PropertyBag">
    <Property name="IsVip" type="Bool" value="0"/>
    <Property name="Credits" type="UInt32" value="500"/>
    <Property name="MetaDiff" type="UInt8" value="1"/>
    <Property name="Short" type="UInt16" value="65535"/>
    <Property name="Flags" type="UInt64" value="18446744073709551615"/>
    <Property name="ShowroomCar" type="Int32" value="1131"/>
    <Property name="Small" type="Int8" value="-128"/>
    <Property name="Medium" type="Int16" value="-2"/>
    <Property name="Big" type="Int64" value="-9223372036854775808"/>
    <!-- A comment, as the schema has many -->
    <Property name="Gamma" type="Float32" value="0.6"/>
    <Property name="Precise" type="Float64" value="0.1"/>
    <Property name="Name" type="String" value="Player"/>
    <Property name="Narrow" type="StringNarrow" value=""/>
  </Property>
  <Property name="Options" type="PropertyBag">
    <Property name="Units" type="Int32" value="0"/>
  </Property>
  <Property name ="HeadTracking" type="PropertyBag"/>
</Profile>)";

ProfileSchema schema()
{
    return ProfileSchema::parse(kSchema);
}

TEST_CASE("typeNamesAreTheGames", "[profile]")
{
    // The table at 0x82154ee8: names, aliases and ids.
    CHECK(openhorizon::profile::propertyTypeFromName("Bool") == PropertyType::Bool);
    CHECK(openhorizon::profile::propertyTypeFromName("Byte") == PropertyType::UInt8);
    CHECK(openhorizon::profile::propertyTypeFromName("UInt") == PropertyType::UInt32);
    CHECK(openhorizon::profile::propertyTypeFromName("Int") == PropertyType::Int32);
    CHECK(openhorizon::profile::propertyTypeFromName("Float") == PropertyType::Float32);
    CHECK(openhorizon::profile::propertyTypeFromName("String") == PropertyType::StringWide);
    CHECK(openhorizon::profile::propertyTypeFromName("PropertyBag") == PropertyType::PropertyBag);
    CHECK(!openhorizon::profile::propertyTypeFromName("bool").has_value());
    CHECK(!openhorizon::profile::propertyTypeFromName("Double").has_value());
    CHECK(static_cast<int>(PropertyType::UInt64) == 4);
    CHECK(static_cast<int>(PropertyType::Float64) == 10);
    CHECK(static_cast<int>(PropertyType::PropertyBag) == 15);
    CHECK(openhorizon::profile::propertyTypeName(PropertyType::UInt8) == std::string_view("UInt8"));
}

TEST_CASE("schemaReadsBagsTypesAndDefaults", "[profile]")
{
    const ProfileSchema s = schema();
    CHECK(s.bags().size() == std::size_t{3});
    CHECK(s.valueCount() == std::size_t{14});
    const openhorizon::profile::Property* credits = s.find("Main/Credits");
    CHECK(credits != nullptr);
    CHECK((credits && credits->type == PropertyType::UInt32));
    CHECK((credits && credits->value == PropertyValue(std::uint64_t{500})));
    CHECK((s.find("Main/IsVip") && s.find("Main/IsVip")->value == PropertyValue(false)));
    CHECK(s.find("Main/Flags")->value == PropertyValue(std::numeric_limits<std::uint64_t>::max()));
    CHECK(s.find("Main/Small")->value == PropertyValue(std::int64_t{-128}));
    CHECK(s.find("Main/Big")->value == PropertyValue(std::numeric_limits<std::int64_t>::min()));
    CHECK(s.find("Main/Gamma")->value == PropertyValue(0.6F));
    CHECK(s.find("Main/Precise")->value == PropertyValue(0.1));
    CHECK(s.find("Main/Name")->type == PropertyType::StringWide);
    CHECK(s.find("Main/Name")->value == PropertyValue(std::string("Player")));
    CHECK(s.find("Options/Units") != nullptr);
    CHECK((s.find("HeadTracking") && s.find("HeadTracking")->children.empty()));
    CHECK((s.find("Main") && s.find("Main")->type == PropertyType::PropertyBag));
    CHECK(s.find("Main/Missing") == nullptr);
    CHECK(s.find("Options/Units/Deeper") == nullptr);
    CHECK(s.find("Nothing") == nullptr);
    // File order is kept.
    CHECK(s.bags()[0].children[0].name == std::string("IsVip"));
    CHECK(s.bags()[2].name == std::string("HeadTracking"));
}

TEST_CASE("schemaRejectsMalformedProperties", "[profile]")
{
    const auto bag = [](std::string_view inner) {
        return std::string("<Profile><Property name=\"Main\" type=\"PropertyBag\">") + std::string(inner)
            + "</Property></Profile>";
    };
    CHECK_THROWS_AS(ProfileSchema::parse("not xml <"), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse("<Other/>"), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property type="Int32" value="0"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int33" value="0"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int32"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int32" value="x"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Bool" value="2"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="UInt8" value="256"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="UInt32" value="-1"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int8" value="128"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="Float32" value="1.0f"/>)")), std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(R"(<Property name="A" type="PropertyBag" value="1"/>)")), std::exception);
    CHECK_THROWS_AS(
        ProfileSchema::parse(
            bag(R"(<Property name="A" type="Int32" value="0"><Property name="B" type="Int32" value="0"/></Property>)")),
        std::exception);
    CHECK_THROWS_AS(ProfileSchema::parse(bag(
                        R"(<Property name="A" type="Int32" value="0"/><Property name="A" type="Int32" value="1"/>)")),
        std::exception);
    CHECK_THROWS_AS(
        ProfileSchema::parse(R"(<Profile><Property name="A" type="Int32" value="0"/></Profile>)"), std::exception);
}

TEST_CASE("valuesReadBackExactly", "[profile]")
{
    for (const float number : {0.24F, 0.6F, 1.0F, 1e-7F, 123456.789F, -0.0F}) {
        const PropertyValue value = number;
        const auto parsed
            = openhorizon::profile::parseValue(PropertyType::Float32, openhorizon::profile::formatValue(value));
        CHECK((parsed && *parsed == value));
    }
    const PropertyValue big = std::numeric_limits<std::uint64_t>::max();
    CHECK(openhorizon::profile::parseValue(PropertyType::UInt64, openhorizon::profile::formatValue(big)) == big);
    CHECK(openhorizon::profile::formatValue(PropertyValue(true)) == std::string("1"));
    CHECK(!openhorizon::profile::valueFits(PropertyType::UInt8, PropertyValue(std::uint64_t{300})));
    CHECK(!openhorizon::profile::valueFits(PropertyType::Int32, PropertyValue(std::uint64_t{3})));
    CHECK(openhorizon::profile::valueFits(PropertyType::Int16, PropertyValue(std::int64_t{-32768})));
}

TEST_CASE("profileStartsFromTheDefaultsAndChecksTypes", "[profile]")
{
    const ProfileSchema s = schema();
    Profile profile(s);
    CHECK(profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    CHECK(profile.set("Main/Credits", std::uint64_t{12345}));
    CHECK(profile.value("Main/Credits") == PropertyValue(std::uint64_t{12345}));
    // The schema's copy is untouched.
    CHECK(s.find("Main/Credits")->value == PropertyValue(std::uint64_t{500}));
    CHECK(!profile.set("Main/Credits", std::int64_t{1}));
    CHECK(!profile.set("Main/MetaDiff", std::uint64_t{256}));
    CHECK(!profile.set("Main/Nothing", std::uint64_t{1}));
    CHECK(!profile.set("Main", std::uint64_t{1}));
    CHECK(!profile.value("Main").has_value());
    CHECK(profile.value("Main/MetaDiff") == PropertyValue(std::uint64_t{1}));
    CHECK(profile.state.speedCameras.empty());
    CHECK(!profile.state.roads.has_value());
}

/// A property list, compared field by field, bags included.
bool sameProperties(
    const std::vector<openhorizon::profile::Property>& a, const std::vector<openhorizon::profile::Property>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t index = 0; index < a.size(); ++index) {
        if (a[index].name != b[index].name || a[index].type != b[index].type || a[index].value != b[index].value
            || !sameProperties(a[index].children, b[index].children)) {
            return false;
        }
    }
    return true;
}

/// A state of a type this engine has no system for, with a field of every
/// kind and a nested bag.
OtherState playerState()
{
    OtherState state{"Player0", "CPlayerState", 1, {}};
    state.fields.push_back({"Raw", PropertyType::UInt32, std::uint64_t{7}, {}});
    state.fields.push_back({"Seen", PropertyType::Bool, true, {}});
    state.fields.push_back({"Offset", PropertyType::Int16, std::int64_t{-300}, {}});
    state.fields.push_back({"Scale", PropertyType::Float64, 2.5, {}});
    state.fields.push_back({"Label", PropertyType::StringNarrow, std::string("garage"), {}});
    state.fields.push_back(
        {"Inner", PropertyType::PropertyBag, {}, {{"Deep", PropertyType::UInt64, std::uint64_t{1} << 40, {}}}});
    return state;
}

Profile sampleProfile(const ProfileSchema& s)
{
    Profile profile(s);
    profile.set("Main/IsVip", true);
    profile.set("Main/Credits", std::uint64_t{4200});
    profile.set("Main/MetaDiff", std::uint64_t{255});
    profile.set("Main/Short", std::uint64_t{12});
    profile.set("Main/Flags", std::uint64_t{0x8000000000000001});
    profile.set("Main/ShowroomCar", std::int64_t{-1});
    profile.set("Main/Small", std::int64_t{127});
    profile.set("Main/Medium", std::int64_t{-32768});
    profile.set("Main/Big", std::int64_t{-5});
    profile.set("Main/Gamma", 0.35F);
    profile.set("Main/Precise", -1e300);
    profile.set("Main/Name", std::string("A <quoted> \"name\" & more, \xc3\xa9t\xc3\xa9"));
    profile.set("Main/Narrow", std::string("x"));
    profile.set("Options/Units", std::int64_t{2});
    RoadDiscoveryState roads;
    roads.visited.assign(70, false);
    for (const std::size_t node : {0U, 1U, 31U, 32U, 63U, 64U, 69U}) {
        roads.visited[node] = true;
    }
    profile.state.roads = roads;
    profile.state.speedCameras["speed_camera_01"] = SpeedCameraState{true, true, 36.123456F};
    profile.state.speedCameras["speed_camera_02"] = SpeedCameraState{false, false, 0.0F};
    profile.state.speedCameras["speed_camera_03"] = SpeedCameraState{true, false, 12.5F};
    profile.state.flyers["flyer_001"] = FlyerState{false, true};
    profile.state.flyers["flyer_002"] = FlyerState{true, false};
    profile.state.gasStations["GasStation_01"] = GasStationState{true};
    profile.state.gasStations["GasStation_02"] = GasStationState{false};
    profile.state.otherStates.push_back(playerState());
    profile.state.otherStates.push_back(OtherState{"Mission_07", "CMissionState", 3, {}});
    return profile;
}

/// A played profile over `s`: the counts of the disc's free roam (12036
/// road nodes, 22 speed traps, 100 discount signs, 10 outposts, as a
/// profile saved by this game holds them), most of it discovered, and
/// settings changed.
Profile playedProfile(const ProfileSchema& s)
{
    Profile profile(s);
    profile.set("Main/Credits", std::uint64_t{1234567});
    profile.set("Main/XP", std::int64_t{48250});
    profile.set("Main/WristbandLevel", std::int64_t{3});
    profile.set("Main/UpgradeDiscount", std::uint64_t{35});
    profile.set("Main/Gamma", 0.55F);
    profile.set("Options/Units", std::int64_t{1});
    profile.set("ControllerAdvancedOptions/SteeringAxisDeadzoneInside", 0.1F);
    RoadDiscoveryState roads;
    roads.visited.resize(12036);
    for (std::size_t node = 0; node < roads.visited.size(); ++node) {
        // Stretches driven and not, as roads are.
        roads.visited[node] = node % 97 < 61;
    }
    profile.state.roads = roads;
    for (int trap = 1; trap <= 22; ++trap) {
        profile.state.speedCameras[std::format("speed_camera_{:02}", trap)]
            = SpeedCameraState{false, trap <= 15, trap <= 15 ? 30.0F + static_cast<float>(trap) * 1.37F : 0.0F};
    }
    for (int sign = 1; sign <= 100; ++sign) {
        profile.state.flyers[std::format("flyer_{:03}", sign)] = FlyerState{false, sign % 3 != 0};
    }
    for (int outpost = 1; outpost <= 10; ++outpost) {
        profile.state.gasStations[std::format("GasStation_{:02}", outpost)] = GasStationState{outpost <= 7};
    }
    return profile;
}

/// The file's bytes with the checksum recomputed after an edit.
std::string resealed(std::string bytes)
{
    const std::uint32_t crc = openhorizon::profile::crc32(std::string_view(bytes).substr(0, bytes.size() - 4));
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[bytes.size() - 4 + index] = static_cast<char>(crc >> (8 * index));
    }
    return bytes;
}

/// A file of the binary layout around `body` (what follows the header).
std::string sealed(std::string_view body)
{
    std::string bytes("FH1P\x02\x00", 6);
    bytes += body;
    bytes.append(4, '\0');
    return resealed(bytes);
}

TEST_CASE("checksumIsCrc32", "[profile]")
{
    // The CRC-32 check value.
    CHECK(openhorizon::profile::crc32("123456789") == std::uint32_t{0xCBF43926});
    CHECK(openhorizon::profile::crc32("") == std::uint32_t{0});
}

TEST_CASE("binaryLayoutIsTheDocumentedOne", "[profile]")
{
    const ProfileSchema s = ProfileSchema::parse(R"(<Profile><Property name="Main" type="PropertyBag">
      <Property name="Credits" type="UInt32" value="300"/>
      <Property name="Offset" type="Int32" value="-2"/>
    </Property></Profile>)");
    Profile profile(s);
    profile.state.flyers["f1"] = FlyerState{true, true};
    const std::string expected = sealed(
        // String table: Main, Credits, Offset, CFlyerState.
        "\x04\x04Main\x07"
        "Credits\x06Offset\x0B"
        "CFlyerState"
        // One bag, Main (name 0, PropertyBag 15), of two values: Credits
        // (UInt32 3) 300 as a varint, Offset (Int32 7) -2 zigzag.
        "\x01\x00\x0F\x02\x01\x03\xAC\x02\x02\x07\x03"
        // Tree version 2 and size 9; one group of CFlyerState version 1
        // (zigzag 2), one state of 4 bytes: "f1" and both flags.
        "\x02\x09\x01\x03\x02\x01\x04\x02"
        "f1\x03"s);
    CHECK(ProfileStore::serialize(profile) == expected);
}

TEST_CASE("storeRoundTripsEveryField", "[profile]")
{
    const std::filesystem::path dir = scratch("roundtrip");
    const ProfileSchema s = schema();
    const Profile saved = sampleProfile(s);
    ProfileStore store(dir / "profile");
    CHECK(store.save(saved));
    CHECK(std::filesystem::exists(dir / "profile" / "ForzaProfile.bin"));

    ProfileStore reader(dir / "profile");
    const ProfileStore::LoadResult result = reader.load(s);
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    const Profile& loaded = result.profile;
    // Every property type, the schema's order kept.
    CHECK(sameProperties(loaded.bags(), saved.bags()));
    CHECK(loaded.value("Main/Flags") == PropertyValue(std::uint64_t{0x8000000000000001}));
    CHECK(loaded.value("Main/Medium") == PropertyValue(std::int64_t{-32768}));
    CHECK(loaded.value("Main/Gamma") == PropertyValue(0.35F));
    CHECK(loaded.value("Main/Precise") == PropertyValue(-1e300));
    CHECK(loaded.value("Main/Name") == PropertyValue(std::string("A <quoted> \"name\" & more, \xc3\xa9t\xc3\xa9")));
    CHECK(loaded.value("Main/Narrow") == PropertyValue(std::string("x")));
    // Every state kind.
    CHECK((loaded.state.roads && loaded.state.roads->visited == saved.state.roads->visited));
    CHECK(loaded.state.speedCameras.size() == std::size_t{3});
    const SpeedCameraState& camera = loaded.state.speedCameras.at("speed_camera_01");
    CHECK((camera.activityDiscovered && camera.discovered));
    CHECK(camera.personalBest == 36.123456F);
    CHECK(!loaded.state.speedCameras.at("speed_camera_02").discovered);
    CHECK(loaded.state.speedCameras.at("speed_camera_03").activityDiscovered);
    CHECK(!loaded.state.speedCameras.at("speed_camera_03").discovered);
    CHECK((loaded.state.flyers.at("flyer_001").collected && !loaded.state.flyers.at("flyer_001").activityDiscovered));
    CHECK((!loaded.state.flyers.at("flyer_002").collected && loaded.state.flyers.at("flyer_002").activityDiscovered));
    CHECK(loaded.state.gasStations.at("GasStation_01").discovered);
    CHECK(!loaded.state.gasStations.at("GasStation_02").discovered);
    CHECK(loaded.state.otherStates.size() == std::size_t{2});
    if (loaded.state.otherStates.size() == 2) {
        const OtherState& player = loaded.state.otherStates[0];
        CHECK(player.name == std::string("Player0"));
        CHECK(player.type == std::string("CPlayerState"));
        CHECK(player.version == 1);
        CHECK(sameProperties(player.fields, playerState().fields));
        CHECK(loaded.state.otherStates[1].type == std::string("CMissionState"));
        CHECK(loaded.state.otherStates[1].version == 3);
    }
    // Saving what was loaded writes the same file again.
    CHECK(ProfileStore::serialize(loaded) == ProfileStore::serialize(saved));
    CHECK(readAll(store.file()) == ProfileStore::serialize(saved));
}

TEST_CASE("storeKeepsWhatTheSchemaDoesNotHave", "[profile]")
{
    // Written by a build whose schema differs: Credits of another type, a
    // value out of its type's range, properties and a bag this schema does
    // not have, a known state of another version and a state of a type
    // this engine has no system for.
    const ProfileSchema other = ProfileSchema::parse(R"(<Profile>
      <Property name="Main" type="PropertyBag">
        <Property name="Credits" type="Int32" value="-3"/>
        <Property name="MetaDiff" type="UInt8" value="1"/>
        <Property name="RallyXP" type="Int32" value="77"/>
      </Property>
      <Property name="Rally" type="PropertyBag"><Property name="Stage" type="UInt8" value="3"/></Property>
    </Profile>)");
    Profile written(other);
    // What a damaged value would hold; set() refuses it.
    openhorizon::profile::findProperty(written.bags(), "Main/MetaDiff")->value = std::uint64_t{999};
    written.state.otherStates.push_back(
        OtherState{"speed_camera_09", "CSpeedCameraState", 2, {{"Discovered", PropertyType::Bool, true, {}}}});
    written.state.otherStates.push_back(playerState());
    written.state.flyers["flyer_002"] = FlyerState{false, true};

    const std::filesystem::path dir = scratch("unknown");
    CHECK(openhorizon::writeFileToDisk(dir / "ForzaProfile.bin", ProfileStore::serialize(written)));
    ProfileStore store(dir);
    const ProfileStore::LoadResult result = store.load(schema());
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    // Saved with another type, or out of range: the schema's default.
    CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    CHECK(result.profile.value("Main/MetaDiff") == PropertyValue(std::uint64_t{1}));
    // Not in the schema: kept.
    CHECK(result.profile.value("Main/RallyXP") == PropertyValue(std::int64_t{77}));
    CHECK(result.profile.value("Rally/Stage") == PropertyValue(std::uint64_t{3}));
    // A version the game's serializer refuses: dropped. Other types: kept.
    CHECK(result.profile.state.speedCameras.empty());
    CHECK(result.profile.state.otherStates.size() == std::size_t{1});
    CHECK((!result.profile.state.otherStates.empty()
        && sameProperties(result.profile.state.otherStates[0].fields, playerState().fields)));
    CHECK(result.profile.state.flyers.at("flyer_002").collected);
    CHECK(store.save(result.profile));
    const ProfileStore::LoadResult again = ProfileStore(dir).load(schema());
    CHECK(again.profile.value("Main/RallyXP") == PropertyValue(std::int64_t{77}));
    CHECK(again.profile.value("Rally/Stage") == PropertyValue(std::uint64_t{3}));
    CHECK(again.profile.state.otherStates.size() == std::size_t{1});

    // Bits past the road node count: that state is dropped, the rest read.
    const std::filesystem::path roads = scratch("roads");
    CHECK(openhorizon::writeFileToDisk(roads / "ForzaProfile.bin",
        sealed(
            // CRoadDiscovery in the table, no bags; a tree of one group of
            // one state: 40 nodes, but 64 bits set.
            "\x01\x0E"
            "CRoadDiscovery"
            "\x00"
            "\x02\x1C\x01\x00\x04\x01\x17\x0D"
            "RoadDiscovery\x28\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"s)));
    const ProfileStore::LoadResult tooMany = ProfileStore(roads).load(schema());
    CHECK(tooMany.status == ProfileStore::LoadStatus::Loaded);
    CHECK(!tooMany.profile.state.roads.has_value());
}

TEST_CASE("storeReplacesTheFileAtomically", "[profile]")
{
    const std::filesystem::path dir = scratch("atomic");
    const ProfileSchema s = schema();
    ProfileStore store(dir);
    Profile first(s);
    first.set("Main/Credits", std::uint64_t{1});
    CHECK(store.save(first));
    const std::string before = readAll(store.file());
    CHECK(!std::filesystem::exists(dir / "ForzaProfile.bin.tmp"));

    // The temporary file cannot be written: the save fails and the profile
    // on disk is the old one, whole.
    std::filesystem::create_directory(dir / "ForzaProfile.bin.tmp");
    Profile second(s);
    second.set("Main/Credits", std::uint64_t{2});
    CHECK(!store.save(second));
    CHECK(readAll(store.file()) == before);
    std::filesystem::remove(dir / "ForzaProfile.bin.tmp");

    CHECK(store.save(second));
    CHECK(!std::filesystem::exists(dir / "ForzaProfile.bin.tmp"));
    CHECK(ProfileStore(dir).load(s).profile.value("Main/Credits") == PropertyValue(std::uint64_t{2}));
}

TEST_CASE("storeRefusesAnotherFormatVersion", "[profile]")
{
    const std::filesystem::path dir = scratch("version");
    Profile rich(schema());
    rich.set("Main/Credits", std::uint64_t{9});
    std::string future = ProfileStore::serialize(rich);
    future[4] = '\x03';
    future = resealed(future);
    writeAll(dir / "ForzaProfile.bin", future);
    ProfileStore store(dir);
    const ProfileStore::LoadResult result = store.load(schema());
    CHECK(result.status == ProfileStore::LoadStatus::VersionMismatch);
    CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    // Untouched until a save, which keeps it aside.
    CHECK(readAll(dir / "ForzaProfile.bin") == future);
    CHECK(store.save(result.profile));
    CHECK(readAll(dir / "ForzaProfile.bin.unreadable") == future);
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Loaded);

    writeAll(dir / "ForzaProfile.bin", "\x01\x02 not a profile");
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Unreadable);
    std::filesystem::remove(dir / "ForzaProfile.bin");
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Missing);
}

TEST_CASE("storeIgnoresTheXmlProfile", "[profile]")
{
    // Format version 1's XML file is neither read nor touched: the folder
    // holds a new profile, saved next to it.
    const std::filesystem::path dir = scratch("xml");
    const std::string xml = R"(<ForzaProfile version="1"><Profile>
      <Property name="Main" type="PropertyBag"><Property name="Credits" type="UInt32" value="9"/></Property>
    </Profile></ForzaProfile>)";
    writeAll(dir / "ForzaProfile.xml", xml);
    ProfileStore store(dir);
    const ProfileStore::LoadResult result = store.load(schema());
    CHECK(result.status == ProfileStore::LoadStatus::Missing);
    CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    CHECK(store.save(result.profile));
    CHECK(std::filesystem::exists(dir / "ForzaProfile.bin"));
    CHECK(readAll(dir / "ForzaProfile.xml") == xml);
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator())
        == std::ptrdiff_t{2});
}

TEST_CASE("storeRefusesDamagedFiles", "[profile]")
{
    const std::filesystem::path dir = scratch("damaged");
    const std::string good = ProfileStore::serialize(sampleProfile(schema()));
    std::string flipped = good;
    flipped[good.size() / 2] = static_cast<char>(flipped[good.size() / 2] ^ 0x10);
    // A thousand bags, each the only property of the one before.
    std::string nested = "\x01\x01"
                         "a"s;
    for (int depth = 0; depth < 1000; ++depth) {
        nested += "\x01\x00\x0F"s;
    }
    nested += "\x00"s;
    const std::vector<std::string> damaged{good.substr(0, good.size() - 1), good.substr(0, good.size() / 2),
        good.substr(0, 8), flipped, good + '\0',
        // Checksum right, content not the layout: a name past the table,
        // bags nested past any schema's.
        sealed("\x00\x01\x05\x00"s), sealed(nested)};
    for (const std::string& bytes : damaged) {
        writeAll(dir / "ForzaProfile.bin", bytes);
        ProfileStore store(dir);
        const ProfileStore::LoadResult result = store.load(schema());
        CHECK(result.status == ProfileStore::LoadStatus::Unreadable);
        // Nothing of it is read.
        CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
        CHECK(result.profile.state.flyers.empty());
        CHECK(store.save(result.profile));
        CHECK(readAll(dir / "ForzaProfile.bin.unreadable") == bytes);
        CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Loaded);
    }
}

TEST_CASE("defaultDirectoryIsInTheUserDataFolder", "[profile]")
{
    // The folder rules themselves are tst_base's; the profile goes in
    // `profile` inside whatever the Data folder is.
    const char* variable = openhorizon::userFolderVariable(openhorizon::UserFolder::Data);
    const std::optional<std::string> previous = openhorizon::environmentVariable(variable);
    const std::filesystem::path dir = scratch("data-home");
    openhorizon::setEnvironmentVariable(variable, openhorizon::pathToUtf8(dir));
    CHECK(ProfileStore::defaultDirectory() == openhorizon::userFolder(openhorizon::UserFolder::Data) / "profile");
    CHECK(openhorizon::pathToUtf8(ProfileStore::defaultDirectory()).starts_with(openhorizon::pathToUtf8(dir)));
    if (previous) {
        openhorizon::setEnvironmentVariable(variable, *previous);
    } else {
        openhorizon::unsetEnvironmentVariable(variable);
    }
}

TEST_CASE("discSchemaIsTheGamesLayout", "[profile]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const ProfileSchema s = ProfileSchema::load(vfs);
    std::vector<std::string> names;
    for (const openhorizon::profile::Property& bag : s.bags()) {
        names.push_back(bag.name);
        CHECK(bag.type == PropertyType::PropertyBag);
        for (const openhorizon::profile::Property& property : bag.children) {
            CHECK(property.type != PropertyType::PropertyBag);
        }
    }
    const std::vector<std::string> expected{"Main", "ControllerOptions", "WheelOptions", "BristolOptions",
        "ControllerAdvancedOptions", "WheelAdvancedOptions", "BristolAdvancedOptions", "Options", "HeadTracking"};
    CHECK(names == expected);
    CHECK((s.find("Main/Credits") && s.find("Main/Credits")->value == PropertyValue(std::uint64_t{500})));
    CHECK((s.find("Main/ShowroomCar") && s.find("Main/ShowroomCar")->value == PropertyValue(std::int64_t{1131})));
    CHECK((s.find("Main/UpgradeDiscount") && s.find("Main/UpgradeDiscount")->type == PropertyType::UInt32));
    CHECK((s.find("Main/Flags") && s.find("Main/Flags")->type == PropertyType::UInt64));
    CHECK((s.find("Main/Shifting") && s.find("Main/Shifting")->type == PropertyType::UInt8));
    CHECK((s.find("Options/Units") && s.find("Options/Units")->type == PropertyType::Int32));
    const openhorizon::profile::Property* deadzone = s.find("ControllerAdvancedOptions/SteeringAxisDeadzoneInside");
    CHECK((deadzone && deadzone->value == PropertyValue(0.24F)));
    CHECK((s.find("HeadTracking") && s.find("HeadTracking")->children.empty()));

    // The disc's defaults survive the store unchanged.
    const std::filesystem::path folder = scratch("disc");
    ProfileStore store(folder);
    CHECK(store.save(Profile(s)));
    const ProfileStore::LoadResult result = ProfileStore(folder).load(s);
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    CHECK(ProfileStore::serialize(result.profile) == ProfileStore::serialize(Profile(s)));
    std::fprintf(stderr, "schema: %zu bags, %zu values\n", s.bags().size(), s.valueCount());

    // A played profile round-trips on the disc's schema too.
    const Profile played = playedProfile(s);
    const std::filesystem::path playedFolder = scratch("disc-played");
    CHECK(ProfileStore(playedFolder).save(played));
    const ProfileStore::LoadResult reread = ProfileStore(playedFolder).load(s);
    CHECK(reread.status == ProfileStore::LoadStatus::Loaded);
    CHECK(ProfileStore::serialize(reread.profile) == ProfileStore::serialize(played));
    std::fprintf(stderr, "profile: new %zu bytes, played %zu bytes\n", ProfileStore::serialize(Profile(s)).size(),
        readAll(ProfileStore(playedFolder).file()).size());
}

/// Points the per-user data folder under the temporary directory before
/// any case runs: never the player's own profile.
class DataFolder : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override
    {
        openhorizon::setEnvironmentVariable(openhorizon::userFolderVariable(openhorizon::UserFolder::Data),
            openhorizon::pathToUtf8(std::filesystem::temp_directory_path() / "openhorizon-tst-profile" / "data"));
    }
};

} // namespace

CATCH_REGISTER_LISTENER(DataFolder)
