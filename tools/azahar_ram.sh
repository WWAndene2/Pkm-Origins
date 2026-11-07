#!/bin/bash
# The game's memory in Azahar at a moment, for tools/match_functions.py --ram (docs/AGENT_HANDOFF.md, setup step 4):
# a headless run of Pomegrade's run_local.sh (read only) waiting <frames>, then its code, heap, linear heap and stack
# dumped into <work>/runs/<name>/. About 6 minutes for 1,500 frames. The memory is the owner's game's: scratch only.
#
#   tools/azahar_ram.sh <Pomegrade> <work> [name (ram_oras)] [frames (1500)]
#
# (The dump's addresses are given with 0x: retro_host reads them in base 0, a leading 0 is octal; its file names are
# relative to the run's own folder, which run_local.sh empties at the start: hence ../<name>/.)
set -euo pipefail
pomegrade=$1; work=$(cd "$2" && pwd); name=${3:-ram_oras}; frames=${4:-1500}
mkdir -p "$work/runs/$name"
"$pomegrade/tools/remake/headless/run_local.sh" "$work" "${name}_run" - "" \
    "wait $frames;dump 0x00100000 0x00700000 ../$name/code.bin;dump 0x08000000 0x06000000 ../$name/heap.bin;dump 0x14000000 0x08000000 ../$name/linear.bin;dump 0x0FF00000 0x00100000 ../$name/stack.bin" \
    900 > /dev/null 2>&1 || true
ls -l "$work/runs/$name"
