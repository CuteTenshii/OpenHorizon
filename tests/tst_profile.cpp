// The player's profile: the schema reader (ForzaProfile.sch) on synthetic
// XML and on the disc's schema when FH1_GAME_DIR names it, and the profile
// store's file (round trip, atomic replacement, format version). Every file
// goes to a folder under the temporary directory; XDG_DATA_HOME points
// there too, so the player's own profile is never touched.

#include "TestHarness.h"

#include "profile/Profile.h"
#include "profile/ProfileSchema.h"
#include "profile/ProfileStore.h"
#include "vfs/Vfs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fh::profile::FlyerState;
using fh::profile::GasStationState;
using fh::profile::Profile;
using fh::profile::ProfileSchema;
using fh::profile::ProfileStore;
using fh::profile::PropertyType;
using fh::profile::PropertyValue;
using fh::profile::RoadDiscoveryState;
using fh::profile::SpeedCameraState;

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

/// A fresh, empty folder for one test.
std::filesystem::path scratch(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fh1-tst-profile" / name;
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

TEST(typeNamesAreTheGames)
{
    // The table at 0x82154ee8: names, aliases and ids.
    CHECK(fh::profile::propertyTypeFromName("Bool") == PropertyType::Bool);
    CHECK(fh::profile::propertyTypeFromName("Byte") == PropertyType::UInt8);
    CHECK(fh::profile::propertyTypeFromName("UInt") == PropertyType::UInt32);
    CHECK(fh::profile::propertyTypeFromName("Int") == PropertyType::Int32);
    CHECK(fh::profile::propertyTypeFromName("Float") == PropertyType::Float32);
    CHECK(fh::profile::propertyTypeFromName("String") == PropertyType::StringWide);
    CHECK(fh::profile::propertyTypeFromName("PropertyBag") == PropertyType::PropertyBag);
    CHECK(!fh::profile::propertyTypeFromName("bool").has_value());
    CHECK(!fh::profile::propertyTypeFromName("Double").has_value());
    CHECK_EQ(static_cast<int>(PropertyType::UInt64), 4);
    CHECK_EQ(static_cast<int>(PropertyType::Float64), 10);
    CHECK_EQ(static_cast<int>(PropertyType::PropertyBag), 15);
    CHECK_EQ(fh::profile::propertyTypeName(PropertyType::UInt8), std::string_view("UInt8"));
}

TEST(schemaReadsBagsTypesAndDefaults)
{
    const ProfileSchema s = schema();
    CHECK_EQ(s.bags().size(), std::size_t{3});
    CHECK_EQ(s.valueCount(), std::size_t{14});
    const fh::profile::Property* credits = s.find("Main/Credits");
    CHECK(credits != nullptr);
    CHECK(credits && credits->type == PropertyType::UInt32);
    CHECK(credits && credits->value == PropertyValue(std::uint64_t{500}));
    CHECK(s.find("Main/IsVip") && s.find("Main/IsVip")->value == PropertyValue(false));
    CHECK(s.find("Main/Flags")->value == PropertyValue(std::numeric_limits<std::uint64_t>::max()));
    CHECK(s.find("Main/Small")->value == PropertyValue(std::int64_t{-128}));
    CHECK(s.find("Main/Big")->value == PropertyValue(std::numeric_limits<std::int64_t>::min()));
    CHECK(s.find("Main/Gamma")->value == PropertyValue(0.6F));
    CHECK(s.find("Main/Precise")->value == PropertyValue(0.1));
    CHECK(s.find("Main/Name")->type == PropertyType::StringWide);
    CHECK(s.find("Main/Name")->value == PropertyValue(std::string("Player")));
    CHECK(s.find("Options/Units") != nullptr);
    CHECK(s.find("HeadTracking") && s.find("HeadTracking")->children.empty());
    CHECK(s.find("Main") && s.find("Main")->type == PropertyType::PropertyBag);
    CHECK(s.find("Main/Missing") == nullptr);
    CHECK(s.find("Options/Units/Deeper") == nullptr);
    CHECK(s.find("Nothing") == nullptr);
    // File order is kept.
    CHECK_EQ(s.bags()[0].children[0].name, std::string("IsVip"));
    CHECK_EQ(s.bags()[2].name, std::string("HeadTracking"));
}

TEST(schemaRejectsMalformedProperties)
{
    const auto bag = [](std::string_view inner) {
        return std::string("<Profile><Property name=\"Main\" type=\"PropertyBag\">") + std::string(inner)
            + "</Property></Profile>";
    };
    CHECK_THROWS(ProfileSchema::parse("not xml <"));
    CHECK_THROWS(ProfileSchema::parse("<Other/>"));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property type="Int32" value="0"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int33" value="0"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int32"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int32" value="x"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Bool" value="2"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="UInt8" value="256"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="UInt32" value="-1"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Int8" value="128"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="Float32" value="1.0f"/>)")));
    CHECK_THROWS(ProfileSchema::parse(bag(R"(<Property name="A" type="PropertyBag" value="1"/>)")));
    CHECK_THROWS(ProfileSchema::parse(
        bag(R"(<Property name="A" type="Int32" value="0"><Property name="B" type="Int32" value="0"/></Property>)")));
    CHECK_THROWS(ProfileSchema::parse(
        bag(R"(<Property name="A" type="Int32" value="0"/><Property name="A" type="Int32" value="1"/>)")));
    CHECK_THROWS(ProfileSchema::parse(R"(<Profile><Property name="A" type="Int32" value="0"/></Profile>)"));
}

