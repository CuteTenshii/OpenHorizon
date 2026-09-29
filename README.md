# fh1-native-x64

A native reimplementation of Forza Horizon (Xbox 360, 2012) for Linux and Windows, written in C++20 from scratch. It reads the game's own files from your copy of the disc at runtime. It does not recompile or emulate the Xbox 360 executable: the engine, the car simulation and the game rules are new code, built from the game's data and from studying how the original uses it.

The repository contains no game data.

## What works

| Area | State |
|------|-------|
| Files | The disc's 524 archives mount as the game mounts them (`media/zipmanifest.xml`), with case-insensitive `game:\` paths; LZX archives through [lzx](https://git.tenshii.moe/Tenshii/lzx). |
| World | Colorado streams in 500 m tiles with level of detail by distance, drawn with the game's own textures (through [xds](https://git.tenshii.moe/Tenshii/xds)): terrain with its splat-blended layers, roads, towns, props and about 128,600 procedural trees, bushes and rocks. |
| Lighting | Sun, sky and haze follow the track's time-of-day curves (`TimeOfDayA.xml`), on a clock that starts at 16:00 and runs 48 times real time, as free roam does. The filmic tone curve uses the game's parameters. |
| Cars | All 176 drivable cars load their physics from `gamedb.slt`: torque curves, gearboxes, clutches, differentials, tyre compounds and their friction curves, springs, dampers, anti-roll bars and aerodynamics. |
| Driving | A 360 Hz vehicle simulation. Against the performance figures the game itself recorded for every car, the median error is 2.2% for cornering grip, 3.2% for the quarter mile, 4.4% for braking and 5.5% for top speed (see `fh1cartest`). ABS, traction control, stability control and the automatic gearbox are available. |
| Collision | The ground comes from the finest render geometry near the car, trees have trunks, and cars collide with the world and each other. |
| Races | The 118 races of Colorado from the game database, with their real names, routes, grids, opponents and AI skill. AI drivers follow the game's racing lines. Countdown, positions, laps, a driving line coloured by braking need, and results. |

## Not done yet

- Car models: cars are drawn as outlines until the `.carbin` format is decoded.
- Sound: engine and world audio (FMOD banks with XMA samples).
- The game's collision meshes (`.fiz`), which carry a surface type per triangle; until then every surface grips like tarmac.
- Menus and HUD from the game's own UI files (`.bgf`, `.bsg`, `.fbf`); a plain HUD stands in.
- Traffic, the career and its progression, festival and showcase events, barn finds, speed traps, upgrades and tuning, photo mode, saving, and online play.

## Build

### Linux

Requires CMake 3.21 or newer, a C++20 compiler (GCC 13 or Clang 17), and the development packages of SDL3, OpenGL (with `GL/glcorearb.h`), glm, pugixml, SQLite 3, FreeType, fontconfig, FFmpeg (libavcodec, libavformat, libavutil), zlib and shaderc (used only by the shader check tool and its test). The first configure downloads lzx and xds; `-DFETCHCONTENT_SOURCE_DIR_LZX=<path>` and `-DFETCHCONTENT_SOURCE_DIR_XDS=<path>` build against local checkouts instead.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
FH1_GAME_DIR=/path/to/extracted/disc ctest --test-dir build   # also the checks against real data
scripts/run-tests.sh                                           # all of it offscreen, scenarios included
```

### Windows

Requires Visual Studio 2022 or later (the "Desktop development with C++" workload, which brings CMake and Ninja) and [vcpkg](https://github.com/microsoft/vcpkg). The dependencies are listed in `vcpkg.json` and pinned by its `builtin-baseline`; vcpkg builds them on the first configure, which takes a while (FFmpeg and shaderc are the long ones).

1. Clone vcpkg and bootstrap it: `git clone https://github.com/microsoft/vcpkg C:\vcpkg`, then `C:\vcpkg\bootstrap-vcpkg.bat`.
2. Set `VCPKG_ROOT` to that folder (`setx VCPKG_ROOT C:\vcpkg`, then open a new terminal).
3. From a Visual Studio "Developer PowerShell":

```powershell
cmake --preset windows-release        # or windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
```

