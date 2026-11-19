cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
bash tests/frag_forms.sh > /dev/null
python3 -I tests/tex_res.py /tmp/frag_forms.txt logs/sweetlog-emu3.txt logs/sweetlog-emu2.txt > /tmp/tex_res.txt
cp /tmp/frag_forms.txt logs/_frag_forms.txt
cp /tmp/tex_res.txt logs/_tex_res.txt
cat /tmp/tex_res.txt