TEST(valuesReadBackExactly)
{
    for (const float number : {0.24F, 0.6F, 1.0F, 1e-7F, 123456.789F, -0.0F}) {
        const PropertyValue value = number;
        const auto parsed = fh::profile::parseValue(PropertyType::Float32, fh::profile::formatValue(value));
        CHECK(parsed && *parsed == value);
    }
    const PropertyValue big = std::numeric_limits<std::uint64_t>::max();
    CHECK(fh::profile::parseValue(PropertyType::UInt64, fh::profile::formatValue(big)) == big);
    CHECK_EQ(fh::profile::formatValue(PropertyValue(true)), std::string("1"));
    CHECK(!fh::profile::valueFits(PropertyType::UInt8, PropertyValue(std::uint64_t{300})));
    CHECK(!fh::profile::valueFits(PropertyType::Int32, PropertyValue(std::uint64_t{3})));
    CHECK(fh::profile::valueFits(PropertyType::Int16, PropertyValue(std::int64_t{-32768})));
}

TEST(profileStartsFromTheDefaultsAndChecksTypes)
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

Profile sampleProfile(const ProfileSchema& s)
{
    Profile profile(s);
    profile.set("Main/Credits", std::uint64_t{4200});
    profile.set("Main/IsVip", true);
    profile.set("Main/Gamma", 0.35F);
    profile.set("Main/Big", std::int64_t{-5});
    profile.set("Main/Name", std::string("A <quoted> \"name\" & more"));
    RoadDiscoveryState roads;
    roads.visited.assign(70, false);
    for (const std::size_t node : {0U, 1U, 31U, 32U, 63U, 64U, 69U}) {
        roads.visited[node] = true;
    }
    profile.state.roads = roads;
    profile.state.speedCameras["speed_camera_01"] = SpeedCameraState{true, true, 36.123456F};
    profile.state.speedCameras["speed_camera_02"] = SpeedCameraState{false, false, 0.0F};
    profile.state.flyers["flyer_001"] = FlyerState{false, true};
    profile.state.gasStations["GasStation_01"] = GasStationState{true};
    profile.state.otherStates.emplace_back(
        R"(<State name="Player0" type="CPlayerState" version="1"><Property name="Raw" type="UInt32" value="7"/></State>)");
    return profile;
}

