cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/hh_vrun hh_vrun.c || exit 1
/tmp/hh_vrun /tmp/s8s/capture21/s9 "$@"