Visual Studio can also open the folder directly and offers the same presets. The executables land in `build-windows\src` with the DLLs they need copied next to them.

Contributing, testing and the git hooks: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Run

```sh
./build/src/fh1 --game /path/to/extracted/disc
```

On Windows, `build-windows\src\fh1.exe --game D:\path\to\extracted\disc`.

The folder is the disc root (holding `default.xex` and `media`) or the `media` folder itself. It is remembered in the settings file (see below), so later starts need no options. The other options are the window size (`--width`, `--height`, `--fullscreen`) and `--log`. Choices without a menu yet are settings in the same file: `car = ALF_8C_08` (a media name), `manual = true` for a manual gearbox, `metric = false` for miles per hour, `debug = true` for a verbose console.

Each run writes its full log, debug messages included, to `fh1.log`, replacing the previous one; `--log FILE` writes it elsewhere. Attach it to bug reports.

Where the game keeps its files:

| File | Linux | Windows |
|------|-------|---------|
| Settings (`settings.ini`) | `~/.config/fh1-native` (`$XDG_CONFIG_HOME`) | `%APPDATA%\fh1-native` |
| Saved profile (`profile`) | `~/.local/share/fh1-native` (`$XDG_DATA_HOME`) | `%LOCALAPPDATA%\fh1-native` |
| Log (`fh1.log`) | `~/.local/state/fh1-native` (`$XDG_STATE_HOME`) | `%LOCALAPPDATA%\fh1-native\logs` |
| Shader cache (`shaders`) | `~/.cache/fh1-native` (`$XDG_CACHE_HOME`) | `%LOCALAPPDATA%\fh1-native\cache` |

### Controls

The pad follows the game's default controller layout.

| Action | Keyboard | Xbox pad |
|--------|----------|----------|
| Throttle, brake | W, S (or arrows) | Right, left trigger |
| Steer | A, D (or arrows) | Left stick |
| Handbrake | Space | A |
| Shift up, down (manual) | E, Q | B, X |
| Start an event at its start point | Enter | X |
| Change the view | C | RB |
| Look around, look back | Right mouse button and mouse, V | Right stick, right stick click |
| Radio station | Page Up, Page Down | D-pad left, right |
| Pause menu | Escape | Start |
| Map | M | Back |
| Menus | Arrows, Enter, Escape | D-pad or left stick, A, B |
| Map cursor, zoom | W, A, S, D, then Q and E | Left stick, right stick |
| Free camera | F, then mouse and WASD, Q and E | |
| Reset the car | R | |
| Debug statistics | Tab or F3 | |
| Fullscreen | F11 | |

## Tools

| Tool | Does |
|------|------|
| `fh1vfs DISC list FOLDER`, `cat PATH`, `extract PATH FILE` | The game's file system as the engine sees it. |
| `fh1carspec DISC CAR` or `all` | The physics specification read for a car. |
| `fh1cartest DISC CAR` or `all` | Runs cars through the game's performance tests and compares with its recorded results. |
| `fh1world DISC TRACK` | Loads a track's world index, tile meshes and textures, and reports counts and timings. |

## Layout

| Folder | Contents |
|--------|----------|
| `src/base` | Logging, big-endian reading, the job pool, settings. |
| `src/vfs` | The game's file system over the disc and its archives. |
| `src/data` | The game database and string tables. |
| `src/world` | Track world formats: render models, zones, placements, procedural sets, textures, tile meshes. |
| `src/car` | Car specifications from the database, and the vehicle simulation. |
| `src/physics` | Collision with the world. |
| `src/render` | OpenGL 4.5 renderer: world, sky, tonemapping, overlays. |
| `src/game` | Free roam, races, AI drivers, time of day. |
| `src/platform` | Window, OpenGL context and input (SDL3). |
| `tools`, `tests` | Command-line tools and tests. |

## Coordinates

The game's world is left-handed: x east, y up, z north. Car space is x right, y up, z forward, with the origin on the ground below the middle of the wheelbase, as the car files use. The renderer mirrors z in its view matrix so OpenGL shows the world the right way round.

## License

MIT for the code in this repository. Forza Horizon and its data belong to their owners; you need your own copy of the game.
