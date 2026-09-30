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

/// A property type as ForzaProfile.sch names it, numbered as the game's
/// type name table numbers it (0x82154ee8: pairs of name and id, read by
/// the schema loader). The table also has the aliases Byte (UInt8), UInt
/// (UInt32), Int (Int32), Float (Float32) and String (StringWide), which
/// propertyTypeFromName() accepts; ids 13 and 14 have no name there.
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

/// The type named `name` in the game's table, aliases included.
std::optional<PropertyType> propertyTypeFromName(std::string_view name);
/// The table's first name for `type` (the one the schema file uses).
std::string_view propertyTypeName(PropertyType type);

/// A property's value: nothing for a bag, bool for Bool, an unsigned
/// integer for UInt8 to UInt64, a signed one for Int8 to Int64, float for
/// Float32, double for Float64 and text for both string types.
using PropertyValue = std::variant<std::monostate, bool, std::uint64_t, std::int64_t, float, double, std::string>;

/// Whether `value` has the kind `type` holds and fits its width.
bool valueFits(PropertyType type, const PropertyValue& value);
/// Reads a `value` attribute as the schema writes one for `type` ("0" and
/// "1" for Bool, decimal numbers, text as is). Nothing when it does not
/// parse or does not fit.
std::optional<PropertyValue> parseValue(PropertyType type, std::string_view text);
/// The inverse of parseValue(); numbers round-trip exactly.
std::string formatValue(const PropertyValue& value);

/// One `<Property>` of the schema: a named, typed value, or a bag of them.
struct Property {
    std::string name;
    PropertyType type = PropertyType::PropertyBag;
    PropertyValue value;
    /// A bag's properties in file order; empty for values.
    std::vector<Property> children;

    const Property* child(std::string_view childName) const;
    Property* child(std::string_view childName);
};

/// The profile's layout and defaults: media/profileschema/ForzaProfile.sch.
///
/// Verified: the game reads it from GAME:\Media\ProfileSchema\ForzaProfile.sch
/// (string at 0x8201ab08, loaded at 0x82581198 as XML whose root element is
/// "Profile") into the property bags its profile object keeps (+32), and
/// then reads the saved values over them. Every `<Property>` has a name and
/// a type from the table above; values have a `value` attribute (the
/// default), bags hold properties. The game names properties by path, the
/// bag and the property joined by "/" ("Main/Credits", "Options/Units" in
/// its string table).
///
/// Verified on the disc's schema: nine top-level bags (Main,
/// ControllerOptions, WheelOptions, BristolOptions,
/// ControllerAdvancedOptions, WheelAdvancedOptions, BristolAdvancedOptions,
/// Options, HeadTracking, and no deeper nesting), types Bool, UInt8, UInt32,
/// UInt64, Int32 and Float32 only, and no version number anywhere in the
/// file. The executable also names paths the schema lacks: Main/RallyXP,
/// Main/CoDriver and others of the rally expansion, and HeadTracking/Enabled
/// and the other head tracking settings under the schema's empty bag. How
/// those are added was not traced, so they are not part of this layout.
class ProfileSchema {
public:
    static constexpr std::string_view kPath = "media/profileschema/ForzaProfile.sch";

    /// Throws DataError when the XML or a property is malformed: a missing
    /// name or type, an unknown type, a value that does not parse for its
    /// type, a value on a bag, a nested property under a value, or a name
    /// used twice in one bag.
    static ProfileSchema parse(std::string_view xml);
    /// Reads kPath from the game's files. Throws DataError.
    static ProfileSchema load(const Vfs& vfs);

    /// The top-level bags in file order.
    const std::vector<Property>& bags() const { return m_bags; }
    /// The property at `path` ("Main/Credits"), or null.
    const Property* find(std::string_view path) const;
    /// Values (not bags) in the whole schema.
    std::size_t valueCount() const;

private:
    std::vector<Property> m_bags;
};

/// The property at `path` below `bags`, splitting the path at "/".
const Property* findProperty(const std::vector<Property>& bags, std::string_view path);
Property* findProperty(std::vector<Property>& bags, std::string_view path);

} // namespace openhorizon::profile
