cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
gcc -O0 -I../core -o /tmp/isa_dump isa_dump.c
/tmp/isa_dump ../data/corpus4.bin v all > /tmp/vert_all.txt
sed -n '/== 0 /,/== 1 /p' /tmp/vert_all.txt | head -24
sed -n '/== 33 /,/== 34 /p' /tmp/vert_all.txt | head -8
