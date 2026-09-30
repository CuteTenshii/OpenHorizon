#include "support/TestSupport.h"

#include "base/Crash.h"
#include "base/Host.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

namespace fh::test {

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        SKIP("FH1_GAME_DIR is not set");
    }
    return dir;
}

int runTests(int argc, char** argv)
{
    // In this process and the ones it starts: a crash under test must end
    // the run, not wait on a click.
    setEnvironmentVariable(kNoCrashDialogVariable, "1");
    // Never a window on the desktop nor a sound through the speakers,
    // unless the caller asked for a driver.
    setEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen", false);
    setEnvironmentVariable("SDL_AUDIO_DRIVER", "dummy", false);
    return Catch::Session().run(argc, argv);
}

} // namespace fh::test
