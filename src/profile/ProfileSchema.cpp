#include "ProfileSchema.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <array>
#include <charconv>
#include <format>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace fh::profile {

namespace {

struct TypeName {
    std::string_view name;
    PropertyType type;
};

/// The game's table at 0x82154ee8, in its order; the first name of each
/// type is the one propertyTypeName() gives.
constexpr std::array<TypeName, 19> kTypeNames{{
    {"Bool", PropertyType::Bool},
    {"Byte", PropertyType::UInt8},
    {"UInt", PropertyType::UInt32},
    {"UInt8", PropertyType::UInt8},
    {"UInt16", PropertyType::UInt16},
    {"UInt32", PropertyType::UInt32},
    {"UInt64", PropertyType::UInt64},
    {"Int", PropertyType::Int32},
    {"Int8", PropertyType::Int8},
    {"Int16", PropertyType::Int16},
    {"Int32", PropertyType::Int32},
    {"Int64", PropertyType::Int64},
    {"Float", PropertyType::Float32},
    {"Float32", PropertyType::Float32},
    {"Float64", PropertyType::Float64},
    {"String", PropertyType::StringWide},
    {"StringNarrow", PropertyType::StringNarrow},
    {"StringWide", PropertyType::StringWide},
    {"PropertyBag", PropertyType::PropertyBag},
}};

std::uint64_t unsignedMax(PropertyType type)
{
    switch (type) {
    case PropertyType::UInt8:
        return std::numeric_limits<std::uint8_t>::max();
    case PropertyType::UInt16:
        return std::numeric_limits<std::uint16_t>::max();
    case PropertyType::UInt32:
        return std::numeric_limits<std::uint32_t>::max();
    default:
        return std::numeric_limits<std::uint64_t>::max();
    }
}

std::pair<std::int64_t, std::int64_t> signedRange(PropertyType type)
{
    switch (type) {
    case PropertyType::Int8:
        return {std::numeric_limits<std::int8_t>::min(), std::numeric_limits<std::int8_t>::max()};
    case PropertyType::Int16:
        return {std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()};
    case PropertyType::Int32:
        return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
    default:
        return {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    }
}

bool isUnsigned(PropertyType type)
{
    return type == PropertyType::UInt8 || type == PropertyType::UInt16 || type == PropertyType::UInt32
        || type == PropertyType::UInt64;
}

bool isSigned(PropertyType type)
{
    return type == PropertyType::Int8 || type == PropertyType::Int16 || type == PropertyType::Int32
        || type == PropertyType::Int64;
}

template <typename T> std::optional<T> parseNumber(std::string_view text)
{
    T number{};
    const char* first = text.data();
    const char* end = first + text.size();
    const auto [last, error] = std::from_chars(first, end, number);
    if (error != std::errc() || last != end) {
        return std::nullopt;
    }
    return number;
}

Property parseProperty(const pugi::xml_node node, std::string_view where)
{
    Property property;
    property.name = node.attribute("name").as_string();
    if (property.name.empty()) {
        throw DataError("{}: a property has no name", where);
    }
    const std::string_view typeName = node.attribute("type").as_string();
    const std::optional<PropertyType> type = propertyTypeFromName(typeName);
    if (!type) {
        throw DataError("{}: {} has unknown type \"{}\"", where, property.name, typeName);
    }
    property.type = *type;
    const pugi::xml_attribute value = node.attribute("value");
    if (property.type == PropertyType::PropertyBag) {
        if (value) {
            throw DataError("{}: bag {} has a value", where, property.name);
        }
        const std::string path = std::format("{}/{}", where, property.name);
        std::unordered_set<std::string> names;
        for (const pugi::xml_node child : node.children("Property")) {
            Property parsed = parseProperty(child, path);
            if (!names.insert(parsed.name).second) {
                throw DataError("{}: {} is defined twice", path, parsed.name);
            }
            property.children.push_back(std::move(parsed));
        }
        return property;
    }
    if (node.child("Property")) {
        throw DataError("{}: {} is a {} but holds properties", where, property.name, typeName);
    }
    std::optional<PropertyValue> parsed = parseValue(property.type, value.as_string());
    if (!value || !parsed) {
        throw DataError("{}: {} has no valid {} value", where, property.name, typeName);
    }
    property.value = std::move(*parsed);
    return property;
}

std::size_t countValues(const std::vector<Property>& properties)
{
    std::size_t count = 0;
    for (const Property& property : properties) {
        count += property.type == PropertyType::PropertyBag ? countValues(property.children) : 1;
    }
    return count;
}

} // namespace

std::optional<PropertyType> propertyTypeFromName(std::string_view name)
{
    for (const TypeName& entry : kTypeNames) {
        if (entry.name == name) {
            return entry.type;
        }
    }
    return std::nullopt;
}

std::string_view propertyTypeName(PropertyType type)
{
    switch (type) {
    case PropertyType::Bool:
        return "Bool";
    case PropertyType::UInt8:
        return "UInt8";
    case PropertyType::UInt16:
        return "UInt16";
    case PropertyType::UInt32:
        return "UInt32";
    case PropertyType::UInt64:
        return "UInt64";
    case PropertyType::Int8:
        return "Int8";
    case PropertyType::Int16:
        return "Int16";
    case PropertyType::Int32:
        return "Int32";
    case PropertyType::Int64:
        return "Int64";
    case PropertyType::Float32:
        return "Float32";
    case PropertyType::Float64:
        return "Float64";
    case PropertyType::StringNarrow:
        return "StringNarrow";
    case PropertyType::StringWide:
        return "StringWide";
    case PropertyType::PropertyBag:
        return "PropertyBag";
    }
    return "PropertyBag";
}

bool valueFits(PropertyType type, const PropertyValue& value)
{
    if (type == PropertyType::PropertyBag) {
        return std::holds_alternative<std::monostate>(value);
    }
    if (type == PropertyType::Bool) {
        return std::holds_alternative<bool>(value);
    }
    if (isUnsigned(type)) {
        const auto* number = std::get_if<std::uint64_t>(&value);
        return number != nullptr && *number <= unsignedMax(type);
    }
    if (isSigned(type)) {
        const auto* number = std::get_if<std::int64_t>(&value);
        const auto [low, high] = signedRange(type);
        return number != nullptr && *number >= low && *number <= high;
    }
    if (type == PropertyType::Float32) {
        return std::holds_alternative<float>(value);
    }
    if (type == PropertyType::Float64) {
        return std::holds_alternative<double>(value);
    }
    return std::holds_alternative<std::string>(value);
}

std::optional<PropertyValue> parseValue(PropertyType type, std::string_view text)
{
    std::optional<PropertyValue> value;
    switch (type) {
    case PropertyType::PropertyBag:
        return std::nullopt;
    case PropertyType::Bool:
        // The schema writes its Bool defaults as 0 and 1.
        if (text == "0" || text == "1") {
            value = text == "1";
        }
        break;
    case PropertyType::Float32:
        if (const std::optional<float> number = parseNumber<float>(text)) {
            value = *number;
        }
        break;
    case PropertyType::Float64:
        if (const std::optional<double> number = parseNumber<double>(text)) {
            value = *number;
        }
        break;
    case PropertyType::StringNarrow:
    case PropertyType::StringWide:
        value = std::string(text);
        break;
    default:
        if (isUnsigned(type)) {
            if (const std::optional<std::uint64_t> number = parseNumber<std::uint64_t>(text)) {
                value = *number;
            }
        } else if (const std::optional<std::int64_t> number = parseNumber<std::int64_t>(text)) {
            value = *number;
        }
        break;
    }
    if (value && !valueFits(type, *value)) {
        return std::nullopt;
    }
    return value;
}

std::string formatValue(const PropertyValue& value)
{
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? "1" : "0";
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return {};
    }
    // std::format gives the shortest text that reads back to the same
    // number, for floats as for integers.
    return std::visit([](const auto& number) -> std::string {
        if constexpr (std::is_arithmetic_v<std::decay_t<decltype(number)>>) {
            return std::format("{}", number);
        } else {
            return {};
        }
    }, value);
}

