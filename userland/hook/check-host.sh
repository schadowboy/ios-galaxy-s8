#!/bin/bash
# Host syntax check of libs8gl's sources with a Linux clang against the stub headers in stubs/ (no iOS SDK needed).
#   userland/hook/check-host.sh        (run in WSL / Linux; needs clang)
# This proves the C is well formed (-Wall -Wextra, -fsyntax-only); it proves nothing about the iOS build or run time.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
FLAGS=(-fsyntax-only -std=gnu11 -Wall -Wextra -Wno-unused-parameter -I"$HERE/stubs" -I"$HERE" -include "$HERE/stubs/compat.h")
for f in eagl_s8.c gl_osmesa_s8.c; do
	echo "check-host: $f"
	"$CC" "${FLAGS[@]}" "$HERE/$f"
done
echo "check-host: s8mali_drm.c"
"$CC" "${FLAGS[@]}" -I"$HERE/../s8mali" "$HERE/../s8mali/s8mali_drm.c"
echo "check-host: ok"
