#!/bin/bash
# Usage: apply.sh <qemu-src>. Idempotent: copies our sources into the QEMU tree and hooks them into Kconfig/meson.
set -e
SRC="${1:?qemu source dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cp "$HERE/hw/arm/s8-a5xelte.c" "$SRC/hw/arm/s8-a5xelte.c"
if ! grep -q S8_A5XELTE "$SRC/hw/arm/Kconfig"; then
cat >> "$SRC/hw/arm/Kconfig" <<'K'

config S8_A5XELTE
    bool
    default y
    depends on TCG && AARCH64
    select ARM_GIC
    select PL011
    select UNIMP
K
fi
if ! grep -q s8-a5xelte "$SRC/hw/arm/meson.build"; then
# must come before hw_common_arch is consumed at the end of the file
sed -i "/^hw_arch += {'arm': arm_ss}/i arm_common_ss.add(when: 'CONFIG_S8_A5XELTE', if_true: files('s8-a5xelte.c'))" "$SRC/hw/arm/meson.build"
fi
# Extra device sources (Agents C/E): every qemu/hw/<dir>/*.c outside arm/ is copied into the
# same dir of the QEMU tree and added to that dir's meson.build under CONFIG_S8_A5XELTE.
for f in "$HERE"/hw/*/*.c; do
  [ -e "$f" ] || continue
  d=$(basename "$(dirname "$f")")
  [ "$d" = arm ] && continue
  b=$(basename "$f")
  cp "$f" "$SRC/hw/$d/$b"
  grep -q "'$b'" "$SRC/hw/$d/meson.build" || \
    echo "system_ss.add(when: 'CONFIG_S8_A5XELTE', if_true: files('$b'))" >> "$SRC/hw/$d/meson.build"
done
# Agent E devices (display/input/sensors): ../devices/apply-devices.sh
[ -x "$HERE/../devices/apply-devices.sh" ] || chmod +x "$HERE/../devices/apply-devices.sh" 2>/dev/null || true
bash "$HERE/../devices/apply-devices.sh" "$SRC"
echo applied
