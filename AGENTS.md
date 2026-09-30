# AGENTS.md

This file guides coding agents (and people) working in this repository.

## What this is

A from-scratch native reimplementation of Forza Horizon (Xbox 360, 2012) in C++20, for Linux (x86-64) and Windows (x64, Visual Studio with vcpkg). It reads the game's own files from an extracted disc at runtime. Recompiling or emulating the Xbox 360 executable (XenonRecomp and similar tools) is out of scope by design: engine, simulation and rules are new code, informed by the game's data and by reading its code. The repository holds no game data.

Knowledge sources outside the repo:
- The extracted disc: pass it with `--game`, or set `FH1_GAME_DIR`; the game remembers it in its settings file.
- A flat, decrypted big-endian PPC image of the executable, base `0x82000000`. Disassemble ranges with `powerpc-linux-gnu-objdump -D -b binary -m powerpc:common -EB --adjust-vma=0x82000000 --start-address=A --stop-address=B IMAGE`. String tables and code references (lis/addi pairs) locate loaders.
- The author's reverse-engineering notes on maps and paths.
- forza-map-manager, the author's Qt map viewer, whose README "Format notes" document many track formats; `src/world` is a Qt-free port of its readers.

## Commands

```sh
cmake -S . -B build -G Ninja            # the game only; RelWithDebInfo by default; fetches lzx and xds on first configure
cmake -S . -B build -DFH1N_BUILD_TESTS=ON -DFH1N_BUILD_TOOLS=ON   # tests and tools, off by default
cmake --build build                      # everything configured
cmake --build build --target fh1         # just the game
ctest --test-dir build -j 8              # unit tests, a process per test case
FH1_GAME_DIR=/path/to/disc ctest --test-dir build -j 8   # also the checks against the real disc
ctest --test-dir build -R tst_collision  # one test program (ctest names cases tst_collision.<case>)
FH1_GAME_DIR=/path/to/disc ctest --test-dir build -L slow   # the slow sweeps over every sample and shader of the disc
build/tests/tst_ui "*font*"              # a program's cases whose names match, run directly
cmake --build build --target format      # clang-format every source (.clang-format)
cmake --build build --target format-check
cmake --build build --target tidy        # clang-tidy over the compilation database (.clang-tidy)
```

Windows builds use Visual Studio (MSVC) with dependencies from `vcpkg.json` (set `VCPKG_ROOT`): `cmake --preset windows-release`. MSVC can't run on this Linux machine: CI's Windows job builds and tests it on every push, so push and read that job's result to check Windows-specific changes.

`scripts/run-tests.sh` and `scripts/capture.sh` turn the tests on; CI builds the tools too.

`lzx` and `xds` come from their git repos via FetchContent, pinned by commit in the root `CMakeLists.txt`. To work against local checkouts use `-DFETCHCONTENT_SOURCE_DIR_LZX=<path>` / `-DFETCHCONTENT_SOURCE_DIR_XDS=<path>`.

