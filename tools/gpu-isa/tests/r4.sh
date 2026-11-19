cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
for s in 8 9 10; do
  python3 -I tests/replay_dump.py data/capture17 $s /tmp/s8s/s$s
done
ls /tmp/s8s/s8 | head -5
