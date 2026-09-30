# Contributing

Thanks for looking into this. The project rebuilds Forza Horizon (Xbox 360, 2012) as a native program for Linux and Windows, reading the game's own files from your copy of the disc. Most of the work is finding out what the original game does, then writing that down in code.

## The three rules

1. **Write new code.** The engine, the physics and the game rules are written here. Recompiling or emulating the Xbox 360 executable (XenonRecomp and similar tools) is out of scope.
2. **Commit no game data.** Nothing extracted from the disc goes into the repository, not even a small sample for a test. Tests and tools read the disc from wherever you keep it.
3. **Guess nothing.** Every value, formula, asset and rule comes from the game's data or its code. If you can't find where the game gets something, leave it out and write down what is missing. A number that merely looks right is a bug that hides.

The third rule shapes most reviews. "It looks like the game" isn't enough; "the game reads this from `TrackSettings.xml`" or "the executable does this at `0x82443D50`" is.

## Getting started

You need:

- the build dependencies listed in the [README](README.md#build), for Linux or Windows;
- an extracted copy of the disc;
- optionally, [Lefthook](https://lefthook.dev) for the git hooks.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/src/fh1 --game /path/to/extracted/disc   # the path is remembered afterwards
lefthook install                                 # once, to turn on the hooks
```

## Finding out what the game does

Start from the data. The disc holds far more than the renderer needs: scenes for every screen, XML for time of day, tracks, effects and game modes, and a SQLite database (`media/db/gamedb.slt`) with every car, event and unlock. The tools in the [README](README.md#tools) read these the way the engine does. `fh1vfs DISC list media/ui` is a good first step when you're looking for something.

When the data isn't enough, read the executable. You need a decrypted, flat image of it (big-endian PowerPC, loaded at `0x82000000`) and a PowerPC disassembler such as `powerpc-linux-gnu-objdump`. Searching the image for a file name or a string you've seen in the data, then following the code that loads its address, finds most loaders.

Write down what you find where the code uses it: a `///` comment that says what you verified and against what (a file, a function address, how many files you checked), and what you inferred and why. The next person, possibly you in six months, will rely on that distinction.

## Fixing a bug

Reproduce it in a test before fixing it. The test should fail on the current code and pass with your fix. There are three kinds:

- **Unit tests** (`tst_world`, `tst_audio`, `tst_ui` and the rest, one per module) check formats and logic, with small hand-built data and, when the disc is available, the real files.
- **Car checks** (`tst_car`) put real cars on a flat or tilted plane to test how they drive. If you change anything in `src/car`, also run `fh1cartest DISC all`, which compares acceleration, braking, grip and top speed with the figures the game recorded; its medians shouldn't get worse.
- **Scenarios** (`tst_scenarios`) run the whole game at a fixed frame rate with scripted controls: a start point, a race or a time of day, then throttle, steering and button presses. They check the game's own state. Most player-reported bugs end up here.

The tests run the game offscreen: no window opens and no sound plays.

A crash report's log ends with a backtrace. On Linux each frame reads `fh1(+0x1a2b3)`; `addr2line -f -C -e build/src/fh1 0x1a2b3` turns it into a function and a line, given the same build. On Windows the log already names them, and the minidump `fh1.dmp` opens in Visual Studio.

```sh
scripts/run-tests.sh                          # configure, build, run everything except the slow tests
scripts/run-tests.sh build -R tst_scenarios   # one test program
scripts/run-tests.sh build -L slow            # checks that go through every file of a kind on the disc
scripts/capture.sh shots/ pause               # run the scenarios matching "pause" and keep their screenshots
```

Tests that need the disc skip themselves unless `FH1_GAME_DIR` points to it. The scripts also find the path you gave the game.

## Writing code

- C++20, formatted with the repository's `.clang-format`. Much of the code predates that file, so format the lines you change, not whole files.
- No compiler warnings, and clean under `.clang-tidy`. If a check is wrong for this code, turn it off in `.clang-tidy` and say why.
- The code builds on Linux and Windows. Anything that differs between the two lives in `src/base/Host`. The README explains the Windows build.
- Comments say why, not what. Don't leave TODOs: finish the piece, or leave it out and note what's missing in the code and in [TODO.md](TODO.md).
- Text the player sees comes from the game's string tables whenever the game has it.
- Units come from the data and are converted once, when loading, with the evidence for the conversion next to it. The database, for example, stores weight in hundreds of kilograms and springs in newtons per millimetre.
- The game's world is left-handed: see [Coordinates](README.md#coordinates) in the README.

## Commits and pull requests

Keep one change per commit, and use the message to explain why it's needed and what it rests on, including anything still unknown. A short scoped subject reads well in the log, for example `fix(car): hold a stopped car on the handbrake`.

Every commit should build and pass the tests on its own. With Lefthook installed, the commit hook checks the formatting of the lines you're committing, and the push hook builds and runs the tests except the scenarios, the performance test and the slow tests. Run those with the scripts above when your change could affect them.

GitHub builds and tests every push and pull request, but without the game's files, so the tests that need the disc only run on your machine. Say in the pull request which of them you ran.

## What to work on

[TODO.md](TODO.md) lists what is being worked on, what comes next and what hasn't started, with what is already known about each item. Pick something, or fix whatever bothers you most while playing.
