cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O1 -g -fsanitize=address,undefined -ffp-contract=off -Wall -I../core -o /tmp/comp_run comp_run.c -lm || exit 1
/tmp/comp_run ../data/corpus_comp.bin "$@"
