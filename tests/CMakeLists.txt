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
