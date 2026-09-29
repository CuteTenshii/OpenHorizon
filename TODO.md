# To do

What is left before this plays like Forza Horizon, from player reports and from the code. Each item is taken from the game's own data or code, not approximated; where that is not possible yet, the item says what is missing.

## In progress

| Area | Problem | Work |
|------|---------|------|
| World | Shading looks wrong, forests are missing, the sky has no clouds, sun or moon. | Track shaders through the Xenos translator, the forest layers, sky objects. |

## Next

- Driving: 0-60 (14.5%) and quarter-mile medians are worse since the game's traction control, which does not act at take-off, replaced the made-up one; the standing-start clutch model is not traced. The game's stability management and traction control only act above 30 mph, so powerful old rear-drive cars can still spin at low speed.
- Flyers: the "{0}/{1} DISCOUNT SIGNS SMASHED" notification is not shown, and their mass comes from the guessed density rule.
- HUD notifications: the combined notification (HUD_COMBINED_NOTIFICATION) is not driven, because its queue and timing (around 0x8289e808) were not traced; the flyer, outpost discovery ("HORIZON OUTPOST DISCOVERED", 10 s) and barn find notifications wait on it.
- Event starts: every career event with a zone is offered, since career progression (which unlocks them) does not exist; the zone's `<object>_flare` object, switched on while the car is inside (0x8298b648), was not found in the world data; the prompt's event icon (SET_EVENTICON_*) is not chosen, as its table was not traced.
- Speed traps: shown on the map once discovered; their "completed" state, the tutorial (Activity/SpeedCamera/TutorialRadius) and the FirstSpeedTrap100MPH award are not done, and the leaderboards are online-only. The gate trigger's box is built with VMX128 code (0x8296d1b8) not decoded, so a pass is the car's ground point crossing the line between the posts.
- Speed zones (9, `average_speed.xml`): not implemented. The zone follows the road network between its two gates (built with the route search at 0x82d9e548, not traced), and its map line and its average both need that route.
- Horizon outposts: discovery, the map icon and the activation prompt work; what the zone opens (CShowGasStationScreen: fast travel for Activities/Outposts/TotalCost credits less the Discount_N_Missions share, and its three missions) needs credits, fast travel and missions.
- Car sounds: wind, suspension bottom-out, transmission whine and surface-change one-shots exist on the disc but are not played; the player's four tyres play centred.
- Cars: environment reflections (the cube maps are runtime render targets), mirrors and shadows are not produced, so chrome and glass reflect black; tyre deformation and wheel scale are not traced, so some cars' tyres sit oversized in their arches; the body sits on the ground because physics does not apply maxdata's stock ride height.
- Effects: tyre smoke and dust run on stand-in inputs for two wheel values the game's tyre model computes; rim smoke, backfire flames and engine smoke are not triggered.
- Stopped cars still creep slowly on steep slopes (2 m in 30 s at 20%): the tyre model has no static friction at walking speed.
- Title screen: the boot movies before it (`T10_MS_Combined.wmv`, then `Dolby_Corona_Intro.wmv`, then `forza_intro.wmv`) are not played; the profile sign-in and the signed-in SPLASH_MENU are skipped; what the idle attract (front-end request 6) plays was not traced.
- Loading screens: the initial load follows prepare_free_roam (the "freeroam" tips until streaming, cars, the radio and the initial traffic are in, then the 1 s fade from black). Fast travel and loading into a race from the menus (CLoadIntoCareerRace, festival tip groups) do not exist yet. Starting or restarting a race from free roam uses no tip screen in the game: prerace_prepare blacks out and waits for streaming and cars, and how its black-out is lifted was not traced, so that wait is not done here. Linked GL programs are cached in `~/.cache/fh1-native/shaders`.
- Pause menu: its entries for multiplayer, photo mode and my profile lead nowhere yet. The help bar's packing of its buttons by their widths (0x8289e100) is not implemented, so help buttons keep their authored widths instead of fitting their text; this applies to every screen with a help bar.
- Race AI: in autopilot races the AI regularly hits scenery at corners and loses tens of seconds, before and after the steering rewrite.
- Collision: asking once per car whether the collision mesh is the ground broke races and was reverted; world contacts still cost about 0.15 ms per car per physics step.
- Exposure and colour grading: dusk and night are as bright as day. The adaptive exposure (`ToneMapAdaptiveExposure` in `TrackSettings.xml`), the night colour-grading LUT and the desaturation matrix are partly decoded.
- Radio in races: the game's `StartRadioFlowForEvent` (a named track timed to the start) is implemented in the radio but not called; gameplay DJ lines, positional stations and forced playlists are not played.
- Traffic: road classes and roads come from racing lines as a stand-in, because the `.nav` segments are not decoded for traffic yet.
- Smashable masses: `physicsdefinitions.bin` is not read.
- Minimap and HUD: route lines, the mini leaderboard, text shadows, career decorations on icons and the "radio tuning" note are not shown (their data or trigger was not found).
- The HUD's rev dial looks lighter in play than in the HUD tool; needs a reference from the real game.
- Race HUD: the HUD scene's lap, last-lap and best-lap timers are fed only the race time, because the rule that fills them (0x828b3f10 and the slot setters 0x828b0d70-0x828b10dc on the race status object) is not traced; the wrong-way warning (`WRONG_WAY_ON`), the position bars' gap times (`SHOW_TIME`, `TEXT_TIME`), the corner arrows (`HUD_CORNER_ARROWS`) and the end-of-race countdown (`HUD_END_OF_RACE_COUNTDOWN`) are not driven; the position bars do not show. After the finish the game shows its post-race scenes (`h_post_race_finished`, `h_post_race_results` and the rest), which are not implemented, so nothing shows the results and free roam comes back after a chosen 8 s. The start and finish gates (`anim_startfinishlinesparks.pgeo`, `obj_startline_digital_panels`) are not placed.
- Driving line: colours compare the car's speed with our own AI's speed profile, since the game's speed planner (0x824a4e40) is not traced; the route's height overrides (0x8342dd20), the stencil mask it is drawn under, the "Braking" and "Off" settings and the free-roam sat-nav line are not implemented.
- Cars whose model fails to load are not drawn (they used to be drawn as outlines).
- Road discovery: the roads driven (which colour the map's roads) are kept for the session only, as the profile is not saved; the "ROADS DRIVEN" {0}/{1} notification waits on the combined notification; the colour curve the map profile loader applies to every colour (0x82cf46b0, through 0x8247c8e0) is not worked out, so the maps use the profile's colours as written.

## Not started

- Career and progression: wristbands, popularity, credits, the festival's event flow, showcase events.
- Upgrades, tuning and paint; the car dealer and autoshow.
- Barn finds (hints, discovery, restoration), PR stunts, speed stunts and photo shoots (the outposts' missions), street race hubs, race encounters, the festival buildings' menus (workshop, paint shop, autoshow, car club, race central, DLC centre).
- Saving and loading the profile (`media/profileschema/ForzaProfile.sch`).
- Photo mode, rewind (Y), the horn, replays.
- Online play.
- Rain, weather, and damage to cars.
