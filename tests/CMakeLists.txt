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
# The disc tests inside (decoding every sample, parsing every project and
# engine tuning) run only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_audio COMMAND tst_audio)

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
# The test of every game shader runs only when FH1_GAME_DIR is set.
add_test(NAME tst_xenos COMMAND tst_xenos)

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

add_executable(tst_car TestHarness.h tst_car.cpp)
target_compile_options(tst_car PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_car PRIVATE fh1data)
# The real cars on a flat or tilted plane; skips without FH1_GAME_DIR.
add_test(NAME tst_car COMMAND tst_car)

add_executable(tst_activities TestHarness.h tst_activities.cpp)
target_compile_options(tst_activities PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_activities PRIVATE fh1engine)
# The Colorado test inside runs only when FH1_GAME_DIR names an extracted disc.
add_test(NAME tst_activities COMMAND tst_activities)

add_executable(tst_scenarios TestHarness.h tst_scenarios.cpp)
target_compile_options(tst_scenarios PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_scenarios PRIVATE fh1engine)
# The whole game offscreen with scripted controls; skips without
# FH1_GAME_DIR. Each scenario loads the game, so they take minutes.
add_test(NAME tst_scenarios COMMAND tst_scenarios)
set_tests_properties(tst_scenarios PROPERTIES TIMEOUT 1800)
