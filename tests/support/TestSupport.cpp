#include "support/TestSupport.h"

#include "base/Crash.h"
#include "base/Host.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

namespace openhorizon::test {

const char* gameDir()
{
    const char* dir = std::getenv("OPENHORIZON_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("OPENHORIZON_GAME_DIR is not set");
    }
    return dir;
}

int runTests(int argc, char** argv)
{
    // Inherited by child processes: a crash under test must not wait on a click.
    setEnvironmentVariable(kNoCrashDialogVariable, "1");
    // No desktop window or speaker sound unless the caller picked a driver.
    setEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen", false);
    setEnvironmentVariable("SDL_AUDIO_DRIVER", "dummy", false);
    return Catch::Session().run(argc, argv);
}

} // namespace openhorizon::test
