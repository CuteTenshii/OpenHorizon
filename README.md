# fh1-native-x64

A native reimplementation of Forza Horizon (Xbox 360, 2012) for Linux and Windows, written in C++20 from scratch. It reads the game's own files from your copy of the disc at runtime. It does not recompile or emulate the Xbox 360 executable: the engine, the car simulation and the game rules are new code, built from the game's data and from studying how the original uses it.

The repository contains no game data.

## What works

| Area | State |
|------|-------|
| Files | The disc's archives mount as the game mounts them (`media/zipmanifest.xml`), with case-insensitive `game:\` paths; LZX archives through [lzx](https://git.tenshii.moe/Tenshii/lzx), textures through [xds](https://git.tenshii.moe/Tenshii/xds). |
| World | Colorado streams around the car, and each model and prop is drawn by the game's own rule: the camera's zone lists it and its distance lies in its level-of-detail band. The track's surfaces, props and procedural trees, bushes and rocks are drawn with the game's own shaders, translated from the Xbox 360's GPU code. Animated objects (wind turbines, fairground rides, balloons, farm machines) play, and the festival and race crowds stand and cheer. |
| Lighting | Sun, moon, stars, sky and clouds follow the track's time-of-day curves. Sun shadows use the game's cached levels and near cascades. The game's post-processing runs: adaptive exposure, bloom, filmic tone curve, grading tables, vignettes and the post-processing zones. |
| Cars | All 176 drivable cars load their physics from `gamedb.slt` and their models from the disc, drawn with the game's car shaders and reflecting a live environment cube map. |
| Driving | A 360 Hz vehicle simulation, steered through the game's own stick shaping. Against the performance figures the game recorded for every car, the median errors are 5.0% for cornering grip, 5.4% for the quarter mile, 6.1% for braking and 8.2% for top speed (see `fh1cartest`). |
| Collision | The game's collision meshes, with a surface type per triangle, plus walls, invisible barriers and the race's barriers. Cars hit the world and each other as the game's contact response does, and breakable props come apart. |
| Sound | Engines, tyres, impacts and scrapes, smashed props, the speed trap shutter, menu sounds and the radio stations, mixed through the game's snapshots (pause, loading, race, crash). |
| Menus and HUD | Drawn from the game's own UI scenes: the title screen, loading screens with tips, the HUD with its minimap and rev dial, the pause menu, and the full map with roads coloured by type, discovered roads, routes, the sat-nav and the player bar. |
| Free roam | Traffic and festival drivers, speed traps, Horizon outposts to discover, discount signs to smash, and event starts with their activation prompts. Progress is saved to a profile. |
| Races | Colorado's races from the game database, with their real names, routes, grids, opponents and AI skill: countdown, positions, laps, the finish, and a driving line coloured by braking need. |
| Stability | A crash writes a backtrace to the log and shows where the log is; startup problems (no game folder, no OpenGL 4.5) explain what to fix. |

What is missing, in progress or planned is in [TODO.md](TODO.md).

## Build

### Windows

