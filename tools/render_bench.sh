#!/bin/bash
# The renderer's speed, measured the same way each time (docs/RECOMPILATION_PLAN.md §11.3): the top-screen frames the
# game produces in <seconds> from its start, unslowed, with the renderer's submissions. Compare two builds by running
# each on the same seconds (6 November: one submission a frame, 1,101 -> 1,301 frames in 120 s).
#
#   tools/render_bench.sh <scratch> <seconds> [game binary]
#
# <scratch>: as tools/build_and_run.sh's (work/oras, work/dumps/oras.3ds, build_oras_mod/game by default). Frames in
# <scratch>/bench_run. Here the GPU is lavapipe, on the CPU: absolute numbers say nothing of a phone, gains between
# builds do. The frames are the owner's game's: they stay in the scratch space.
set -uo pipefail
S=$1; secs=$2; bin=${3:-$S/build_oras_mod/game}
rm -rf "$S/bench_run"; mkdir -p "$S/bench_run"
cd "$S" || exit 1
HOS_RENDER_DIR="$S/bench_run" timeout -s INT "$secs" "$bin" work/oras work/dumps/oras.3ds > "$S/bench_run/log" 2>&1
echo "$(ls "$S/bench_run" | grep -c _1808C800) top-screen frames in $secs s"
grep 'submissions\|command lists' "$S/bench_run/log" | tail -2
