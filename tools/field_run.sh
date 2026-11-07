#!/bin/bash
# The game run to the field, frames written (docs/AGENT_HANDOFF.md, step 4 of 7 November): START at 110 s on the title
# screen, then A every 6 s until 220 s (Continue, the save's messages; more A walks the player into the house), with the save in <scratch>/work/oras/save/main (the owner's
# field save; Pomegrade's main_Startpoint reaches the field too but its top screen stays black, in Azahar as well).
#
#   tools/field_run.sh <scratch> <out dir> [seconds (default 260)] [game binary (default <scratch>/build_oras_mod/game)]
#
# The last top-screen frame is the field. Compare it with Azahar's shot taken at the same time of day
# (tools/field_compare.py): the game lights the field by the real clock. Frames and save stay in the scratch space.
set -uo pipefail
S=$1; out=$2; secs=${3:-260}; bin=${4:-$S/build_oras_mod/game}
input="110:start,110.3:-"
for t in $(seq 118 6 220); do input="$input,$t:a,$t.3:-"; done
rm -rf "$out"; mkdir -p "$out"
cd "$S" || exit 1
HOS_INPUT="$input" HOS_RENDER_DIR="$out" timeout -s KILL "$secs" "$bin" work/oras work/dumps/oras.3ds > "$out/log" 2>&1
ls "$out" | grep -c _1808C800 | sed 's/$/ top-screen frames/'
