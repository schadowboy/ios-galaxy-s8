#!/bin/bash
# Build and ad-hoc sign s8mali_test for iOS arm64 (run on macOS with Xcode).
#   userland/build-ios.sh [OUT_DIR]      (default userland/out)
# Needs: xcrun (iPhoneOS SDK), and ldid (LDID=/path/to/ldid, else "ldid" on PATH).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
LDID="${LDID:-ldid}"
mkdir -p "$OUT"

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "build-ios: SDK $SDK"
xcrun --sdk iphoneos clang --version | head -1

# IOKit is private on iOS: s8mali_drm.c declares the few functions it needs itself and links the
# SDK's IOKit.framework (a stub .tbd). Find it, else fail with the evidence.
FW=""
for d in "$SDK/System/Library/Frameworks" "$SDK/System/Library/PrivateFrameworks"; do
	if [ -e "$d/IOKit.framework/IOKit.tbd" ] || [ -e "$d/IOKit.framework/Versions/A/IOKit.tbd" ]; then FW="$d"; break; fi
done
if [ -z "$FW" ]; then
	echo "build-ios: IOKit.framework not found in the SDK; listing:"
	ls "$SDK/System/Library/Frameworks" | grep -i iokit || true
	ls "$SDK/System/Library/PrivateFrameworks" | grep -i iokit || true
	exit 1
fi
echo "build-ios: IOKit.framework in $FW"

xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra \
	-F"$FW" -framework IOKit \
	-o "$OUT/s8mali_test" "$HERE/s8mali/s8mali_test.c" "$HERE/s8mali/s8mali_drm.c"

"$LDID" -S"$HERE/s8mali/s8mali_test.entitlements" "$OUT/s8mali_test"
"$LDID" -e "$OUT/s8mali_test" | head -20
file "$OUT/s8mali_test" || true
xcrun --sdk iphoneos vtool -show-build "$OUT/s8mali_test" 2>/dev/null | head -8 || true
(cd "$OUT" && shasum -a 256 s8mali_test | tee SHA256SUMS)
