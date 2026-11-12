#!/bin/bash
# Build and ad-hoc sign libhook_log.dylib for iOS arm64 (run on macOS with Xcode).
#   userland/hook/build-hook.sh [OUT_DIR]      (default userland/hook/out)
# Needs: xcrun (iPhoneOS SDK) and ldid (LDID=/path/to/ldid, else "ldid" on PATH).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
LDID="${LDID:-ldid}"
mkdir -p "$OUT"

# hook_log.c is plain C against the ObjC runtime: no ARC, manual memory (it allocates nothing).
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -dynamiclib -O2 -Wall -Wextra \
	-framework Foundation -install_name /usr/lib/libhook_log.dylib \
	-o "$OUT/libhook_log.dylib" "$HERE/hook_log.c"

"$LDID" -S "$OUT/libhook_log.dylib"
file "$OUT/libhook_log.dylib" || true
xcrun --sdk iphoneos vtool -show-build "$OUT/libhook_log.dylib" 2>/dev/null | head -8 || true
xcrun --sdk iphoneos otool -L "$OUT/libhook_log.dylib" 2>/dev/null | head -8 || true
(cd "$OUT" && shasum -a 256 libhook_log.dylib | tee SHA256SUMS)
