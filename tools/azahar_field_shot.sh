#!/bin/bash
# Azahar's picture of the field, for tools/field_compare.py (docs/AGENT_HANDOFF.md, step 4): a headless run of
# Pomegrade's run_local.sh (read only) from the given save, one frame in 15 drawn until the field is up (or 300 s of
# game time), then 120 s more, then a shot: <work>/runs/<name>/f1.png. About 5 minutes. Start it at the same time as
# tools/field_run.sh: the game lights the field by the real clock. A black shot: Azahar's field was not up yet.
#
#   tools/azahar_field_shot.sh <Pomegrade> <work> <name> <save main>
set -euo pipefail
pomegrade=$1; work=$(cd "$2" && pwd); name=$3; save=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
POMEGRADE_SAVE=$save "$pomegrade/tools/remake/headless/run_local.sh" "$work" "$name" - "" \
    "draw every 15;field 300;wait 120;draw on;wait 8;shot f1" 2400 > /dev/null 2>&1 || true
ls -l "$work/runs/$name/f1.png"
