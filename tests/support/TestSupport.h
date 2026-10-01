#pragma once

namespace openhorizon::test {

/// The extracted disc OPENHORIZON_GAME_DIR names; skips the test case when unset.
const char* gameDir();

/// Runs the Catch2 session with the game's error boxes closed and SDL's
/// offscreen video and dummy audio, unless the environment names drivers.
int runTests(int argc, char** argv);

} // namespace openhorizon::test
