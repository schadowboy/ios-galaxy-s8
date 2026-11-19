# r13.sh <capture:session> <pass> <draw>: build comp_replay and trace the first fragment pixel
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
n=${1%%:*}; s=${1##*:}
for c in capture17:8 capture17:10 capture18:11 capture18:14; do
  nn=${c%%:*}; ss=${c##*:}
  [ -d /tmp/s8s/$nn/s$ss ] || python3 -I tests/replay_dump.py data/$nn $ss /tmp/s8s/$nn/s$ss
done
gcc -O1 -ffp-contract=off -Wall -Wno-misleading-indentation -Icore -o /tmp/comp_replay tests/comp_replay.c || exit 1
/tmp/comp_replay /tmp/s8s/$n/s$s /tmp/s8s/capture17/s10 -tf $2 $3 -s 211
