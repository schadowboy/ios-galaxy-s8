cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -ffp-contract=off -I../core -o /tmp/isa_walk isa_walk.c -lm || exit 1
/tmp/isa_walk ../data/corpus4.bin f
/tmp/isa_walk ../data/corpus4.bin v | grep -v OK
