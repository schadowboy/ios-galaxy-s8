cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
for p in 704 768 640 712 720; do
python3 -I tests/r8_dump.py data/capture21/final_res_106.bin 40 $p 79 $p /tmp/g$p.ppm
python3 -I tests/ppm2png.py /tmp/g$p.ppm logs/_tex_res106_p$p.png
done
