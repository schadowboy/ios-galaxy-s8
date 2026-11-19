cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/isa_hist isa_hist.c || exit 1
/tmp/isa_hist ../data/corpus_comp.bin all
echo ---
for o in "$@"; do echo "== $o"; /tmp/isa_hist ../data/corpus_comp.bin $o | sort -k1 -n -r | head -${N:-60}; done
