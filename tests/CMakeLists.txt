add_executable(tst_base TestHarness.h tst_base.cpp)
target_compile_options(tst_base PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_base PRIVATE fh1base)
# UTF-8 paths, user folders and the file system on host paths; needs no disc.
add_test(NAME tst_base COMMAND tst_base)

add_executable(tst_crash TestHarness.h tst_crash.cpp)
target_compile_options(tst_crash PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_crash PRIVATE fh1base)
# Starts itself as a child that crashes on purpose and checks the child's
# log and exit status, then the texts of the error boxes; needs no disc.
add_test(NAME tst_crash COMMAND tst_crash)

add_executable(tst_world TestHarness.h tst_world.cpp)
target_compile_options(tst_world PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_world PRIVATE fh1world)
# The Colorado test inside runs only when FH1_GAME_DIR names an extracted
# disc; the variable is read when the test runs, not when it is configured.
add_test(NAME tst_world COMMAND tst_world)

add_executable(tst_collision TestHarness.h tst_collision.cpp)
target_compile_options(tst_collision PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_collision PRIVATE fh1collision)
# As with tst_world, the Colorado test runs only when FH1_GAME_DIR is set.
add_test(NAME tst_collision COMMAND tst_collision)

add_executable(tst_audio TestHarness.h tst_audio.cpp)
target_compile_options(tst_audio PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_audio PRIVATE fh1audio)
# The disc tests inside (decoding a few banks of every kind, parsing every
# project and engine tuning) run only when FH1_GAME_DIR names an extracted
# disc. Decoding every sample takes most of a minute: it is the slow test
# tst_audio_slow, left out unless the label slow is asked for (ctest -L slow).
add_test(NAME tst_audio COMMAND tst_audio)
add_test(NAME tst_audio_slow COMMAND tst_audio --slow)
set_tests_properties(tst_audio_slow PROPERTIES LABELS slow)

add_executable(tst_carmodel TestHarness.h tst_carmodel.cpp)
target_compile_options(tst_carmodel PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_carmodel PRIVATE fh1carmodel)
# The disc test inside (loading real cars) runs only when FH1_GAME_DIR names
# an extracted disc.
add_test(NAME tst_carmodel COMMAND tst_carmodel)

add_executable(tst_spectators TestHarness.h tst_spectators.cpp)
target_compile_options(tst_spectators PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_spectators PRIVATE fh1spectators)
# The Colorado test inside runs only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_spectators COMMAND tst_spectators)

add_executable(tst_xenos TestHarness.h tst_xenos.cpp)
target_compile_options(tst_xenos PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_xenos PRIVATE fh1xenoscheck)
# The game's shaders are tested only when FH1_GAME_DIR is set: a few
# effects by default, and every effect in the slow test tst_xenos_slow.
add_test(NAME tst_xenos COMMAND tst_xenos)
add_test(NAME tst_xenos_slow COMMAND tst_xenos --slow)
set_tests_properties(tst_xenos_slow PROPERTIES LABELS slow)

add_executable(tst_ui TestHarness.h tst_ui.cpp)
target_compile_options(tst_ui PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_ui PRIVATE fh1ui)
# The disc tests inside (every scene, gauge and font) run only when
# FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_ui COMMAND tst_ui)

add_executable(tst_radio TestHarness.h tst_radio.cpp)
target_compile_options(tst_radio PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_radio PRIVATE fh1radio)
# The disc tests inside (the real stations, banks and a mixed render) run
# only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_radio COMMAND tst_radio)

add_executable(tst_menu TestHarness.h tst_menu.cpp)
target_compile_options(tst_menu PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_menu PRIVATE fh1ui)
# The disc tests inside (the pause menu's entries and texts, the scenes and
# the map's data) run only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_menu COMMAND tst_menu)

add_executable(tst_particles TestHarness.h tst_particles.cpp)
target_compile_options(tst_particles PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_particles PRIVATE fh1particles)
# The disc test inside (every effect file, the surface table and the effect
# list the game loads) runs only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_particles COMMAND tst_particles)

add_executable(tst_render TestHarness.h tst_render.cpp)
target_compile_options(tst_render PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_render PRIVATE fh1platform)
# The GL cases run in an offscreen context and skip without one; the cache
# goes to a folder of their own under the temporary directory.
add_test(NAME tst_render COMMAND tst_render)

add_executable(tst_input TestHarness.h tst_input.cpp)
target_compile_options(tst_input PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_input PRIVATE fh1platform)
# The thumbstick shaping on raw pad values; no disc and no window needed.
add_test(NAME tst_input COMMAND tst_input)

