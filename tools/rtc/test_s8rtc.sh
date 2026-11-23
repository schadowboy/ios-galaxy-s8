#!/bin/bash
# Host compile + run of the pure epoch<->RTC register functions of S8Rtc.cpp.
# Run: wsl -u root -d Ubuntu -- bash /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/rtc/test_s8rtc.sh
set -e
cd "$(dirname "$0")"
D=$(mktemp -d)
g++ -std=gnu++17 -Wall -Wextra -Werror -O2 -o "$D/t" test_s8rtc.cpp
"$D/t"
rm -rf "$D"
