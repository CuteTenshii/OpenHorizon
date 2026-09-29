#!/usr/bin/env bash
# Configures, builds and runs every test: the unit tests, the car checks and
# the scenario tests that drive the whole game offscreen. Tests that need
# the game's disc run when FH1_GAME_DIR names it (or when the settings file
# remembers it) and skip otherwise.
#
#   scripts/run-tests.sh [BUILD_DIR] [ctest arguments...]
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

# Never open windows on the desktop or play through the speakers.
export SDL_VIDEO_DRIVER=offscreen
export SDL_AUDIO_DRIVER=dummy

cmake -S "$root" -B "$build" -G Ninja
cmake --build "$build"
ctest --test-dir "$build" --output-on-failure "$@"