Requires Visual Studio 2022 or later (the "Desktop development with C++" workload, which brings CMake and Ninja) and [vcpkg](https://github.com/microsoft/vcpkg). The dependencies are listed in `vcpkg.json` and pinned by its `builtin-baseline`; vcpkg builds them on the first configure, which takes a while (FFmpeg and shaderc are the long ones).

1. Clone vcpkg and bootstrap it: `git clone https://github.com/microsoft/vcpkg C:\vcpkg`, then `C:\vcpkg\bootstrap-vcpkg.bat`.
2. Set `VCPKG_ROOT` to that folder (`setx VCPKG_ROOT C:\vcpkg`, then open a new terminal).
3. From a Visual Studio "Developer PowerShell":

```powershell
cmake --preset windows-release        # or windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
$env:FH1_GAME_DIR = "D:\path\to\extracted\disc"; ctest --preset windows-release   # also the checks against real data
```

The first configure also downloads lzx and xds with git, so git must be on the path (the Visual Studio installer offers Git for Windows as a component). Visual Studio can also open the folder directly and offers the same presets. The executables land in `build-windows\src` with the DLLs they need copied next to them.

### Linux

Requires CMake 3.21 or newer, Ninja, git, a C++20 compiler with `std::format` (GCC 13 or Clang 17 and newer), and the development packages of SDL3, OpenGL (with `GL/glcorearb.h`) and EGL, glm, pugixml, SQLite 3, FreeType, fontconfig, FFmpeg (libavcodec, libavformat, libavutil), zlib and shaderc (used only by the shader check tool and its test). SDL3 is packaged from Ubuntu 25.04 on; on Ubuntu 26.04, as CI uses:

```sh
sudo apt-get install cmake ninja-build g++ pkg-config git libsdl3-dev libgl-dev libegl-dev \
  libglm-dev libpugixml-dev libsqlite3-dev libfreetype-dev libfontconfig-dev \
  libavcodec-dev libavformat-dev libavutil-dev zlib1g-dev libshaderc-dev
```

The first configure downloads lzx and xds; `-DFETCHCONTENT_SOURCE_DIR_LZX=<path>` and `-DFETCHCONTENT_SOURCE_DIR_XDS=<path>` build against local checkouts instead.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build
FH1_GAME_DIR=/path/to/extracted/disc ctest --test-dir build   # also the checks against real data
scripts/run-tests.sh                                           # all of it offscreen, scenarios included
```

Contributing, testing and the git hooks: see [CONTRIBUTING.md](CONTRIBUTING.md).

## Run

```sh
./build/src/fh1 --game /path/to/extracted/disc
```

On Windows, `build-windows\src\fh1.exe --game D:\path\to\extracted\disc`.

The folder is the disc root (holding `default.xex` and `media`) or the `media` folder itself. It is remembered in the settings file (see below), so later starts need no options. The other options are the window size (`--width`, `--height`, `--fullscreen`) and `--log`. Choices without a menu yet are settings in the same file: `car = ALF_8C_08` (a media name), `manual = true` for a manual gearbox, `metric = false` for miles per hour, `debug = true` for a verbose console.

Each run writes its full log, debug messages included, to `fh1.log`, replacing the previous one; `--log FILE` writes it elsewhere. Attach it to bug reports. If the game crashes, the log ends with what went wrong and a backtrace, and a message box says where the log is. On Windows the game also saves a minidump, `fh1.dmp`, next to the log: attach both.

Where the game keeps its files:

| File | Linux | Windows |
|------|-------|---------|
| Settings (`settings.ini`) | `~/.config/fh1-native` (`$XDG_CONFIG_HOME`) | `%APPDATA%\fh1-native` |
| Saved profile (`profile`) | `~/.local/share/fh1-native` (`$XDG_DATA_HOME`) | `%LOCALAPPDATA%\fh1-native` |
| Log (`fh1.log`) | `~/.local/state/fh1-native` (`$XDG_STATE_HOME`) | `%LOCALAPPDATA%\fh1-native\logs` |
| Crash dump (`fh1.dmp`) | none: the system keeps core dumps (`coredumpctl`) | `%LOCALAPPDATA%\fh1-native\logs` |
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

The tools are built next to the game, in `build/src` and its module folders.

| Tool | Does |
|------|------|
| `fh1vfs` | The game's file system as the engine sees it: list, print or extract any file. |
| `fh1carspec` | The physics specification read for a car, or for all of them. |
| `fh1cartest` | Runs cars through the game's performance tests and compares with its recorded results. |
| `fh1carmodel` | Loads a car's models and reports parts, materials, textures and wheels; exports OBJ. |
| `fh1world` | Loads a track's world as the streamer does; `--sizes` measures the whole map. |
| `fh1fiz` | The track's collision meshes: statistics, surfaces at a point, OBJ export. |
| `fh1shader` | Lists, disassembles and translates the game's compiled shaders. |
| `fh1particles` | The game's particle effects and the tyre effects per surface, with offscreen renders. |
| `fh1crowd` | A track's crowd: what was read, who stands where, checks against the ground. |
| `fh1audio` | Sound banks and FMOD events; renders a car's engine and tyre sounds. |
| `fh1radio` | The radio's stations, tracks and DJ lines; renders a station offline. |
| `fh1ui` | UI fonts, scene trees and textures; renders the HUD for sample states. |
| `fh1menu` | A scene's contracts and handlers; renders the menus, map and loading screen. |

## Layout

| Folder | Contents |
|--------|----------|
| `src/base` | Platform layer (`Host`), logging, big-endian reading, the job pool, settings, the crash handler. |
| `src/vfs` | The game's file system over the disc and its archives. |
| `src/data` | The game database and string tables. |
| `src/world` | Track world formats: render models, zones, placements, procedural sets, textures, tile meshes. |
| `src/collision`, `src/physics` | The game's collision meshes and surfaces, and the world queries the cars use. |
| `src/car` | Car specifications from the database, and the vehicle simulation. |
| `src/carmodel` | Car render models and materials. |
| `src/xenos` | Translation of the game's Xbox 360 shaders to GLSL. |
| `src/render` | OpenGL 4.5 renderer: world, cars, sky, clouds, shadows, crowds, animated objects, post-processing. |
| `src/animated` | Animated world objects and their Granny animations. |
| `src/spectators` | Crowd placement and animation. |
| `src/particles` | Particle effects. |
| `src/audio`, `src/radio` | Sound banks, the mixer, engine and collision sounds; the radio stations. |
| `src/ui` | The game's UI scenes: HUD, menus, map, loading and title screens. |
| `src/profile` | The saved profile and its schema. |
| `src/game` | Free roam, races, AI drivers, traffic, activities, time of day. |
| `src/platform` | Window, OpenGL context and input (SDL3). |
| `src/app` | The `fh1` executable. |
| `tools`, `tests` | Command-line tools and tests. |

## Coordinates

The game's world is left-handed: x east, y up, z north. Car space is x right, y up, z forward, with the origin on the ground below the middle of the wheelbase, as the car files use. The renderer mirrors z in its view matrix so OpenGL shows the world the right way round.

## License

MIT for the code in this repository. Forza Horizon and its data belong to their owners; you need your own copy of the game.
