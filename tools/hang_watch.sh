#!/bin/bash
# The game run again and again until it hangs (its GPU command lists stop while the screens' VBlanks go on), then its
# state taken: the game's threads (SIGUSR1: src/hos/Thread.c's dump, priority, state, what each waits on) and the host
# stacks (gdb), so that a hang that comes once in ten runs is caught in the act (6 November: the opening's freeze).
#
#   tools/hang_watch.sh <game binary> <game dir> <rom> <out dir> [runs (10)] [seconds per run (100)]
#
# Each run's log is <out>/run_<k>.log; on a hang, <out>/hang_threads.txt and <out>/hang_stacks.txt, and the script
# stops. A hang: the list count unchanged over the last 4 progress lines (each line every 300 VBlanks, 5 s): a shader
# being compiled stalls a few seconds, not 15. Other variables ($HOS_RENDER_DIR...) are passed on to the game.
set -u
game=$1; dir=$2; rom=$3; out=$4; runs=${5:-10}; seconds=${6:-100}
mkdir -p "$out"
for k in $(seq 1 "$runs"); do
  log=$out/run_$k.log
  "$game" "$dir" "$rom" > "$log" 2>&1 &
  pid=$!
  for t in $(seq 1 "$seconds"); do
    sleep 1
    kill -0 $pid 2>/dev/null || break
    lines=$(grep -c 'command lists' "$log")
    distinct=$(grep 'command lists' "$log" | tail -4 | awk '{print $2}' | sort -u | wc -l)
    if [ "$lines" -ge 5 ] && [ "$distinct" -eq 1 ]; then
      echo "run $k: hung at $(grep 'command lists' "$log" | tail -1 | awk '{print $2}') command lists"
      before=$(wc -l < "$log")
      kill -USR1 $pid; sleep 1
      tail -n +$((before + 1)) "$log" | sed -n '/threads (/,/^[^ ]/p' > "$out/hang_threads.txt"
      gdb -p $pid -batch -ex 'thread apply all bt 30' > "$out/hang_stacks.txt" 2>&1
      kill -9 $pid
      exit 1
    fi
  done
  kill -INT $pid 2>/dev/null; wait $pid 2>/dev/null
  echo "run $k: no hang ($(grep 'command lists' "$log" | tail -1 | awk '{print $2}') command lists)"
done
exit 0
