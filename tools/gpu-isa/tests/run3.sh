cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -ffp-contract=off -I../core -o /tmp/corpus_run corpus_run.c -lm
/tmp/corpus_run ../data/corpus4.bin | tail -8
cat corpus_run.c | head -40