TEST(storeRoundTripsEveryField)
{
    const std::filesystem::path dir = scratch("roundtrip");
    const ProfileSchema s = schema();
    const Profile saved = sampleProfile(s);
    ProfileStore store(dir / "profile");
    CHECK(store.save(saved));
    CHECK(std::filesystem::exists(dir / "profile" / "ForzaProfile.xml"));

    ProfileStore reader(dir / "profile");
    const ProfileStore::LoadResult result = reader.load(s);
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    const Profile& loaded = result.profile;
    CHECK(loaded.value("Main/Credits") == PropertyValue(std::uint64_t{4200}));
    CHECK(loaded.value("Main/IsVip") == PropertyValue(true));
    CHECK(loaded.value("Main/Gamma") == PropertyValue(0.35F));
    CHECK(loaded.value("Main/Big") == PropertyValue(std::int64_t{-5}));
    CHECK(loaded.value("Main/Name") == PropertyValue(std::string("A <quoted> \"name\" & more")));
    CHECK(loaded.value("Main/ShowroomCar") == PropertyValue(std::int64_t{1131}));
    CHECK(loaded.state.roads.has_value());
    CHECK(loaded.state.roads && loaded.state.roads->visited == saved.state.roads->visited);
    CHECK_EQ(loaded.state.speedCameras.size(), std::size_t{2});
    const SpeedCameraState& camera = loaded.state.speedCameras.at("speed_camera_01");
    CHECK(camera.activityDiscovered && camera.discovered);
    CHECK(camera.personalBest == 36.123456F);
    CHECK(!loaded.state.speedCameras.at("speed_camera_02").discovered);
    CHECK(loaded.state.flyers.at("flyer_001").collected);
    CHECK(loaded.state.gasStations.at("GasStation_01").discovered);
    CHECK_EQ(loaded.state.otherStates.size(), std::size_t{1});
    // Saving what was loaded writes the same file again.
    CHECK_EQ(ProfileStore::serialize(loaded), ProfileStore::serialize(saved));
}

TEST(storeKeepsWhatTheSchemaDoesNotHave)
{
    const std::filesystem::path dir = scratch("unknown");
    writeAll(dir / "ForzaProfile.xml", R"(<ForzaProfile version="1"><Profile>
      <Property name="Main" type="PropertyBag">
        <Property name="Credits" type="Int32" value="-3"/>
        <Property name="MetaDiff" type="UInt8" value="999"/>
        <Property name="RallyXP" type="Int32" value="77"/>
      </Property>
      <Property name="Rally" type="PropertyBag"><Property name="Stage" type="UInt8" value="3"/></Property>
    </Profile><GameState version="2">
      <State name="speed_camera_09" type="CSpeedCameraState" version="2"/>
      <State name="RoadDiscovery" type="CRoadDiscovery" version="2" nodes="40">ffffffff</State>
      <State name="flyer_002" type="CFlyerState" version="1"><Property name="Collected" type="Bool" value="1"/></State>
    </GameState></ForzaProfile>)");
    ProfileStore store(dir);
    const ProfileStore::LoadResult result = store.load(schema());
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    // Saved with another type, or out of range: the schema's default.
    CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    CHECK(result.profile.value("Main/MetaDiff") == PropertyValue(std::uint64_t{1}));
    // Not in the schema: kept.
    CHECK(result.profile.value("Main/RallyXP") == PropertyValue(std::int64_t{77}));
    CHECK(result.profile.value("Rally/Stage") == PropertyValue(std::uint64_t{3}));
    // A version the game's serializer refuses, and too few words for the
    // node count: dropped.
    CHECK(result.profile.state.speedCameras.empty());
    CHECK(!result.profile.state.roads.has_value());
    CHECK(result.profile.state.flyers.at("flyer_002").collected);
    CHECK(store.save(result.profile));
    const ProfileStore::LoadResult again = ProfileStore(dir).load(schema());
    CHECK(again.profile.value("Main/RallyXP") == PropertyValue(std::int64_t{77}));
    CHECK(again.profile.value("Rally/Stage") == PropertyValue(std::uint64_t{3}));
}

TEST(storeReplacesTheFileAtomically)
{
    const std::filesystem::path dir = scratch("atomic");
    const ProfileSchema s = schema();
    ProfileStore store(dir);
    Profile first(s);
    first.set("Main/Credits", std::uint64_t{1});
    CHECK(store.save(first));
    const std::string before = readAll(store.file());
    CHECK(!std::filesystem::exists(dir / "ForzaProfile.xml.tmp"));

    // The temporary file cannot be written: the save fails and the profile
    // on disk is the old one, whole.
    std::filesystem::create_directory(dir / "ForzaProfile.xml.tmp");
    Profile second(s);
    second.set("Main/Credits", std::uint64_t{2});
    CHECK(!store.save(second));
    CHECK_EQ(readAll(store.file()), before);
    std::filesystem::remove(dir / "ForzaProfile.xml.tmp");

    CHECK(store.save(second));
    CHECK(!std::filesystem::exists(dir / "ForzaProfile.xml.tmp"));
    CHECK(ProfileStore(dir).load(s).profile.value("Main/Credits") == PropertyValue(std::uint64_t{2}));
}

