# To do

What is left before this plays like Forza Horizon, from player reports and from the code. Each item is taken from the game's own data or code, not approximated; where that is not possible yet, the item says what is missing.

## In progress

| Area | Problem | Work |
|------|---------|------|
| Driving | The car spins under power: traction and stability control do not hold it (a one-second full-lock pulse at 45 km/h on full throttle keeps yawing at 0.7 rad/s). | Reimplementing ABS, TCS and STM from CCarDynamics. |
| Sound | A constant squeak or rub while driving. The tyre skid event gets made-up inputs (slip angle = slip x 4, slip ratio 0, load 1). | Per-wheel tyre audio from the game's FMOD project and code. |
| Camera | The chase camera goes wrong on hills. Its distance behind the car and its height rule are guesses; four ini values are unused. | The follow camera from the game's code. |
| Cars | Models are broken (rear body cut off, misplaced side panel, glass shows noise, tyres wrong) and use a hand-written shader. | .carbin decoding fixes and the game's car shaders through the Xenos translator. |
| World | Shading looks wrong, forests are missing, the sky has no clouds, sun or moon. | Track shaders through the Xenos translator, the forest layers, sky objects. |
| Effects | No tyre smoke, dust or other particles. | The game's particle system. |
| Menus | No pause menu or full-screen map. | The game's pause menu and world map scenes. |
| Controls | Pad layout partly assumed (Back changes view; the map should be on Back). | Reading the game's controller layout, then rebinding. |
| Props | The 100 discount flyers ("upgrade signs") cannot be smashed. | Spawning them as breakables with their model, and counting them. |

## Next

- Parking brake: a stopped car should hold still. The game switches `CPlayerCarParkingBrake` on and off through flow states (`media/gamemodes/state_definitions.xml`); what it does while on is in the code, not yet read.
- Race AI: in autopilot races the AI regularly hits scenery at corners and loses tens of seconds, before and after the steering rewrite.
- Car physics regressions from the database's centre-of-mass height: braking median 5.24% and 0-60 11.65% (were 4.29% and 9.59%), because the brake split and launch were tuned to the old made-up value.
- Collision: asking once per car whether the collision mesh is the ground broke races and was reverted; world contacts still cost about 0.15 ms per car per physics step.
- Exposure and colour grading: dusk and night are as bright as day. The adaptive exposure (`ToneMapAdaptiveExposure` in `TrackSettings.xml`), the night colour-grading LUT and the desaturation matrix are partly decoded.
- Radio in races: the game's `StartRadioFlowForEvent` (a named track timed to the start) is implemented in the radio but not called; gameplay DJ lines, positional stations and forced playlists are not played.
- Traffic: road classes and roads come from racing lines as a stand-in, because the `.nav` segments are not decoded for traffic yet.
- Smashable masses: `physicsdefinitions.bin` is not read.
- Minimap and HUD: route lines, the mini leaderboard, text shadows, career decorations on icons and the "radio tuning" note are not shown (their data or trigger was not found).
- The HUD's rev dial looks lighter in play than in the HUD tool; needs a reference from the real game.

## Not started

- Career and progression: wristbands, popularity, credits, the festival's event flow, showcase events.
- Upgrades, tuning and paint; the car dealer and autoshow.
- Barn finds, speed traps and speed zones, PR stunts.
- Saving and loading the profile (`media/profileschema/ForzaProfile.sch`).
- Photo mode, rewind (Y), the horn, replays.
- Online play.
- Rain, weather, and damage to cars.
