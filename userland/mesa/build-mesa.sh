#!/bin/bash
# Cross-build Mesa 25.0.7 (OSMesa, ES profile; gallium panfrost + softpipe) for iOS arm64.
# Usage: build-mesa.sh <out-dir>     Env: MESA_TAG (mesa-25.0.7), LDID (path to ldid, optional), WORK (scratch dir)
# Host needs: xcrun with the iPhoneOS SDK, git, python3, ninja, bison >= 2.4 (not macOS's 2.3), flex, pkg-config.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)"
MESA_TAG="${MESA_TAG:-mesa-25.0.7}"
WORK="${WORK:-$HERE/work}"
LDID="${LDID:-ldid}"
mkdir -p "$WORK"; WORK="$(cd "$WORK" && pwd)"

# bison/flex from brew if present (macOS's bison is 2.3)
for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin /opt/homebrew/opt/flex/bin /usr/local/opt/flex/bin; do
  if [ -d "$d" ]; then PATH="$d:$PATH"; fi
done
export PATH
bison --version | head -1
flex --version | head -1

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
echo "SDK=$SDK"

# python tools in a venv
if [ ! -x "$WORK/venv/bin/meson" ]; then
  python3 -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" -q install meson mako pyyaml packaging
fi
export PATH="$WORK/venv/bin:$PATH"
meson --version

# source
SRC="$WORK/mesa"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
fi
git -C "$SRC" reset -q --hard
git -C "$SRC" clean -qfd
APPLIED=""
for p in "$HERE"/patches/*.patch; do
  echo "apply $(basename "$p")"
  git -C "$SRC" apply --whitespace=nowarn "$p"
  APPLIED="$APPLIED $(basename "$p")"
done
mkdir -p "$SRC/src/s8-drm"
cp "$HERE"/overlay/src/s8-drm/* "$SRC/src/s8-drm/"

# cross file + pkg-config for zlib (iOS SDK has zlib.h and libz.tbd but no .pc)
sed "s|@SDK@|$SDK|g" "$HERE/ios-arm64.cross" > "$WORK/ios-arm64.cross"
mkdir -p "$WORK/pc"
cat > "$WORK/pc/zlib.pc" <<PC
prefix=$SDK/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: zlib
Description: zlib (iPhoneOS SDK)
Version: 1.2.12
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
PC
export PKG_CONFIG_LIBDIR="$WORK/pc"
export PKG_CONFIG_PATH=""

OPTS="--buildtype=release -Dplatforms= -Dgallium-drivers=softpipe,panfrost -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false -Dgallium-opencl=disabled -Dgallium-rusticl=false"
B="$WORK/build"
rm -rf "$B"
# shellcheck disable=SC2086
meson setup "$B" "$SRC" --cross-file "$WORK/ios-arm64.cross" $OPTS 2>&1 | tee "$WORK/meson-setup.log" | tail -80
ninja -C "$B" 2>&1 | tee "$WORK/ninja.log" | tail -60

# collect
rm -rf "${OUT:?}"/*
mkdir -p "$OUT/lib" "$OUT/include/GL"
find "$B/src" \( -name 'libOSMesa*.dylib' -o -name 'libglapi*.dylib' -o -name 'libGLESv2*.dylib' \) -print
find "$B/src" -maxdepth 5 \( -name 'libOSMesa*.dylib' -o -name 'libglapi*.dylib' \) -exec cp -P {} "$OUT/lib/" \;
cp "$SRC/include/GL/osmesa.h" "$SRC/include/GL/gl.h" "$SRC/include/GL/glext.h" "$OUT/include/GL/" || true
cp -R "$SRC/include/GLES2" "$SRC/include/GLES3" "$SRC/include/KHR" "$OUT/include/" || true
if ! ls "$OUT"/lib/libOSMesa*.dylib >/dev/null 2>&1; then echo "no libOSMesa produced"; exit 1; fi

for f in "$OUT"/lib/*.dylib; do
  if [ -L "$f" ]; then continue; fi
  "$LDID" -S "$f"
done

{
  echo "Mesa tag: $MESA_TAG ($(git -C "$SRC" rev-parse --short HEAD))"
  echo "Date: $(date -u +%FT%TZ)"
  echo "SDK: $SDK"
  echo "meson options: $OPTS"
  echo "cross file: ios-arm64.cross (clang -arch arm64 -miphoneos-version-min=16.0)"
  echo "patches applied:$APPLIED"
  echo "overlay: src/s8-drm (xf86drm.h stand-in + weak failing s8_drm_stub.c)"
  echo "--- sizes"
  ls -l "$OUT/lib"
  echo "--- otool -L"
  for f in "$OUT"/lib/*.dylib; do
    if [ -L "$f" ]; then continue; fi
    echo "# $f"; xcrun otool -L "$f"
  done
  echo "--- nm -gU (OSMesa, gl*, drm)"
  for f in "$OUT"/lib/*.dylib; do
    if [ -L "$f" ]; then continue; fi
    echo "# $f"; xcrun nm -gU "$f" | grep -E "OSMesa|_gl[A-Z]|_drm|hakios" | head -80 || true
  done
  echo "--- sha256"
  (cd "$OUT" && shasum -a 256 lib/*.dylib)
} > "$OUT/build-info.txt"
cp "$WORK/meson-setup.log" "$OUT/meson-setup.log" || true
cat "$OUT/build-info.txt"
