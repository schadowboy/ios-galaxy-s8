cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
bash tests/comp_replay.sh > /dev/null 2>&1
echo "pass1 draw0 (capture17 s8): transform push 0x100a77c22, 64 B; vertex buffer 0x100a77c80"
python3 -I tests/hx.py /tmp/s8s/capture17/s8/res_103.bin 7c00 b0