Tests use Catch2 v3: one program per module (`tests/tst_<module>.cpp`), added in `tests/CMakeLists.txt` with `fh1n_add_test_program`, which registers every `TEST_CASE` with ctest as `<program>.<case>` through `catch_discover_tests`. `tests/support` holds what the programs share: the `main()` (which sets `FH1_NO_CRASH_DIALOG` and SDL's offscreen and dummy drivers), `fh::test::gameDir()`, which `SKIP`s a case when `FH1_GAME_DIR` is not set, and `fh::test::requireGlContext()`, which skips without an OpenGL context. Setup a program needs before its cases (a cache or data folder under the temporary directory) goes in a Catch2 event listener in its file. A sweep over every file of a kind on the disc is tagged `[slow][.]`: ctest gives it the label `slow`, which the default runs leave out, next to a fast check of a few files chosen for their variety. The cases of one program run one at a time (they share temporary folders); different programs run in parallel. Lefthook (`lefthook.yml`, `lefthook install` once) checks the formatting of staged lines before a commit; CI builds and runs the unit tests on every push; the scenarios, the perf test and the slow tests run with `scripts/run-tests.sh`. See CONTRIBUTING.md.

### Running the game when testing

Never open windows on the desktop or play through the speakers. The game itself has no test options; behaviour is tested through scenarios, which run the whole game offscreen (SDL on the GPU through EGL, dummy audio) at a fixed 60 Hz with scripted controls, through `Game::frame(input, dt)`, and assert on its state:

```sh
scripts/run-tests.sh                        # configure, build, every test offscreen
scripts/run-tests.sh build -R tst_scenarios # just the scenarios
scripts/run-tests.sh build -L perf          # just the frame-time test
scripts/run-tests.sh build -L slow          # just the slow sweeps, which the other runs leave out
scripts/capture.sh OUT_DIR [FILTER]         # run scenarios and keep their screenshots
```

The game's error boxes (after a crash, or a startup error such as a missing disc folder) stay closed when `FH1_NO_CRASH_DIALOG` is set, when `SDL_VIDEO_DRIVER` is `offscreen` or `dummy`, and when there is no display. The scripts and the tests' `main()` (`tests/support`) set `FH1_NO_CRASH_DIALOG`; set it too when running `fh1` or a test binary any other way.

A player-reported bug becomes a scenario (or a car check in `tests/tst_car.cpp` when no world is needed) that fails before the fix. Scenarios live in `tests/scenarios`, one file per area (`Menus`, `Map`, `Driving`, `World`, `Audio`, `Races`, `Activities`, `Profile`, `Perf`), all built into `tst_scenarios`. Each is a `TEST_CASE_METHOD(GameScenario, "name", "[scenario][area]")` on the shared support in `tests/scenarios/Session.h`: the `GameScenario` fixture (skips without the disc or an OpenGL context, `disc()` for reading game files), the process's one `window()`, and `Session`, which starts a game and plays it (`waitForCar`, `run`, `runUntil`, `press`, `pressTimes`, `focusPauseItem`, `capture`), with `saveProfile`, `readScene`, `RoadFollower` and the game-state readers beside it. New scenarios use these and add what is missing there rather than helpers of their own; a helper only one area needs stays in that area's file. Scenarios set up through `GameOptions` (start point, race, time, autopilot) and read the game through its read-only accessors; `Session::capture` saves a screenshot when `FH1_CAPTURE_DIR` is set. A game takes about 3 GB, so ctest runs the scenarios one at a time (they hold the lock `game`) beside the unit tests; each case logs to `fh1-scenarios-logs/<case>.log` under the temporary directory. The game folder is remembered in `~/.config/fh1-native/settings.ini`, which the scripts read when `FH1_GAME_DIR` is not set.

### Tools (in `build/src/...`, built with `-DFH1N_BUILD_TOOLS=ON`)

- `fh1vfs DISC list|cat|extract` - the game's file system as the engine sees it; use it to inspect any game file.
- `fh1carspec DISC CAR|all` - a car's physics spec as read from gamedb.
- `fh1cartest DISC CAR|all [--trace]` - runs cars through the tests whose results the game recorded (`Data_Car.Sim*`) and prints ours against theirs. Run it over `all` after any change to `src/car`; the medians it reports are the car model's regression check.
- `world/fh1world`, `collision/fh1fiz`, `audio/fh1audio` - per-module inspection tools for world tiles, collision meshes and sound.

### Skills

Step-by-step procedures for recurring work live in `.agents/skills/<name>/SKILL.md`. Agents that support skills load them from there (`.claude/skills` points to the same folder); others should read the matching file before starting such work:

- `find-asset-evidence`: before using any asset, value, layout or rule from the game, find what the game itself uses and record what is verified, inferred or missing.

## Architecture

Libraries, bottom up (targets in `src/CMakeLists.txt` and per-folder `CMakeLists.txt`):

- `fh1base` (`src/base`, `src/vfs`): `Host` for everything that differs between Linux and Windows (per-user folders, environment, UTF-8 paths, positional reads, durable writes), logging with `std::format`, `BeReader` for bounds-checked big-endian reads (Xbox data is big-endian; FSB audio headers are the exception), `DataError`, a priority `JobPool`, settings, and the crash handler (`Crash`: signal handlers or the Windows exception filter, which log a backtrace and start `fh1 --crash-report=LOG` to show the message box outside the crashed process). `Vfs` mounts the disc's loose files plus every archive in `media/zipmanifest.xml` at its own path minus `.zip` (so `media/physics.zip` provides `media/physics/...`), matching paths case-insensitively with or without `game:\`.
- `fh1data` (`src/data`, `src/car`): SQLite access to `media/db/gamedb.slt`, the LSB2 string tables (database text like `_&202713995` resolves through `Localisation`), `CarSpec` (a car's physics gathered from about fifteen linked gamedb tables, converted to SI) and `CarPhysics` (360 Hz vehicle model).
- `fh1world` (`src/world`): track world formats (render models, PVS tables, hex zone grid, zone placements, procedural scatter sets, textures and shader vertex layouts), a `WorldIndex` of every drawable chunk, and `buildTileMesh`, which turns a list of chunks into GPU-ready interleaved geometry. `TrackWorld::load` ties them together.
- `fh1collision` (`src/collision`): the game's `.fiz` collision meshes (surface type per triangle, per-route barrier triangles) indexed by the track's `.col`, and `surfaceTypes.xml`.
- `fh1audio` (`src/audio`): FSB4 banks (XMA and MPEG through FFmpeg), FMOD `.fev` projects, a software mixer on SDL3, and `EngineSound`, driven by the per-car engine tunings.
- `fh1platform` (`src/platform`, `src/render/Gl.*`): SDL3 window, OpenGL 4.5 core context, a hand-written GL entry-point loader (`Gl.h` lists every function used; add new ones there), input mapped onto Xbox pad controls.
- `fh1animated` (`src/animated`): the animated world objects: kind-4 procedural sets (geometry and an embedded RAD Granny file of skeletons and animations, `Granny.h`), kind-5 sets that place them, and `AnimatedObjects`, which culls, plays and poses them on the presentation clock; `render/AnimatedObjectRenderer` draws them with ports of the executable's PROC_ANIM_OBJ shaders.
- `fh1engine` (`src/render`, `src/physics`, `src/game`): the game proper, run by `app/main.cpp`.

How a frame fits together (`game/Game.cpp`):
- `WorldRenderer` streams 500 m tiles on the `JobPool`: a tile holds every chunk whose distance band the camera could reach within `kPrefetchDistance`, whichever way it looks, and is rebuilt when that set changes; the new mesh replaces the old only once its textures are uploaded. Each frame it picks the chunks to draw one by one, as the game does: the camera's zone lists the chunk (PVS) and its 3D distance to the chunk lies in the chunk's band. The tiles' batches keep each chunk's index run (`world::ChunkRun`), drawn through indirect multi-draws. It also draws "dynamic" copies of chunks with a model matrix (knocked-over props) and can hide chunks (their tiles rebuild without them).
- `FrameRenderer` owns the scene target with a float depth buffer and reversed Z (infinite far plane, `glClipControl` zero-to-one), a sky pass, and the game's post-processing (`PostProcess`: exposure, bloom, filmic tone map, grading, vignette), driven by `game/PostProcessing` (TrackSettings.xml, the dynamicpost templates, the post-processing zones and the time-of-day multipliers). The scene target holds the square root of linear colour, as the game's does: every pass drawing into it writes that. The HUD and `Overlay` (debug lines, text) draw after the post-processing.
- `WorldCollision` implements `GroundQuery` for the cars: `.fiz` collision meshes first (per-surface grip and drag, walls, invisible barriers, the active race's barriers), render geometry for everything else (ground outside the drivable area, walls of props), and trunk cylinders for tall procedural trees. Chunks of breakable props are excluded from it; `Smashables` owns them as dynamic bodies.
- `CarPhysics::step` runs at 360 Hz inside `Game::stepPhysics`, followed per step by body-sphere contacts against the world, breakables and other cars.
- Races: `RaceData` (events from gamedb, `TrackRouteNNN.xml` routes, `.owt` racing lines), `Race` (countdown, progress along the line, standings), `AiDriver` (pure pursuit plus a curvature speed profile, also used to colour the player's driving line).

## Conventions that span files

- World space is the game's and is left-handed: x east, y up, z north. Car space: x right, y up, z forward, origin on the ground below the middle of the wheelbase. A positive rotation about y (as glm computes it) is a right turn. Render model files store Z negated; the parsers flip it. `Camera::view()` mirrors Z so OpenGL shows the world the right way round, and world triangles are wound so back-face culling works after that mirror.
- Units come from the data: gamedb stores curb weight in 100 kg, springs in N/mm, dampers in kN·s/m, peak power in 100 W; aero forces are pounds-force at 100 mph. `CarSpec` converts on load, and each such conversion is commented with its evidence.
- Code stays portable to Windows: OS-specific calls live in `src/base/Host.*` only (and the crash handler's in `src/base/Crash.*`), and text becomes a path through `fh::pathFromUtf8` / `fh::pathToUtf8` (never `path::string()` or a `std::string` constructor). Dependencies come from the system on Linux and from `vcpkg.json` on Windows; a new one goes in both, found as a CMake package first and through pkg-config second (see the root `CMakeLists.txt`).
- Doc comments (`///`) on formats say which facts were verified against the data and which are inferred. Keep that distinction when extending them. Any fitted or guessed constant says so in its comment.
- Comments explain why, not what; no decorative dividers, no TODO or placeholder comments. User-visible text avoids em dashes.
- Commits follow Conventional Commits with the area as scope (`feat(car): ...`, `fix(render): ...`), one concern per commit, body explaining why.
- Several agents often work at once, each in its own `src/<module>` folder and adding one `add_subdirectory(...)` line to `src/CMakeLists.txt`. When committing, stage only the lines for modules that are ready (e.g. via `git update-index --cacheinfo` with a filtered copy), so the committed tree always builds.
