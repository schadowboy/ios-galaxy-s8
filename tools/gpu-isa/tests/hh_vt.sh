# hh_vt.sh <cap> <session> <pass> <draw> [-t|-tf]: vertex/fragment trace via comp_replay on a snapshot
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
[ -d /tmp/s8s/$1/s$2 ] || python3 -I tests/replay_dump.py data/$1 $2 /tmp/s8s/$1/s$2
gcc -O1 -ffp-contract=off -Wall -Wno-misleading-indentation -Icore -o /tmp/comp_replay tests/comp_replay.c || exit 1
/tmp/comp_replay /tmp/s8s/$1/s$2 ${6} ${5:--t} $3 $4 -s 211
