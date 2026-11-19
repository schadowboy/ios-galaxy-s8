# build frag_forms with the current core and export data/corpus_comp.bin + the text listing
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
D=""
for c in capture17:8 capture17:10 capture18:1 capture18:9 capture18:10 capture18:11 capture18:12 capture18:13 capture18:14; do
  n=${c%%:*}; s=${c##*:}
  [ -d /tmp/s8s/$n/s$s ] || python3 -I tests/replay_dump.py data/$n $s /tmp/s8s/$n/s$s
  D="$D /tmp/s8s/$n/s$s"
done
gcc -O0 -Wall -Icore -o /tmp/frag_forms tests/frag_forms.c || exit 1
/tmp/frag_forms -o data/corpus_comp.bin $D > /tmp/frag_forms.txt
ls -la data/corpus_comp.bin
tail -90 /tmp/frag_forms.txt | head -50
