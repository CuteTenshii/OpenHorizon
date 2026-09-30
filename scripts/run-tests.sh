#!/usr/bin/env bash
# Configures, builds and runs every test: the unit tests, the car checks and
# the scenario tests that drive the whole game offscreen. Tests that need
# the game's disc run when FH1_GAME_DIR names it (or when the settings file
# remembers it) and skip otherwise. The slow tests, sweeps over every file
# of a kind on the disc, run only when a label option asks for them.
#
# ctest runs a test case per process and as many at once as there are
# processors; the scenarios hold a lock that keeps them to one at a time,
# since a game takes about 3 GB, and the perf case runs alone.
#
#   scripts/run-tests.sh [BUILD_DIR] [ctest arguments...]
#   scripts/run-tests.sh build -L slow     # just the slow tests
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$root/build}"
shift || true

if [[ -z "${FH1_GAME_DIR:-}" ]]; then
    settings="${XDG_CONFIG_HOME:-$HOME/.config}/fh1-native/settings.ini"
    if [[ -f "$settings" ]]; then
        FH1_GAME_DIR="$(sed -n 's/^[[:space:]]*game[[:space:]]*=[[:space:]]*//p' "$settings" | head -n 1)"
        export FH1_GAME_DIR
    fi
fi

# Never open windows on the desktop or play through the speakers, and
# never wait on an error box after a crash.
export SDL_VIDEO_DRIVER=offscreen
export SDL_AUDIO_DRIVER=dummy
export FH1_NO_CRASH_DIALOG=1

label_given=false
for arg in "$@"; do
    case "$arg" in
    -L* | --label-*) label_given=true ;;
    esac
done
if [[ "$label_given" == false ]]; then
    set -- "$@" -LE slow
fi

# A folder configured for the game alone has no tests.
cmake -S "$root" -B "$build" -G Ninja -DFH1N_BUILD_TESTS=ON
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure --parallel "$(getconf _NPROCESSORS_ONLN)" "$@"
