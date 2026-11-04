#!/bin/bash
# A headless Azahar run (Pomegrade's run_local.sh) with Pomegrade's code trace on (tools/azahar_trace_preload.c), every
# call kept: the trace holds the last 131,072 calls and rewrites its record every 30 s, far fewer calls than that apart,
# so each record written is copied as it comes (<out>/snapshots/NNNN/) and tools/hos_calls.py joins them, checking
# that each one overlaps the ones before (no call lost between two).
#
#   tools/azahar_trace_run.sh <Pomegrade> <work> <out> <seconds> "<retro_host script>"   (POMEGRADE_GAME=dumps/x.3ds: X)
set -uo pipefail
pomegrade=$1; work=$2; out=$3; secs=$4; script=$5
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/record" "$out/snapshots"
lib="$out/azahar_trace_preload.so"
cc -shared -fPIC -O1 -Wall -o "$lib" "$here/azahar_trace_preload.c" -ldl || exit 1
name=trace_$(basename "$out")
PKM_TRACE_DIR="$out/record" LD_PRELOAD="$lib" timeout "$secs" \
    "$pomegrade/tools/remake/headless/run_local.sh" "$work" "$name" - "" "$script" "$secs" > "$out/run.txt" 2>&1 &
run=$!
last=; n=0
copy() {
    f=$(ls -t "$out"/record/*/calls.txt 2>/dev/null | head -1)
    [ -n "$f" ] || return
    stamp=$(stat -c %Y.%s "$f")
    [ "$stamp" = "$last" ] && return
    sleep 2  # (the record is written in the background: let it finish)
    n=$((n + 1)); d=$(printf "%s/snapshots/%04d" "$out" $n); mkdir -p "$d"
    cp "$(dirname "$f")"/calls.txt "$(dirname "$f")"/summary.txt "$d"/
    last=$(stat -c %Y.%s "$f")
}
while kill -0 $run 2>/dev/null; do copy; sleep 3; done
wait $run; status=$?
copy
cp "$work/runs/$name/log.txt" "$out/log.txt" 2>/dev/null
echo "run ended ($status), $n records copied to $out/snapshots"
