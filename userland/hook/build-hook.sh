#!/bin/bash
# Build and ad-hoc sign libhook_log.dylib and libs8gl.dylib for iOS arm64 (run on macOS with Xcode).
#   userland/hook/build-hook.sh [OUT_DIR]      (default userland/hook/out)
# Needs: xcrun (iPhoneOS SDK) and ldid (LDID=/path/to/ldid, else "ldid" on PATH).
#
# libs8gl.dylib = eagl_s8.c (EAGLContext replacement + generated gl table eagl_gen_s8.h) + gl_osmesa_s8.c (Mesa OSMesa through
# dlopen at run time: no Mesa header or library is needed to build) + ../s8mali/s8mali_drm.c (the S8Mali DRM shim given to Mesa).
# It links only libSystem, libobjc, CoreFoundation and the SDK's IOKit stub; IOSurface is reached with dlsym.
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

# IOKit is private on iOS: s8mali_drm.c declares the few functions it needs itself and links the SDK's IOKit.framework
# (a stub .tbd). Find it, else fail with the evidence (same search as userland/build-ios.sh).
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
FW=""
for d in "$SDK/System/Library/Frameworks" "$SDK/System/Library/PrivateFrameworks"; do
	if [ -e "$d/IOKit.framework/IOKit.tbd" ] || [ -e "$d/IOKit.framework/Versions/A/IOKit.tbd" ]; then FW="$d"; break; fi
done
if [ -z "$FW" ]; then
	echo "build-hook: IOKit.framework not found in the SDK; listing:"
	ls "$SDK/System/Library/Frameworks" | grep -i iokit || true
	ls "$SDK/System/Library/PrivateFrameworks" | grep -i iokit || true
	exit 1
fi

# libs8gl: -fvisibility=hidden keeps the shim's drm* functions and everything else private to the library (the constructor in
# eagl_s8.c is what runs; nothing else is meant to be called from outside).
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -dynamiclib -O2 -Wall -Wextra -Wno-unused-parameter \
	-fvisibility=hidden -I"$HERE" -F"$FW" \
	-framework CoreFoundation -framework IOKit -lobjc -install_name /usr/lib/libs8gl.dylib \
	-o "$OUT/libs8gl.dylib" "$HERE/eagl_s8.c" "$HERE/ca_function_s8.c" "$HERE/gl_osmesa_s8.c" "$HERE/../s8mali/s8mali_drm.c"

"$LDID" -S "$OUT/libs8gl.dylib"
file "$OUT/libs8gl.dylib" || true
xcrun --sdk iphoneos vtool -show-build "$OUT/libs8gl.dylib" 2>/dev/null | head -8 || true
xcrun --sdk iphoneos otool -L "$OUT/libs8gl.dylib" 2>/dev/null | head -12 || true
(cd "$OUT" && shasum -a 256 libhook_log.dylib libs8gl.dylib | tee SHA256SUMS)
