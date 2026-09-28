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