const Property* Property::child(std::string_view childName) const
{
    for (const Property& property : children) {
        if (property.name == childName) {
            return &property;
        }
    }
    return nullptr;
}

Property* Property::child(std::string_view childName)
{
    for (Property& property : children) {
        if (property.name == childName) {
            return &property;
        }
    }
    return nullptr;
}

ProfileSchema ProfileSchema::parse(std::string_view xml)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", kPath);
    }
    const pugi::xml_node root = document.child("Profile");
    if (!root) {
        throw DataError("{}: no Profile element", kPath);
    }
    ProfileSchema schema;
    std::unordered_set<std::string> names;
    for (const pugi::xml_node node : root.children("Property")) {
        Property property = parseProperty(node, "Profile");
        if (property.type != PropertyType::PropertyBag) {
            throw DataError("{}: {} lies outside a bag", kPath, property.name);
        }
        if (!names.insert(property.name).second) {
            throw DataError("{}: bag {} is defined twice", kPath, property.name);
        }
        schema.m_bags.push_back(std::move(property));
    }
    return schema;
}

ProfileSchema ProfileSchema::load(const Vfs& vfs)
{
    const Bytes data = vfs.readOrThrow(kPath);
    return parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

const Property* ProfileSchema::find(std::string_view path) const
{
    return findProperty(m_bags, path);
}

std::size_t ProfileSchema::valueCount() const
{
    return countValues(m_bags);
}

const Property* findProperty(const std::vector<Property>& bags, std::string_view path)
{
    const std::vector<Property>* level = &bags;
    const Property* found = nullptr;
    while (!path.empty()) {
        const std::size_t slash = path.find('/');
        const std::string_view name = path.substr(0, slash);
        found = nullptr;
        for (const Property& property : *level) {
            if (property.name == name) {
                found = &property;
                break;
            }
        }
        if (found == nullptr) {
            return nullptr;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        level = &found->children;
        path.remove_prefix(slash + 1);
    }
    return found;
}

Property* findProperty(std::vector<Property>& bags, std::string_view path)
{
    return const_cast<Property*>(findProperty(static_cast<const std::vector<Property>&>(bags), path));
}

} // namespace fh::profile
