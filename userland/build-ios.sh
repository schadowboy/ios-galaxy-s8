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

# s8gl_test: a triangle through Mesa (libOSMesa.8.dylib, loaded at run time with dlopen). It links the same DRM shim, and
# exports its symbols (-export_dynamic) so that the weak, failing drm* stand-ins inside libOSMesa are replaced by the
# real ones when Mesa is loaded.
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall -Wextra \
	-F"$FW" -framework IOKit -Wl,-export_dynamic \
	-o "$OUT/s8gl_test" "$HERE/s8mali/s8gl_test.c" "$HERE/s8mali/s8mali_drm.c"
"$LDID" -S"$HERE/s8mali/s8mali_test.entitlements" "$OUT/s8gl_test"
file "$OUT/s8gl_test" || true
# s8mtl_test: does Metal create a device on the AGX stand-in (test scaffolding, no IOKit shim needed)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -fobjc-arc -Wall \
	-framework Foundation -framework Metal \
	-o "$OUT/s8mtl_test" "$HERE/s8mali/s8mtl_test.m"
"$LDID" -S"$HERE/s8mali/s8mali_test.entitlements" "$OUT/s8mtl_test"
file "$OUT/s8mtl_test" || true
# s8pm_test: real restart / power off through the reboot() system call (test scaffolding)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -Wall \
	-o "$OUT/s8pm_test" "$HERE/s8mali/s8pm_test.c"
"$LDID" -S"$HERE/s8mali/s8mali_test.entitlements" "$OUT/s8pm_test"
(cd "$OUT" && shasum -a 256 s8mali_test s8gl_test s8mtl_test s8pm_test | tee SHA256SUMS)
# libs8gl.dylib (EAGLContext replacement over Mesa, userland/hook): built by hook/build-hook.sh; BUILD_HOOK=1 does it from here too
if [ "${BUILD_HOOK:-0}" = 1 ]; then bash "$HERE/hook/build-hook.sh" "$OUT/hook"; fi
