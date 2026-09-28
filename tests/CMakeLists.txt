add_executable(tst_world TestHarness.h tst_world.cpp)
target_compile_options(tst_world PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_world PRIVATE fh1world)
# The Colorado test inside runs only when FH1_GAME_DIR names an extracted
# disc; the variable is read when the test runs, not when it is configured.
add_test(NAME tst_world COMMAND tst_world)
