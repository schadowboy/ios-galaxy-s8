cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
gcc -O0 -Wall -Icore -o /tmp/stream_decode tests/stream_decode.c
for s in 11 14; do
[ -d /tmp/s8s/capture18/s$s ] || python3 -I tests/replay_dump.py data/capture18 $s /tmp/s8s/capture18/s$s
/tmp/stream_decode /tmp/s8s/capture18/s$s /tmp/s8s/capture17/s10 -v > /tmp/o$s.txt
cp /tmp/o$s.txt logs/_stream_capture18_s$s.txt
grep -E '^TOTAL' /tmp/o$s.txt
done
grep -c FAIL /tmp/o11.txt
grep 'DL ' /tmp/o11.txt | grep -E 'FAIL|unwalked' | head -30
grep -B1 -A2 'FAIL' /tmp/o11.txt | grep 'vs prog' | sort | uniq -c | head
