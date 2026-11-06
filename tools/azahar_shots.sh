#!/bin/bash
# Azahar's screenshots of a moment of the game, for tools/frame_compare.py (docs/RECOMPILATION_PLAN.md §11.6): a
# headless run of Pomegrade's (tools/remake/headless/run_local.sh), a shot every <step> frames from <first>.
#
#   tools/azahar_shots.sh <Pomegrade> <work> <name> <first frame> <count> <step> [save|none] [prefix]
#
# The shots: <work>/runs/<name>/<prefix><k>.png (400 x 480: the top screen above the bottom one), k = 0..count-1, at
# frames first + k * step. save: the owner's save (<work>/dumps/save/main, the default: our runs read the same one,
# work/oras/save/main); none: no save (a first boot: after the opening, the language menu instead of the title movie,
# 6 November). Pomegrade is only read. The images are the owner's game's: they stay in the scratch space.
set -uo pipefail
pomegrade=$1; work=$2; name=$3; first=$4; count=$5; step=$6; save=${7:-save}; prefix=${8:-s}
script="wait $first"
for k in $(seq 0 $((count - 1))); do script="$script;shot $prefix$k;wait $step"; done
secs=$(( (first + count * step) / 4 + 300 ))  # (about 4 frames a second headless, and the boot)
if [ "$save" = none ]; then export POMEGRADE_SAVE=none; fi
"$pomegrade/tools/remake/headless/run_local.sh" "$work" "$name" - "" "$script" "$secs"
ls "$work/runs/$name/$prefix"*.png 2>/dev/null | wc -l | sed 's/$/ shots/'
