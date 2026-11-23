#!/bin/bash
# Assemble xnu/patches/0151-s8-rtc-writeback.patch from S8Rtc.cpp (new file hunk) and 0151-hooks.part (the two small hooks),
# then dry-run it against a scratch tree built from the recovered post-0127 IOPlatformExpert.cpp and a files.arm64 excerpt.
# Run: wsl -u root -d Ubuntu -- bash /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/rtc/gen-patch.sh
set -e
W=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress
SRC=$W/tools/rtc/S8Rtc.cpp
OUT=$W/xnu/patches/0151-s8-rtc-writeback.patch
N=$(wc -l < "$SRC")
{
  echo "diff --git a/iokit/Kernel/arm/S8Rtc.cpp b/iokit/Kernel/arm/S8Rtc.cpp"
  echo "new file mode 100644"
  echo "index 0000000..1111111"
  echo "--- /dev/null"
  echo "+++ b/iokit/Kernel/arm/S8Rtc.cpp"
  echo "@@ -0,0 +1,$N @@"
  sed 's/^/+/' "$SRC"
  cat "$W/tools/rtc/0151-hooks.part"
} > "$OUT"
echo "wrote $OUT ($N lines of S8Rtc.cpp)"

# dry run on a scratch tree
T=$(mktemp -d)
mkdir -p "$T/iokit/Kernel" "$T/iokit/conf"
cp /mnt/c/github/ios-s8/tools/s8recov-work/IOPlatformExpert.cpp "$T/iokit/Kernel/IOPlatformExpert.cpp"
cat > "$T/iokit/conf/files.arm64" <<'EOF'
iokit/Kernel/arm/SweetSurface.cpp                                optional iokitcpp
iokit/Kernel/arm/SweetFramebuffer.cpp                            optional iokitcpp
iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp
iokit/Kernel/arm/S8Mali.cpp                                      optional iokitcpp
iokit/Kernel/arm/S8Agx.cpp                                       optional iokitcpp
iokit/Kernel/arm/hid/SweetHID.cpp                                optional iokitcpp
iokit/Kernel/arm/hid/IOHIDEvent.cpp                              optional iokitcpp
iokit/Kernel/arm/hid/IOHIDEventServiceQueue.cpp                  optional iokitcpp
EOF
cd "$T"
patch -p1 --dry-run < "$OUT"
patch -p1 < "$OUT" > /dev/null
diff -u /mnt/c/github/ios-s8/tools/s8recov-work/IOPlatformExpert.cpp iokit/Kernel/IOPlatformExpert.cpp || true
cmp iokit/Kernel/arm/S8Rtc.cpp "$SRC" && echo "S8Rtc.cpp identical to the source"
cat iokit/conf/files.arm64
rm -rf "$T"
