cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
mkdir -p /tmp/passt
gcc -O1 -Wall -o /tmp/passt/pass_test tests/pass_test.c || exit 1
# compositor (capture16): replay res_63 (the pass descriptor ring) as of session $1
for s in ${SESSIONS:-3 4 8 10}; do
  python3 -I tests/replay.py data/capture16 $s res_63 0 0 /tmp/passt/c16_s$s.bin > /dev/null
  echo "=== capture16 session $s res_63 (VA 0x100650000)"
  /tmp/passt/pass_test /tmp/passt/c16_s$s.bin 100650000
done
# my test passes (capture9): the pass descriptor region res_18 as of session 31 (and the target-only form)
python3 -I tests/replay.py data/capture9 31 res_18 0 0 /tmp/passt/c9_s31.bin > /dev/null
echo "=== capture9 session 31 res_18 (VA 0x100100000), test pass 64x64"
/tmp/passt/pass_test /tmp/passt/c9_s31.bin 100100000
