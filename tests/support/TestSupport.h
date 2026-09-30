#pragma once

namespace openhorizon::test {

/// The extracted disc FH1_GAME_DIR names. When it names none, skips the
/// test case that asks: the tests that need the disc run only where it is.
const char* gameDir();

/// Runs a test program's Catch2 session on its command line and returns
/// the exit code. Keeps every error box of the game closed first, and
/// picks SDL's offscreen video and dummy audio drivers unless the
/// environment names others.
int runTests(int argc, char** argv);

} // namespace openhorizon::test
