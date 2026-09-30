#!/usr/bin/env bash
# Runs scenario tests and keeps the screenshots they take, for a person to
# look at: the pause menu, the map, a turn, and so on.
#
#   scripts/capture.sh OUT_DIR [SCENARIO_FILTER] [BUILD_DIR]
#
# SCENARIO_FILTER selects scenarios whose names contain it (all when empty).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:?usage: scripts/capture.sh OUT_DIR [SCENARIO_FILTER] [BUILD_DIR]}"
filter="${2:-}"
build="${3:-$root/build}"

if [[ -z "${FH1_GAME_DIR:-}" ]]; then
    settings="${XDG_CONFIG_HOME:-$HOME/.config}/fh1-native/settings.ini"
    FH1_GAME_DIR="$(sed -n 's/^[[:space:]]*game[[:space:]]*=[[:space:]]*//p' "$settings" 2>/dev/null | head -n 1)"
fi
if [[ -z "$FH1_GAME_DIR" ]]; then
    echo "Set FH1_GAME_DIR to the extracted disc." >&2
    exit 2
fi
export FH1_GAME_DIR
export SDL_VIDEO_DRIVER=offscreen
export SDL_AUDIO_DRIVER=dummy
export FH1_NO_CRASH_DIALOG=1
mkdir -p "$out"
export FH1_CAPTURE_DIR="$(cd "$out" && pwd)"

cmake --build "$build" --target tst_scenarios
"$build/tests/tst_scenarios" "$filter"
echo "Screenshots in $FH1_CAPTURE_DIR"
