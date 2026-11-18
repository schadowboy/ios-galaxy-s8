#!/bin/bash
# Copy the exynos-ufs device (+ optional test machine) into a QEMU 10.2.1 source tree and
# add the meson/Kconfig hooks. Idempotent. Usage: apply-ufs.sh <qemu-src-dir> [--test-machine]
set -e
SRC=${1:?qemu source dir}
HERE=$(cd "$(dirname "$0")/.." && pwd)
cp "$HERE/hw/misc/ufs-exynos.c" "$SRC/hw/block/ufs-exynos.c"
if ! grep -q CONFIG_EXYNOS_UFS "$SRC/hw/block/meson.build"; then
  echo "system_ss.add(when: 'CONFIG_EXYNOS_UFS', if_true: files('ufs-exynos.c'))" >> "$SRC/hw/block/meson.build"
fi
if ! grep -q "config EXYNOS_UFS" "$SRC/hw/block/Kconfig"; then
  printf '\nconfig EXYNOS_UFS\n    bool\n' >> "$SRC/hw/block/Kconfig"
fi
if [ "$2" = "--test-machine" ]; then
  cp "$HERE/hw/arm/s8-ufs-test.c" "$SRC/hw/arm/s8-ufs-test.c"
  if ! grep -q s8-ufs-test "$SRC/hw/arm/meson.build"; then
    echo "arm_common_ss.add(when: 'CONFIG_S8_UFS_TEST', if_true: files('s8-ufs-test.c'))" >> "$SRC/hw/arm/meson.build"
  fi
  if ! grep -q "config S8_UFS_TEST" "$SRC/hw/arm/Kconfig"; then
    printf '\nconfig S8_UFS_TEST\n    bool\n    default y\n    depends on TCG && AARCH64\n    select PL011\n    select EXYNOS_UFS\n' >> "$SRC/hw/arm/Kconfig"
  fi
fi
# when the s8-a5xelte machine exists it must 'select EXYNOS_UFS' in hw/arm/Kconfig
echo applied
