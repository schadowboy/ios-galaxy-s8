cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
[ -d /tmp/s8s/capture21/s9 ] || python3 -I tests/replay_dump.py data/capture21 9 /tmp/s8s/capture21/s9
gcc -O0 -ffp-contract=off -Wall -Icore -o /tmp/stream_decode tests/stream_decode.c || exit 1
/tmp/stream_decode /tmp/s8s/capture21/s9 -v > /tmp/sd9.txt
awk '/^pass 8:/{f=1} /^pass 9:/{f=0} f' /tmp/sd9.txt | head -${1:-40}
