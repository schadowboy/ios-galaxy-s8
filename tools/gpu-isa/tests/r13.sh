cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
[ -d /tmp/s8s/capture21/s9 ] || python3 -I tests/replay_dump.py data/capture21 9 /tmp/s8s/capture21/s9
gcc -O1 -ffp-contract=off -Wall -Wno-misleading-indentation -Icore -o /tmp/comp_replay tests/comp_replay.c || exit 1
if [ "${4:-}" = "-v" ]; then /tmp/comp_replay /tmp/s8s/capture21/s9 -v | awk -v p="== pass ${1:-0} " 'index($0,p){f=1;next} /^== pass/{f=0} f' | head -${3:-30}; exit; fi
/tmp/comp_replay /tmp/s8s/capture21/s9 ${4:--tf} ${1:-0} ${2:-1} | grep -E 'TRACE|^        [0-9a-f]{3}:|draw ${2:-1} ' | head -${3:-60}
