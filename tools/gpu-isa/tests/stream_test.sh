# tests/stream_test.sh: replay capture sessions, decode each with core/s8gfx_stream.h and print the checks.
# env: CAP=data/capture17 (default; data/capture18 has session 1 intact), SESS="8 9 10" (sessions), V=-v (verbose draw list).
# Output: /tmp/stream_<capname>_s<N>.txt. Run in WSL: wsl -u root -d Ubuntu -- bash tests/stream_test.sh
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
CAP=${CAP:-data/capture17}
N=$(basename $CAP)
SESS=${SESS:-8 9 10}
# MERGE="/tmp/s8s/capture17/s10": extra snapshot dirs whose program heaps fill the zero bytes of the primary memory
for m in $MERGE; do d=${m#/tmp/s8s/}; [ -d $m ] || python3 -I tests/replay_dump.py data/${d%%/*} ${d##*/s} $m; done
for s in $SESS; do
  [ -d /tmp/s8s/$N/s$s ] || python3 -I tests/replay_dump.py $CAP $s /tmp/s8s/$N/s$s
done
gcc -O0 -ffp-contract=off -Wall -Icore -o /tmp/stream_decode tests/stream_decode.c || exit 1
g++ -O0 -Wall -Icore -fsyntax-only -x c++ core/s8gfx_stream.h 2>&1 | head -5
for s in $SESS; do
  echo "=== $N session $s"
  /tmp/stream_decode /tmp/s8s/$N/s$s $MERGE $V > /tmp/stream_${N}_s$s.txt
  echo "exit $?"
  grep -E '^TOTAL|^fragment programs|^vertex buffers|^passes' /tmp/stream_${N}_s$s.txt
done
