#!/bin/bash
# A headless Azahar run (Pomegrade's run_local.sh) counting every system call and service request the game makes
# (tools/azahar_trace_preload.c), for tools/hos_calls.py: <out>/counts.txt, and the run's log in <out>/log.txt.
#
#   tools/azahar_trace_run.sh <Pomegrade> <work> <out> <seconds> "<retro_host script>"   (POMEGRADE_GAME=dumps/x.3ds: X)
set -uo pipefail
pomegrade=$1; work=$2; out=$3; secs=$4; script=$5
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
lib="$out/azahar_trace_preload.so"
cc -shared -fPIC -O1 -Wall -o "$lib" "$here/azahar_trace_preload.c" -ldl -lpthread || exit 1
name=trace_$(basename "$out")
PKM_TRACE_DIR="$out" LD_PRELOAD="$lib" timeout "$secs" \
    "$pomegrade/tools/remake/headless/run_local.sh" "$work" "$name" - "" "$script" "$secs" > "$out/run.txt" 2>&1
status=$?
cp "$work/runs/$name/log.txt" "$out/log.txt" 2>/dev/null
grep -h "azahar_trace_preload" "$out/run.txt" "$out/log.txt" 2>/dev/null | head -2
echo "run ended ($status): $out/counts.txt"
