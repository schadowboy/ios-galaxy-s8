cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
grep -E 'type 9' data/capture21/index.txt | awk '{print $2,$3,$4,$5,$6,$7,$8}' | sort -u | head -20
python3 -I tests/hx.py data/capture21/final_res_105.bin 0 80
echo; grep -E 'res_105|res_106|res_125' /tmp/s8s/capture21/s9/regions.txt
python3 -I tests/hx.py /tmp/s8s/capture21/s9/res_105.bin 40 40
