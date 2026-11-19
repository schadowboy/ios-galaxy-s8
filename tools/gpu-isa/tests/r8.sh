cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
for s in 11 14; do
  echo "== capture18 session $s, passes with a LINK start and complete state (live frame):"
  grep -E '^pass [0-9]+:' logs/_stream_capture18_s$s.txt | cut -c1-120 | head -30
  grep 'DL ' logs/_stream_capture18_s$s.txt | grep -oE 'vs [0-9a-f]+\[[a-z-]+\]' | sed 's/.*\[//' | sort | uniq -c
  grep 'DL ' logs/_stream_capture18_s$s.txt | grep -oE 'fs [0-9a-f]+\[[a-z-]+\]' | sed 's/.*\[//' | sort | uniq -c
done
