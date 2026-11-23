# hh_vt2.sh <cap> <session> <pass> <draw>: vertex trace with the whole uniform file printed (patched copy of comp_replay.c in /tmp)
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
[ -d /tmp/s8s/$1/s$2 ] || python3 -I tests/replay_dump.py data/$1 $2 /tmp/s8s/$1/s$2
sed 's|for (z = 0; z < 12; z++) printf(" %08x", uni\[z\]);|for (z = 0; z < 64; z++) printf(" %08x", uni[z]);|' tests/comp_replay.c > /tmp/comp_replay_u.c
gcc -O1 -ffp-contract=off -Wno-misleading-indentation -Icore -Itests -o /tmp/comp_replay_u /tmp/comp_replay_u.c || exit 1
/tmp/comp_replay_u /tmp/s8s/$1/s$2 -t $3 $4 -s 211 | grep "TRACE vertex 0"
