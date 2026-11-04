#!/bin/sh
# The matching check of one whole game with everything it needs, so a run is one short command:
#   PKM_WORK=<work folder> tools/match_game.sh <oras|x> [match_functions.py options: --uses ldrex, --only 507F50...]
# The work folder (outside the repository: it holds the owner's game) has, per game, <game>/code.bin, exheader.bin and
# cro/ (tools/n3ds_extract.py) and runs/ram_<game>/ (the RAM dumps: docs/RECOMPILATION_PLAN.md §10, step 5).
set -e
game=$1
shift
: "${PKM_WORK:?set PKM_WORK to the work folder}"
case $game in oras|x) ;; *) echo "game: oras or x" >&2; exit 2 ;; esac
here=$(dirname "$0")
exec python3 "$here/match_functions.py" "$PKM_WORK/$game/code.bin" "$here/../symbols/${game}_functions.tsv" \
    --ram "$PKM_WORK/runs/ram_$game" --cro "$PKM_WORK/$game/cro" --exheader "$PKM_WORK/$game/exheader.bin" "$@"
