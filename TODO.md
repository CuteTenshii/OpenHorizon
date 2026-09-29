# To do

What is left before this plays like Forza Horizon, from player reports and from the code. Each item is taken from the game's own data or code, not approximated; where that is not possible yet, the item says what is missing.

## In progress

| Area | Problem | Work |
|------|---------|------|
| World | Shading looks wrong, forests are missing, the sky has no clouds, sun or moon. | Track shaders through the Xenos translator, the forest layers, sky objects. |

## Next

- Driving: 0-60 (14.5%) and quarter-mile medians are worse since the game's traction control, which does not act at take-off, replaced the made-up one; the standing-start clutch model is not traced. The game's stability management and traction control only act above 30 mph, so powerful old rear-drive cars can still spin at low speed.
- Flyers: the "{0}/{1} DISCOUNT SIGNS SMASHED" notification is not shown, and their mass comes from the guessed density rule.
- Car sounds: wind, suspension bottom-out, transmission whine and surface-change one-shots exist on the disc but are not played; the player's four tyres play centred.
- Cars: environment reflections (the cube maps are runtime render targets), mirrors and shadows are not produced, so chrome and glass reflect black; tyre deformation and wheel scale are not traced, so some cars' tyres sit oversized in their arches; the body sits on the ground because physics does not apply maxdata's stock ride height.
- Effects: tyre smoke and dust run on stand-in inputs for two wheel values the game's tyre model computes; rim smoke, backfire flames and engine smoke are not triggered.
- Stopped cars still creep slowly on steep slopes (2 m in 30 s at 20%): the tyre model has no static friction at walking speed.
- Title screen: the boot movies before it (`T10_MS_Combined.wmv`, then `Dolby_Corona_Intro.wmv`, then `forza_intro.wmv`) are not played; the profile sign-in and the signed-in SPLASH_MENU are skipped; what the idle attract (front-end request 6) plays was not traced.
- Loading screens: shown for the initial load; fast travel and race loads do not exist yet.
- Pause menu: its entries for multiplayer, photo mode and my profile lead nowhere yet; long entries overrun their bars until the menu's text-fitting layout is implemented.
- Race AI: in autopilot races the AI regularly hits scenery at corners and loses tens of seconds, before and after the steering rewrite.
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
