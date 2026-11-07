#!/bin/bash
# A game made ready to run from the player's cartridge (docs/AGENT_HANDOFF.md, "Setting up a fresh session", step 3):
# its code and modules extracted (tools/n3ds_extract.py), the save put in place, the system font and the three system
# data archives made from Azahar's open-source replacements, and the console configuration copied from an Azahar run
# (without it the game faults at boot: cfg's requests have no answer). Everything is written outside the repository.
#
#   tools/prepare_game.sh <game.3ds> <out dir> <save main> <save metadata> <Pomegrade> [azahar run dir]
#
# <save main>: the game's own save (never another game's: X with Omega Ruby's save showed only a menu background).
# <azahar run dir>: any run of Pomegrade's run_local.sh (its Azahar/nand/.../sysdata/00010017/00000000/config); left
# out, the config is not made and the script says so.
set -euo pipefail
game=$1; out=$2; save=$3; metadata=$4; pomegrade=$5; run=${6:-}
repo=$(cd "$(dirname "$0")/.." && pwd)
python3 "$repo/tools/n3ds_extract.py" "$game" "$out" | tail -1
mkdir -p "$out/save"
cp "$save" "$out/save/main"
cp "$metadata" "$out/save.metadata"
archives=$pomegrade/azahar/externals/open_source_archives/include
python3 "$repo/tools/make_shared_font.py" "$archives/shared_font.app.romfs.h" "$out/shared_font.bin"
python3 "$repo/tools/make_system_archive.py" "$archives/bad_word_list.app.romfs.h" "$out" 000400DB00010302
python3 "$repo/tools/make_system_archive.py" "$archives/mii.app.romfs.h" "$out" 0004009B00010202
python3 "$repo/tools/make_system_archive.py" "$archives/country_list.app.romfs.h" "$out" 0004009B00010402
config=$(find "${run:-/nonexistent}" -path '*sysdata/00010017/00000000/config' 2>/dev/null | head -1)
if [ -n "$config" ]; then cp "$config" "$out/config"; echo "config from $config"
else echo "no console config: give an Azahar run dir (the game faults at boot without it)"; fi
