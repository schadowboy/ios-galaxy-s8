cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/hh_start hh_start.c || exit 1
/tmp/hh_start /tmp/s8s/capture21/s9 "$@"
