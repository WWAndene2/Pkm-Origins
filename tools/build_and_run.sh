#!/bin/bash
# The game rebuilt and run once, rendering (the loop of docs/RECOMPILATION_PLAN.md §10-11's work): the translation
# (tools/build_game.py, about 9 min on 4 cores; skipped with --no-translate when only src/ changed), the runtime linked
# against it, the game run with its frames written, and the end of its log.
#
#   tools/build_and_run.sh <scratch> <seconds> [--no-translate] [--slow]
#
# <scratch>: the session's scratch space holding work/oras (the owner's extracted game: code.bin, cro/, save/...),
# work/dumps/oras.3ds and the build in build_oras_mod/. Frames in <scratch>/run/, the log in <scratch>/run/log.
# --slow: $HOS_TIME_SCALE=0.12, the game's clock slowed so that no frame is dropped (for tools/frame_compare.py
# against Azahar's shots). The game's files and frames stay in the scratch space (CLAUDE.md §1).
set -uo pipefail
S=$1; secs=$2; shift 2
translate=1; slow=
for x in "$@"; do
    case $x in --no-translate) translate=0 ;; --slow) slow=0.12 ;; esac
done
repo=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo"
if [ $translate = 1 ]; then
    python3 tools/build_game.py "$S/work/oras/code.bin" symbols/oras_functions.tsv "$S/build_oras_mod" \
        --cro "$S/work/oras/cro" --modules symbols/oras_cro_functions.tsv --gpu-trace > "$S/build_oras_mod.log" 2>&1 ||
        { tail -5 "$S/build_oras_mod.log"; exit 1; }
    head -2 "$S/build_oras_mod.log"
fi
cc -std=c11 -D_GNU_SOURCE -O2 -w -DHOS_GPU_TRACE -I src/hos -o "$S/build_oras_mod/game" $(find src -name "*.c" -not -path "src/platform/android/*") \
    "$S/build_oras_mod/libgame_code.a" -lm -lpthread -lvulkan || exit 1
rm -rf "$S/run"; mkdir -p "$S/run"
cd "$S"
HOS_TIME_SCALE=${slow:-1} HOS_RENDER_DIR="$S/run" timeout -s INT "$secs" ./build_oras_mod/game work/oras work/dumps/oras.3ds \
    > "$S/run/log" 2>&1
grep -v 'renderer\|command lists\|fault 100' "$S/run/log" | tail -6
grep 'command lists' "$S/run/log" | tail -1
ls "$S/run"/frame_*_1808C800.ppm 2>/dev/null | wc -l | sed 's/$/ top-screen frames/'
