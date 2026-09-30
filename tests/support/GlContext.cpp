#include "support/GlContext.h"

#include "platform/Window.h"

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <string>

namespace openhorizon::test {

Window& requireGlContext()
{
    static std::string error;
    static const std::unique_ptr<Window> window = [] {
        WindowOptions options;
        options.width = 64;
        options.height = 64;
        options.hidden = true;
        options.vsync = false;
        try {
            return std::make_unique<Window>(options);
        } catch (const std::exception& e) {
            error = e.what();
            return std::unique_ptr<Window>();
        }
    }();
    if (!window) {
        SKIP("no OpenGL context (" << error << ")");
    }
    return *window;
}

} // namespace openhorizon::test
