#include "Profile.h"

namespace fh::profile {

Profile::Profile(const ProfileSchema& schema)
    : m_bags(schema.bags())
{
}

std::optional<PropertyValue> Profile::value(std::string_view path) const
{
    const Property* property = find(path);
    if (property == nullptr || property->type == PropertyType::PropertyBag) {
        return std::nullopt;
    }
    return property->value;
}

bool Profile::set(std::string_view path, const PropertyValue& value)
{
    Property* property = findProperty(m_bags, path);
    if (property == nullptr || property->type == PropertyType::PropertyBag || !valueFits(property->type, value)) {
        return false;
    }
    property->value = value;
    return true;
}

} // namespace fh::profile
