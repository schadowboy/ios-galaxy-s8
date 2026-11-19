cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
python3 -I tests/crop.py logs/_frame_f21s9_before.png 1040 40 160 90 logs/_crop_status_before.png 4
python3 -I tests/crop.py logs/_frame_f21s9.png 1040 40 160 90 logs/_crop_status_after.png 4
python3 -I tests/crop.py logs/_frame_f21s9_before.png 440 2900 560 50 logs/_crop_dock_before.png 2
python3 -I tests/crop.py logs/_frame_f21s9.png 440 2900 560 50 logs/_crop_dock_after.png 2
