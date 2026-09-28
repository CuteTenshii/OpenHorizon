#include "Bytes.h"

#include <algorithm>
#include <cmath>

namespace fh {

float halfToFloat(std::uint16_t half)
{
    const std::uint32_t sign = (half >> 15) & 1U;
    const std::uint32_t exponent = (half >> 10) & 0x1FU;
    const std::uint32_t mantissa = half & 0x3FFU;
    float value = 0.0F;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa == 0 ? INFINITY : NAN;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

std::string toLower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = static_cast<char>(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = static_cast<char>(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

bool endsWithNoCase(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && iequals(text.substr(text.size() - suffix.size()), suffix);
}

bool startsWithNoCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

} // namespace fh
