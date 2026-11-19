# tests/stream_test.sh: replay capture17 sessions 8, 9, 10 (10 == the final_res_*.bin files), decode each with core/s8gfx_stream.h and print the checks.
# V=-v for the verbose draw list, SESS="8" for one session. Output files: /tmp/stream_s<N>.txt. Run in WSL: wsl -u root -d Ubuntu -- bash tests/stream_test.sh
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
for s in 8 9 10; do
  [ -d /tmp/s8s/s$s ] || python3 -I tests/replay_dump.py data/capture17 $s /tmp/s8s/s$s
done
gcc -O0 -ffp-contract=off -Wall -Icore -o /tmp/stream_decode tests/stream_decode.c || exit 1
g++ -O0 -Wall -Icore -fsyntax-only -x c++ core/s8gfx_stream.h 2>&1 | head -5
for s in ${SESS:-8 9 10}; do
  echo "=== session $s"
  /tmp/stream_decode /tmp/s8s/s$s $V > /tmp/stream_s$s.txt
  echo "exit $?"
  tail -5 /tmp/stream_s$s.txt
done
