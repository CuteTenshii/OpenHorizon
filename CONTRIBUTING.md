# Contributing

This is a native Linux reimplementation of Forza Horizon (Xbox 360, 2012). Before changing anything, keep three rules in mind.

1. **New code, the game's data.** The engine, the simulation and the rules are written here from scratch. Recompiling or emulating the Xbox 360 executable (XenonRecomp and similar) is out of scope. The game's own files are read at runtime from your copy of the disc.
2. **No game data in the repository.** Tests and tools read the disc from `FH1_GAME_DIR`; nothing extracted from it is committed.
3. **Nothing guessed.** Every asset use, formula and value comes from the game's data (scenes, XML, gamedb, curves) or from its code. When something cannot be found, leave it out and say so, in the code and in the commit. Do not fill the gap with a value that looks right.

## Getting set up

You need the build dependencies listed in the [README](README.md#build), an extracted copy of the disc, and optionally [Lefthook](https://lefthook.dev) for the git hooks.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/src/fh1 --game /path/to/extracted/disc   # remembered in ~/.config/fh1-native/settings.ini
lefthook install                                 # once, to enable the hooks
```

To work on the executable itself you need its decrypted, flat image (big-endian PowerPC, base `0x82000000`). AGENTS.md shows how to disassemble ranges of it; string references built with `lis`/`addi` pairs lead to most loaders. The author's notes on maps and paths and the `forza-map-manager` format notes cover many track formats.

## Evidence

- Doc comments (`///`) on formats and game rules say what was verified, and against what: the file, the function address, the count of files checked. They also say what is inferred and why.
- A constant that had to be fitted or chosen says so in its comment, with the reason. It should be the exception.
- Commit messages explain why the change is needed and what it rests on, including anything still unknown.

## Testing

Everything runs offscreen: never open a window on the desktop or play sound through the speakers while testing.

```sh
scripts/run-tests.sh                          # configure, build, every test (disc tests when FH1_GAME_DIR is set) but the slow ones
scripts/run-tests.sh build -R tst_scenarios   # one test executable
scripts/run-tests.sh build -L slow            # the slow tests: every sample and every shader on the disc
scripts/capture.sh shots/ pause               # run the scenarios matching "pause" and keep their screenshots
```

There are three kinds of test, all plain executables on `tests/TestHarness.h`:

- **Unit tests** (`tst_world`, `tst_audio`, `tst_ui` and others, one per module): formats and logic, with synthetic data and, when `FH1_GAME_DIR` is set, the real disc. A check of every file of a kind that takes more than a few seconds is a `SLOW_TEST`, left out of the default run and of the pre-push hook (`scripts/run-tests.sh build -L slow` runs them); keep beside it a fast check of a few files chosen for their variety, and say in a comment why those.
- **Car checks** (`tst_car`): the real cars on a flat or tilted plane, for how they drive. `fh1cartest DISC all` compares the performance figures with the ones the game recorded; run it after any change to `src/car` and do not let its medians get worse without a reason from the game's code.
- **Scenarios** (`tst_scenarios`): the whole game, offscreen at a fixed 60 Hz, driven by scripted controls through `Game::frame(input, dt)` and checked through the game's own state. A `Session` sets the game up through `GameOptions` (start point, race, time of day), holds controls for a while (`run(seconds, {.throttle = 1.0F, .steer = -0.3F})`), presses buttons (`press(Button::Start)`) and asserts.

When a player reports a bug, first write the scenario or car check that shows it failing, then fix it. The game has no command-line options for testing; if a test needs to set something up or read something out, add it to `GameOptions` or to the game's read-only accessors.

## Code

- C++20, the style in `.clang-format`. The pre-commit hook checks the lines you are committing (`git clang-format --staged`); much of the code predates the file, so do not reformat whole files in unrelated changes.
- Zero compiler warnings, and clean under `.clang-tidy` (`cmake --build build --target tidy`). If a check is wrong for this code, switch it off in `.clang-tidy` with the reason, rather than suppressing it line by line.
- Comments explain why, not what. No TODOs or placeholders: write the whole thing, or leave it out and document what is missing.
- User-visible text comes from the game's string tables wherever the game has it, and avoids em dashes otherwise.
- Units come from the data and are converted once, on load, with the evidence for the conversion in a comment (gamedb stores curb weight in 100 kg, springs in N/mm, and so on).
- World space is the game's, left-handed: x east, y up, z north. Car space is x right, y up, z forward.

## Layout

Each area is a library in its own `src/` folder with its own `CMakeLists.txt`, test executable and, often, an inspection tool in `tools/`. AGENTS.md describes how they fit together and how a frame is drawn. When several people or agents work at once, each keeps to its own folder; shared files (`src/CMakeLists.txt`, `tests/CMakeLists.txt`, `src/game/Game.cpp`) are edited in place, never rewritten from an older copy.

## Commits

One concern per commit, with a message that says why. A short scoped subject such as `fix(car): hold a stopped car on the handbrake` reads well in the log. Every commit should build and pass the tests on its own; the pre-push hook builds and runs everything except the scenarios and the slow tests.

## What to work on

[TODO.md](TODO.md) lists what is in progress, what comes next and what has not been started, with what is known about each item.
