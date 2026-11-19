cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -ffp-contract=off -Wall -I../core -o /tmp/isa_test isa_test.c -lm && /tmp/isa_test ../data/corpus4.bin | head -40
