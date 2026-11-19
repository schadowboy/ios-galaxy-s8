cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
bash tests/comp_replay.sh > /dev/null 2>&1
gcc -O1 -ffp-contract=off -Wall -Wno-misleading-indentation -Icore -o /tmp/comp_replay tests/comp_replay.c || exit 1
/tmp/comp_replay /tmp/s8s/capture17/s8 /tmp/s8s/capture17/s10 ${4:--t} ${1:-1} ${2:-0} | grep -E 'TRACE|^        [0-9a-f]{3}:' | head -${3:-60}
