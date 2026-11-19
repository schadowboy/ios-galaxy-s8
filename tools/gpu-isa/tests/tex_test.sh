cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O1 -Wall -I../core tex_test.c -o /tmp/tex_test -lm && /tmp/tex_test ../data/capture9
