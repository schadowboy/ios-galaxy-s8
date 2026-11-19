cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
D=/tmp/s8s/capture21/s9
[ -d $D ] || python3 -I tests/replay_dump.py data/capture21 9 $D
gcc -O0 -Wall -Icore -o /tmp/frag_forms tests/frag_forms.c || exit 1
/tmp/frag_forms -o data/corpus_f21.bin $D > /tmp/ff21.txt
grep -n "^### program" /tmp/ff21.txt | head -60
