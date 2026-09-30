---
name: find-asset-evidence
description: Find which game file, scene element, XML entry, gamedb row or string drives a feature before writing code for it, and record what is verified, inferred or missing. Use whenever work needs an asset, a value, a colour, a layout or a rule from Forza Horizon (HUD, menus, map, sky, effects, audio, car data), and whenever you are tempted to pick a value that "looks right".
---

# Find the evidence for an asset or value

The project rule: every asset use and value comes from the game's data or its code. When neither gives it, the feature leaves it out and says so. A plausible stand-in is a bug, even if it looks fine in a screenshot.

The disc is `$FH1_GAME_DIR` (if unset, the `game` key in `~/.config/fh1-native/settings.ini`). Machine paths for the executable image and notes are in `AGENTS.local.md`. The tools are built into `build/src/` and `build/src/<module>/`; build them first (`cmake -S . -B build -DFH1N_BUILD_TOOLS=ON`, then `cmake --build build`).

## 1. Say what you are looking for

Write down the question before you search: "which texture is the minimap's player arrow", "what colour multiplies the map viewport", "which mixer snapshot plays while paused". If you are looking for "anything that looks like a speedometer", you will find something and use it whether it is right or not.

## 2. Find the file

Everything is searched through the engine's own VFS, which mounts loose files and every archive in `media/zipmanifest.xml` (`media/ui.zip` becomes `media/ui/...`):

```sh
build/src/fh1vfs "$FH1_GAME_DIR" list media/ui | grep -i map
build/src/fh1vfs "$FH1_GAME_DIR" cat media/audio/audiomixersnapshots.xml | less
build/src/fh1vfs "$FH1_GAME_DIR" extract PATH "$SCRATCH/out.bin"
```

Where things usually live:

| What | Where | Tool |
|---|---|---|
| HUD, menus, map, loading screens | `media/ui/scenes/ui4/*` scenes | `build/src/ui/fh1ui DISC scene NAME [--attributes]`, `build/src/ui/fh1menu DISC contracts SCENE [--names FILE]` |
| UI textures and fonts | paths named by the scene | `fh1ui DISC texture PATH OUT.png`, `fh1ui DISC fonts` |
| Text | `media/stringtables/<lang>.zip` | `fh1ui DISC string ID` (for `IDS_*` ids); database `_&N` references go through `Localisation::resolve` with the table named after the database table |
| Cars, events, upgrades, tunables | `media/db/gamedb.slt` | `sqlite3 "$FH1_GAME_DIR/media/db/gamedb.slt"`, `build/src/fh1carspec DISC CAR` |
| Track data, time of day, routes | `media/tracks/<track>/` | `build/src/world/fh1world DISC TRACK --at X,Z` |
| Effects | `media/effects/` | `build/src/particles/fh1particles DISC list\|show\|surfaces` |
| Shaders | `media/shaders/*.fxobj` | `build/src/xenos/fh1shader DISC list\|dis\|glsl PATH` |
| Audio banks and events | `media/audio/` | `build/src/audio/fh1audio` |
| Collision and surfaces | `.fiz`, `surfaceTypes.xml` | `build/src/collision/fh1fiz` |

Search broadly with `list` plus `grep -i`, and look in the XML with `cat`. For gamedb, start with `.tables` and `.schema TABLE`, then query. Don't read table names and assume what the columns mean.

## 3. Find how the game uses it

Having a file is not the same as knowing how it is used. A texture named `arrow` might not be the one drawn, a colour might be multiplied by another, a scene element might be hidden by a state. To settle it:

- **Scene contracts and handlers:** `fh1menu DISC contracts SCENE` shows the behaviour elements, their parameters and which events each handles. Names appear as hashes; pass the executable's strings (`strings -n 3 IMAGE > "$SCRATCH/names.txt"`, then `--names "$SCRATCH/names.txt"`) to resolve them.
- **The executable:** find the file name or a key string in the image, follow the `lis`/`addi` pair that loads its address to the code, and disassemble that range (the command is in AGENTS.md). This is how you learn units, multipliers, which of several candidates is picked, and the conditions under which something shows.
- **Reference screenshots from the user:** use them to compare against, never as the source. Match what you derived to them; do not tune values until they match.

## 4. Record the result

Every value and asset used from this search is written down where the code uses it, in a `///` doc comment on the format or rule, that says:

- **Verified:** what, against which file or function address, and over how many files if you checked a pattern (for example "every scene in ui4 has it").
- **Inferred:** what, and the reasoning.
- **Missing:** what could not be found. That part of the feature is left out. It is not approximated.

Put the same information in the commit body. When something is missing, tell the user in your report as well, naming what was searched, so they can point to it or accept the gap.

## Red flags

Stop and go back to step 2 or 3 if you catch yourself:

- choosing a colour, size, offset or timing because it looks close to a screenshot;
- using a texture because its name sounds right, without finding what references it;
- adding a constant with no comment on where it came from;
- replacing an element you could not find with a drawn shape, a solid colour or text of your own;
- writing "approximately" or "roughly matches" about game behaviour.
