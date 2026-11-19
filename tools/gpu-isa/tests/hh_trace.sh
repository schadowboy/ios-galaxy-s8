cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O1 -g -ffp-contract=off -Wall -I../core -o /tmp/comp_run_t comp_run.c -lm || exit 1
/tmp/comp_run_t ../data/corpus_comp.bin -const -t $1 | grep -E "^    "
