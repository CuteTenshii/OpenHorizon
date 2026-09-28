#pragma once

#include <format>
#include <stdexcept>
#include <string>

namespace fh {

/// A file or piece of game data that could not be read or does not have the
/// layout the reader expects.
class DataError : public std::runtime_error {
public:
    explicit DataError(const std::string& message)
        : std::runtime_error(message)
    {
    }

    template <typename... Args>
    explicit DataError(std::format_string<Args...> format, Args&&... args)
        : std::runtime_error(std::format(format, std::forward<Args>(args)...))
    {
    }
};

} // namespace fh
