#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::profile {

/// Numbered as the game's type name table (0x82154ee8), which also has the aliases Byte, UInt, Int, Float and
/// String; ids 13 and 14 have no name there.
enum class PropertyType : std::uint8_t {
    Bool = 0,
    UInt8 = 1,
    UInt16 = 2,
    UInt32 = 3,
    UInt64 = 4,
    Int8 = 5,
    Int16 = 6,
    Int32 = 7,
    Int64 = 8,
    Float32 = 9,
    Float64 = 10,
    StringNarrow = 11,
    StringWide = 12,
    PropertyBag = 15,
};

/// Aliases included.
std::optional<PropertyType> propertyTypeFromName(std::string_view name);
/// The table's first name, the one the schema file uses.
std::string_view propertyTypeName(PropertyType type);

/// Monostate for a bag.
using PropertyValue = std::variant<std::monostate, bool, std::uint64_t, std::int64_t, float, double, std::string>;

/// Whether `value` has the kind `type` holds and fits its width.
bool valueFits(PropertyType type, const PropertyValue& value);
/// "0" and "1" for Bool. Nothing when it does not parse or does not fit.
std::optional<PropertyValue> parseValue(PropertyType type, std::string_view text);
/// The inverse of parseValue(); numbers round-trip exactly.
std::string formatValue(const PropertyValue& value);

struct Property {
    std::string name;
    PropertyType type = PropertyType::PropertyBag;
    PropertyValue value;
    /// In file order; empty for values.
    std::vector<Property> children;

    const Property* child(std::string_view childName) const;
    Property* child(std::string_view childName);
};

/// The profile's layout and defaults. Verified: path string at 0x8201ab08, loaded at 0x82581198 with the saved
/// values read over it; paths join bag and property with "/" ("Main/Credits"); nine flat bags, no version number.
/// Paths the executable names beyond it (Main/RallyXP, HeadTracking/Enabled) are added in an untraced way and left out.
class ProfileSchema {
public:
    static constexpr std::string_view kPath = "media/profileschema/ForzaProfile.sch";

    /// Throws DataError when the XML or a property is malformed, including a name used twice in one bag.
    static ProfileSchema parse(std::string_view xml);
    /// Throws DataError.
    static ProfileSchema load(const Vfs& vfs);

    /// In file order.
    const std::vector<Property>& bags() const { return m_bags; }
    /// `path` like "Main/Credits"; null when absent.
    const Property* find(std::string_view path) const;
    /// Values (not bags) in the whole schema.
    std::size_t valueCount() const;

private:
    std::vector<Property> m_bags;
};

const Property* findProperty(const std::vector<Property>& bags, std::string_view path);
Property* findProperty(std::vector<Property>& bags, std::string_view path);

} // namespace openhorizon::profile
