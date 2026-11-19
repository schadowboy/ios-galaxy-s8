# tests/comp_replay.sh: host reproduction of execComposite() on capture17 session 8 and capture18 session 11 (program heaps merged from capture17 session 10).
# env: V=-v for vertex detail, STEP=N fragment sampling stride. Output /tmp/comp_<name>.txt and logs/_comp_*.txt. Run in WSL: wsl -u root -d Ubuntu -- bash tests/comp_replay.sh
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
for c in capture17:8 capture17:10 capture18:11 capture18:14; do
  n=${c%%:*}; s=${c##*:}
  [ -d /tmp/s8s/$n/s$s ] || python3 -I tests/replay_dump.py data/$n $s /tmp/s8s/$n/s$s
done
[ -d /tmp/s8s/capture17/s10 ] || python3 -I tests/replay_dump.py data/capture17 10 /tmp/s8s/capture17/s10
gcc -O1 -ffp-contract=off -Wall -Wno-misleading-indentation -Icore -o /tmp/comp_replay tests/comp_replay.c || exit 1
for c in capture17:8 capture18:11 capture18:14; do
  n=${c%%:*}; s=${c##*:}
  /tmp/comp_replay /tmp/s8s/$n/s$s /tmp/s8s/capture17/s10 $V -s ${STEP:-211} > /tmp/comp_${n}_s$s.txt
  cp /tmp/comp_${n}_s$s.txt logs/_comp_${n}_s$s.txt
  echo "== $n session $s"; grep -A3 '^SUMMARY' /tmp/comp_${n}_s$s.txt
done
