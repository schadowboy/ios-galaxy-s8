cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
for a in 0 1 2 3 4 5 6 7; do for b in 0 1 2 3 4 5 6 7; do
  gcc -O1 -ffp-contract=off -DS8F_CC_SEARCH -DS8F_CC56=$a -DS8F_CC36=$b -I../core -o /tmp/cc_$a$b comp_run.c -lm 2>/dev/null || continue
  for tex in ffbf4080 80bf4080; do
    echo "cc56=$a cc36=$b tex=$tex: $(HH_START=40 HH_TILE=ff996633 HH_TEX=$tex /tmp/cc_$a$b ../data/corpus_f21.bin -const -t 6 | grep -E '^program +6 ' | sed 's/.*rgba//')"
  done
done; done
