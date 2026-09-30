include(Catch)

# What every test program shares: the Catch2 session, which keeps the
# game's error boxes closed and picks SDL's offscreen drivers, and
# openhorizon::test::gameDir(), which skips a case when there is no disc.
add_library(fh1testsupport STATIC support/TestSupport.h support/TestSupport.cpp)
target_compile_options(fh1testsupport PRIVATE ${FH1N_WARNINGS})
target_include_directories(fh1testsupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fh1testsupport PUBLIC Catch2::Catch2 fh1base)

add_library(fh1testmain STATIC support/Main.cpp)
target_compile_options(fh1testmain PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1testmain PUBLIC fh1testsupport)

# The hidden offscreen context of the cases that draw.
add_library(fh1testgl STATIC support/GlContext.h support/GlContext.cpp)
target_compile_options(fh1testgl PRIVATE ${FH1N_WARNINGS})
target_link_libraries(fh1testgl PUBLIC fh1testsupport fh1platform)

# fh1n_add_test_program(NAME SOURCES... [LIBRARIES...] [NO_MAIN])
#
# Builds a test program and registers each of its test cases with ctest as
# NAME.<case>, so ctest runs and reports them one by one. Cases tagged
# [slow] (sweeps over every file of a kind on the disc) carry the label
# slow, which the default runs leave out. The cases of one program share
# folders under the temporary directory, so they take a lock of the
# program's name: they run one at a time, while different programs run in
# parallel.
function(fh1n_add_test_program name)
    cmake_parse_arguments(ARG "NO_MAIN" "" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_compile_options(${name} PRIVATE ${FH1N_WARNINGS})
    if(ARG_NO_MAIN)
        target_link_libraries(${name} PRIVATE fh1testsupport ${ARG_LIBRARIES})
    else()
        target_link_libraries(${name} PRIVATE fh1testmain ${ARG_LIBRARIES})
    endif()
    # Listing the cases when ctest runs rather than after the build keeps a
    # build from starting the programs it has just linked.
    catch_discover_tests(${name}
        TEST_PREFIX "${name}."
        TEST_SPEC "~[slow]"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES RESOURCE_LOCK ${name})
    catch_discover_tests(${name}
        TEST_PREFIX "${name}."
        TEST_SPEC "[slow]"
        DISCOVERY_MODE PRE_TEST
        PROPERTIES RESOURCE_LOCK ${name} LABELS slow)
endfunction()

# UTF-8 paths, user folders and the file system on host paths; needs no disc.
fh1n_add_test_program(tst_base SOURCES tst_base.cpp LIBRARIES fh1base)

# Starts itself as a child that crashes on purpose and checks the child's
# log and exit status, then the texts of the error boxes; needs no disc. Its
# own main() runs the child and the reporter before the Catch2 session.
fh1n_add_test_program(tst_crash SOURCES tst_crash.cpp LIBRARIES fh1base NO_MAIN)

# The Colorado case inside runs only when FH1_GAME_DIR names an extracted
# disc; the variable is read when the case runs, not when it is configured.
fh1n_add_test_program(tst_world SOURCES tst_world.cpp LIBRARIES fh1world)

# As with tst_world, the Colorado case runs only when FH1_GAME_DIR is set.
fh1n_add_test_program(tst_collision SOURCES tst_collision.cpp LIBRARIES fh1collision)

# The disc cases inside (decoding a few banks of every kind, parsing every
# project and engine tuning) run only when FH1_GAME_DIR names an extracted
# disc. Decoding every sample takes most of a minute: that case is tagged
# [slow].
fh1n_add_test_program(tst_audio SOURCES tst_audio.cpp LIBRARIES fh1audio)

# The disc case inside (loading real cars) runs only when FH1_GAME_DIR names
# an extracted disc.
fh1n_add_test_program(tst_carmodel SOURCES tst_carmodel.cpp LIBRARIES fh1carmodel)

# The Colorado case inside runs only when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_spectators SOURCES tst_spectators.cpp LIBRARIES fh1spectators)

# The game's shaders are tested only when FH1_GAME_DIR is set: a few
# effects by default, and every effect in the [slow] case.
fh1n_add_test_program(tst_xenos SOURCES tst_xenos.cpp LIBRARIES fh1xenoscheck)

# The disc cases inside (every scene, gauge and font) run only when
# FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_ui SOURCES tst_ui.cpp LIBRARIES fh1ui)

# The disc cases inside (the real stations, banks and a mixed render) run
# only when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_radio SOURCES tst_radio.cpp LIBRARIES fh1radio)

# The disc cases inside (the pause menu's entries and texts, the scenes and
# the map's data) run only when FH1_GAME_DIR names an extracted disc; the
# drawing ones need an offscreen context too.
fh1n_add_test_program(tst_menu SOURCES tst_menu.cpp LIBRARIES fh1ui fh1testgl)

# The disc case inside (every effect file, the surface table and the effect
# list the game loads) runs only when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_particles SOURCES tst_particles.cpp LIBRARIES fh1particles)

# The GL cases run in an offscreen context and skip without one; the cache
# goes to a folder of their own under the temporary directory.
fh1n_add_test_program(tst_render SOURCES tst_render.cpp LIBRARIES fh1platform fh1testgl)

# The thumbstick shaping on raw pad values; no disc and no window needed.
fh1n_add_test_program(tst_input SOURCES tst_input.cpp LIBRARIES fh1platform)

# clouddefs.xml, cloud curves, motion and domes on synthetic data; the disc's files,
# Colorado's curves and an offscreen draw when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_clouds SOURCES tst_clouds.cpp LIBRARIES fh1engine fh1testgl)

# The shadow cascades' fitting on synthetic cameras; the ShadowIntensity curve
# and an offscreen shadow mask when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_shadows SOURCES tst_shadows.cpp LIBRARIES fh1engine)

# The cube decoder and the track's scene constants on synthetic data; the
# disc's cube maps and settings only when FH1_GAME_DIR names an extracted
# disc.
fh1n_add_test_program(tst_shading SOURCES tst_shading.cpp LIBRARIES fh1engine)

# The disc cases inside (Colorado's animated objects and a few files of the
# media/animatedobjects pack) run only when FH1_GAME_DIR names an extracted
# disc; reading every file of the pack is the [slow] case.
fh1n_add_test_program(tst_animated SOURCES tst_animated.cpp LIBRARIES fh1animated)

# The dynamic environment cube's face schedule, face cameras and mip filter
# maths on synthetic data; an offscreen render of the cube when a GL context
# is available.
fh1n_add_test_program(tst_envcube SOURCES tst_envcube.cpp LIBRARIES fh1engine fh1testgl)

# The real cars on a flat or tilted plane; skips without FH1_GAME_DIR.
fh1n_add_test_program(tst_car SOURCES tst_car.cpp LIBRARIES fh1data fh1collision)

# The Colorado case inside runs only when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_activities SOURCES tst_activities.cpp LIBRARIES fh1engine)

# The driving line's rules on synthetic lines; needs no disc.
fh1n_add_test_program(tst_race SOURCES tst_race.cpp LIBRARIES fh1engine)

# The schema reader and the profile store on synthetic data, in folders
# under the temporary directory; the disc's schema only when FH1_GAME_DIR
# names an extracted disc.
fh1n_add_test_program(tst_profile SOURCES tst_profile.cpp LIBRARIES fh1profile)

# The GPU cases run in an offscreen context and skip without one; the
# Colorado cases run only when FH1_GAME_DIR names an extracted disc.
fh1n_add_test_program(tst_postprocess SOURCES tst_postprocess.cpp LIBRARIES fh1engine fh1testgl)

# The cutscene clock, triggers, fades and cameras on synthetic cutscenes;
# the disc's race and free-roam cutscenes only when FH1_GAME_DIR names an
# extracted disc.
fh1n_add_test_program(tst_cutscenes SOURCES tst_cutscenes.cpp LIBRARIES fh1engine)

# The whole game offscreen with scripted controls, one file per area around
# the shared GameScenario fixture and Session (scenarios/Session.h); they
# skip without FH1_GAME_DIR or an OpenGL context. A game takes about 3 GB,
# so the scenarios take the lock "game" and run one at a time, beside the
# unit tests. The frame-time case, tagged [perf], carries the label perf
# (its timings depend on the machine and its load) and runs alone.
set(FH1N_SCENARIO_SOURCES
    scenarios/Session.h scenarios/Session.cpp scenarios/RoadFollower.h
    scenarios/Activities.cpp scenarios/Audio.cpp scenarios/Driving.cpp scenarios/Map.cpp scenarios/Menus.cpp
    scenarios/Perf.cpp scenarios/Profile.cpp scenarios/Races.cpp scenarios/World.cpp)
add_executable(tst_scenarios ${FH1N_SCENARIO_SOURCES})
target_compile_options(tst_scenarios PRIVATE ${FH1N_WARNINGS})
target_link_libraries(tst_scenarios PRIVATE fh1testmain fh1engine)
catch_discover_tests(tst_scenarios
    TEST_PREFIX "tst_scenarios."
    TEST_SPEC "~[perf]"
    DISCOVERY_MODE PRE_TEST
    PROPERTIES RESOURCE_LOCK game TIMEOUT 600)
catch_discover_tests(tst_scenarios
    TEST_PREFIX "tst_scenarios."
    TEST_SPEC "[perf]"
    DISCOVERY_MODE PRE_TEST
    PROPERTIES RUN_SERIAL TRUE LABELS perf TIMEOUT 600)
