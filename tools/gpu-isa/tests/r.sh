cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
python3 -I tests/replay.py ${CAP:-data/capture16} "$@"
