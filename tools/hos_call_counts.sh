#!/bin/bash
# The system calls and service requests our run of the game makes, counted (HOS_TRACE=1, src/hos/Kernel.c and Ipc.c),
# for tools/call_counts_compare.py against Azahar's (tools/azahar_trace_run.sh): what the game asks of one and not
# the other shows where src/hos/ answers differently (6 November: after the opening, the game decoding its title
# movie but drawing nothing).
#
#   tools/hos_call_counts.sh <game binary> <game dir> <rom> <seconds> <out counts.txt>
#
# Lines: "svc <id> <count>" and "ipc <service>:<request> <count>"; a file's or a directory's session (ours "(fs file)")
# named as Azahar's counts name them, ":<request>". Other variables ($HOS_RENDER_DIR...) are passed on.
set -uo pipefail
game=$1; dir=$2; rom=$3; secs=$4; out=$5
( HOS_TRACE=1 timeout -s INT "$secs" "$game" "$dir" "$rom" 2>&1 >/dev/null; true ) |
    awk '/^trace thread/ { if ($4 == "ipc" && $5 ~ /^\(/) k = "ipc :" substr($6, index($6, ":") + 1)
                           else if ($4 == "ipc") k = "ipc " $5; else k = "svc " $7; c[k]++ }
         END { for (x in c) print x, c[x] }' | sort > "$out"
wc -l < "$out" | sed 's/$/ kinds of calls/'