TEST(storeRefusesAnotherFormatVersion)
{
    const std::filesystem::path dir = scratch("version");
    const std::string future = R"(<ForzaProfile version="2"><Profile>
      <Property name="Main" type="PropertyBag"><Property name="Credits" type="UInt32" value="9"/></Property>
    </Profile></ForzaProfile>)";
    writeAll(dir / "ForzaProfile.xml", future);
    ProfileStore store(dir);
    const ProfileStore::LoadResult result = store.load(schema());
    CHECK(result.status == ProfileStore::LoadStatus::VersionMismatch);
    CHECK(result.profile.value("Main/Credits") == PropertyValue(std::uint64_t{500}));
    // Untouched until a save, which keeps it aside.
    CHECK_EQ(readAll(dir / "ForzaProfile.xml"), future);
    CHECK(store.save(result.profile));
    CHECK_EQ(readAll(dir / "ForzaProfile.xml.unreadable"), future);
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Loaded);

    writeAll(dir / "ForzaProfile.xml", "\x01\x02 not a profile");
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Unreadable);
    std::filesystem::remove(dir / "ForzaProfile.xml");
    CHECK(ProfileStore(dir).load(schema()).status == ProfileStore::LoadStatus::Missing);
}

TEST(defaultDirectoryIsUnderXdgDataHome)
{
    const std::filesystem::path dir = scratch("xdg");
    const char* previous = std::getenv("XDG_DATA_HOME");
    const std::string saved = previous != nullptr ? previous : "";
    setenv("XDG_DATA_HOME", dir.c_str(), 1);
    CHECK(ProfileStore::defaultDirectory() == dir / "fh1-native" / "profile");
    unsetenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        CHECK(ProfileStore::defaultDirectory() == std::filesystem::path(home) / ".local/share/fh1-native/profile");
    }
    if (previous != nullptr) {
        setenv("XDG_DATA_HOME", saved.c_str(), 1);
    }
}

TEST(discSchemaIsTheGamesLayout)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const ProfileSchema s = ProfileSchema::load(vfs);
    std::vector<std::string> names;
    for (const fh::profile::Property& bag : s.bags()) {
        names.push_back(bag.name);
        CHECK(bag.type == PropertyType::PropertyBag);
        for (const fh::profile::Property& property : bag.children) {
            CHECK(property.type != PropertyType::PropertyBag);
        }
    }
    const std::vector<std::string> expected{"Main", "ControllerOptions", "WheelOptions", "BristolOptions",
        "ControllerAdvancedOptions", "WheelAdvancedOptions", "BristolAdvancedOptions", "Options", "HeadTracking"};
    CHECK(names == expected);
    CHECK(s.find("Main/Credits") && s.find("Main/Credits")->value == PropertyValue(std::uint64_t{500}));
    CHECK(s.find("Main/ShowroomCar") && s.find("Main/ShowroomCar")->value == PropertyValue(std::int64_t{1131}));
    CHECK(s.find("Main/UpgradeDiscount") && s.find("Main/UpgradeDiscount")->type == PropertyType::UInt32);
    CHECK(s.find("Main/Flags") && s.find("Main/Flags")->type == PropertyType::UInt64);
    CHECK(s.find("Main/Shifting") && s.find("Main/Shifting")->type == PropertyType::UInt8);
    CHECK(s.find("Options/Units") && s.find("Options/Units")->type == PropertyType::Int32);
    const fh::profile::Property* deadzone = s.find("ControllerAdvancedOptions/SteeringAxisDeadzoneInside");
    CHECK(deadzone && deadzone->value == PropertyValue(0.24F));
    CHECK(s.find("HeadTracking") && s.find("HeadTracking")->children.empty());

    // The disc's defaults survive the store unchanged.
    const std::filesystem::path folder = scratch("disc");
    ProfileStore store(folder);
    CHECK(store.save(Profile(s)));
    const ProfileStore::LoadResult result = ProfileStore(folder).load(s);
    CHECK(result.status == ProfileStore::LoadStatus::Loaded);
    CHECK_EQ(ProfileStore::serialize(result.profile), ProfileStore::serialize(Profile(s)));
    std::fprintf(stderr, "schema: %zu bags, %zu values\n", s.bags().size(), s.valueCount());
}

} // namespace

int main(int argc, char** argv)
{
    // Never the player's own profile.
    setenv("XDG_DATA_HOME", (std::filesystem::temp_directory_path() / "fh1-tst-profile" / "data").c_str(), 1);
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
