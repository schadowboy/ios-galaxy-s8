cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/isa_dump isa_dump.c
/tmp/isa_dump ../data/corpus4.bin f
gcc -O0 -ffp-contract=off -I../core -o /tmp/corpus_frag corpus_frag.c -lm
/tmp/corpus_frag ../data/corpus4.bin
gcc -O0 -ffp-contract=off -I../core -o /tmp/corpus_run corpus_run.c -lm
/tmp/corpus_run ../data/corpus4.bin | tail -3
