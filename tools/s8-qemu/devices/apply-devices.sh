#!/bin/bash
# Agent E: copy the s8 display/input/sensor devices into a QEMU tree (hw/misc) and hook them
# into meson + Kconfig. Idempotent. Usage: apply-devices.sh <qemu-src>
set -e
SRC="${1:?qemu source dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
for f in s8-devices.h s8-devices.c s8-fb.c s8-hsi2c.c s8-i2c-devs.c s8-alive.c s8-speedy.c; do
  case "$f" in
    *.h) cp "$HERE/$f" "$SRC/include/hw/misc/$f" ;;
    *)   cp "$HERE/$f" "$SRC/hw/misc/$f" ;;
  esac
done
# the .c files include "s8-devices.h"; provide it next to them too
cp "$HERE/s8-devices.h" "$SRC/hw/misc/s8-devices.h"
if ! grep -q 's8-fb.c' "$SRC/hw/misc/meson.build"; then
  cat >> "$SRC/hw/misc/meson.build" <<'M'

system_ss.add(when: 'CONFIG_S8_A5XELTE', if_true: files(
  's8-devices.c', 's8-fb.c', 's8-hsi2c.c', 's8-i2c-devs.c', 's8-alive.c', 's8-speedy.c'))
M
fi
for sel in I2C FRAMEBUFFER; do
  if ! sed -n '/^config S8_A5XELTE/,/^$/p' "$SRC/hw/arm/Kconfig" | grep -q "select $sel\$"; then
    sed -i "/^config S8_A5XELTE/,/^\$/ s/^    select UNIMP\$/    select UNIMP\n    select $sel/" "$SRC/hw/arm/Kconfig"
  fi
done
echo devices applied
