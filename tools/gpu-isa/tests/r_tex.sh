cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
bash tests/comp_render.sh f21s9 capture21 9 -- -all -v > /tmp/r_tex.out 2>&1
cat /tmp/r_tex.out | head -20
grep -E "^  draw|^== pass" logs/_render_f21s9.txt | cut -c1-230 | sed -n 20,50p
