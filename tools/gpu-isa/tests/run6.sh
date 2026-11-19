cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -include stdlib.h -I../core -o /tmp/isa_progs_ls isa_progs_ls.c && /tmp/isa_progs_ls ../data/scenes_raw.bin | tail -16
