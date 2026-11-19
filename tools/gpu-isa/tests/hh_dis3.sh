cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/hh_dis isa_dis.c || exit 1
/tmp/hh_dis ../data/${2:-corpus_f21.bin} $1 full