add_executable(tst_clouds TestHarness.h tst_clouds.cpp)
target_compile_options(tst_clouds PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_clouds PRIVATE fh1engine)
# clouddefs.xml, cloud curves, motion and domes on synthetic data; the disc's files,
# Colorado's curves and an offscreen draw when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_clouds COMMAND tst_clouds)

add_executable(tst_shadows TestHarness.h tst_shadows.cpp)
target_compile_options(tst_shadows PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_shadows PRIVATE fh1engine)
# The shadow cascades' fitting on synthetic cameras; the ShadowIntensity curve
# and an offscreen shadow mask when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_shadows COMMAND tst_shadows)

add_executable(tst_shading TestHarness.h tst_shading.cpp)
target_compile_options(tst_shading PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_shading PRIVATE fh1engine)
# The cube decoder and the track's scene constants on synthetic data; the
# disc's cube maps and settings only when FH1_GAME_DIR names an extracted
# disc.
add_test(NAME tst_shading COMMAND tst_shading)

add_executable(tst_animated TestHarness.h tst_animated.cpp)
target_compile_options(tst_animated PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_animated PRIVATE fh1animated)
# The disc tests inside (Colorado's animated objects and a few files of the
# media/animatedobjects pack) run only when FH1_GAME_DIR names an extracted
# disc; reading every file of the pack is the slow test tst_animated_slow.
add_test(NAME tst_animated COMMAND tst_animated)
add_test(NAME tst_animated_slow COMMAND tst_animated --slow)
set_tests_properties(tst_animated_slow PROPERTIES LABELS slow)

add_executable(tst_envcube TestHarness.h tst_envcube.cpp)
target_compile_options(tst_envcube PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_envcube PRIVATE fh1engine)
# The dynamic environment cube's face schedule, face cameras and mip filter
# maths on synthetic data; an offscreen render of the cube when a GL context
# is available.
add_test(NAME tst_envcube COMMAND tst_envcube)

add_executable(tst_car TestHarness.h tst_car.cpp)
target_compile_options(tst_car PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_car PRIVATE fh1data fh1collision)
# The real cars on a flat or tilted plane; skips without FH1_GAME_DIR.
add_test(NAME tst_car COMMAND tst_car)

add_executable(tst_activities TestHarness.h tst_activities.cpp)
target_compile_options(tst_activities PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_activities PRIVATE fh1engine)
# The Colorado test inside runs only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_activities COMMAND tst_activities)

add_executable(tst_race TestHarness.h tst_race.cpp)
target_compile_options(tst_race PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_race PRIVATE fh1engine)
# The driving line's rules on synthetic lines; needs no disc.
add_test(NAME tst_race COMMAND tst_race)

add_executable(tst_profile TestHarness.h tst_profile.cpp)
target_compile_options(tst_profile PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_profile PRIVATE fh1profile)
# The schema reader and the profile store on synthetic data, in folders
# under the temporary directory; the disc's schema only when FH1_GAME_DIR
# names an extracted disc.
add_test(NAME tst_profile COMMAND tst_profile)

add_executable(tst_postprocess TestHarness.h tst_postprocess.cpp)
target_compile_options(tst_postprocess PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_postprocess PRIVATE fh1engine)
# The GPU cases run in an offscreen context and skip without one; the
# Colorado case runs only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_postprocess COMMAND tst_postprocess)

add_executable(tst_cutscenes TestHarness.h tst_cutscenes.cpp)
target_compile_options(tst_cutscenes PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_cutscenes PRIVATE fh1engine)
# The cutscene clock, triggers, fades and cameras on synthetic cutscenes;
# the disc's race and free-roam cutscenes only when FH1_GAME_DIR names an
# extracted disc.
add_test(NAME tst_cutscenes COMMAND tst_cutscenes)

add_executable(tst_scenarios RoadFollower.h TestHarness.h tst_scenarios.cpp)
target_compile_options(tst_scenarios PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_scenarios PRIVATE fh1engine)
# The whole game offscreen with scripted controls; skips without
# FH1_GAME_DIR. Each scenario loads the game, so they take minutes.
add_test(NAME tst_scenarios COMMAND tst_scenarios)
set_tests_properties(tst_scenarios PROPERTIES TIMEOUT 1800)

add_executable(tst_perf RoadFollower.h TestHarness.h tst_perf.cpp)
target_compile_options(tst_perf PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_perf PRIVATE fh1engine)
# Free roam driven along the roads, timed per part of the frame, against a
# drawing budget. Needs the disc and a GPU, and its timings depend on the
# machine and its load: labelled perf and left out of the pre-push run.
add_test(NAME tst_perf COMMAND tst_perf)
set_tests_properties(tst_perf PROPERTIES LABELS perf TIMEOUT 600)
