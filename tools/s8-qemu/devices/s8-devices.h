/*
 * s8-qemu (Agent E): user-facing devices of the s8-a5xelte machine.
 * All base addresses are listed in addresses.md (keep both in sync).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef S8_DEVICES_H
#define S8_DEVICES_H

#include "hw/sysbus.h"
#include "system/memory.h"

#define TYPE_S8_FB        "s8-fb"
#define TYPE_S8_HSI2C     "s8-hsi2c"
#define TYPE_S8_S6SY761   "s8-s6sy761"
#define TYPE_S8_MAX77865_FG  "s8-max77865-fg"
#define TYPE_S8_MAX77865_CHG "s8-max77865-chg"
#define TYPE_S8_ALIVE     "s8-alive-gpio"
#define TYPE_S8_SPEEDY    "s8-speedy"

/* Exynos 8895 physical addresses (kernel patch 0124, loader shim/dreamlte). */
#define S8_FB_BASE        0xcc000000ULL  /* scan-out buffer in DRAM */
#define S8_FB_WIDTH       1440
#define S8_FB_HEIGHT      2960
#define S8_FB_PITCH       5760
#define S8_DECON_BASE     0x12860000ULL
#define S8_DECON_SIZE     0x1000
#define S8_HSI2C_TS_BASE  0x108e0000ULL  /* hsi2c_23: touch S6SY761 @0x48 */
#define S8_HSI2C_BAT_BASE 0x10970000ULL  /* hsi2c_32: MAX77865 FG @0x36, CHG @0x69 */
#define S8_HSI2C_SIZE     0x1000
#define S8_ALIVE_BASE     0x164b0000ULL  /* pinctrl ALIVE: GPA0/1/2 */
#define S8_ALIVE_SIZE     0x10000
#define S8_SPEEDY_BASE    0x15b50000ULL  /* SPEEDY: S2MPS17 PMIC + RTC */
#define S8_SPEEDY_SIZE    0x1000

#define S8_TS_ADDR        0x48
#define S8_FG_ADDR        0x36
#define S8_CHG_ADDR       0x69

/*
 * One call from the machine: creates and maps every device above into
 * 'sysmem' (priority 0, so an RAZ/WI window at lower priority is overridden).
 * 'crop': show only the centred 1125x2436 area the iOS UI occupies instead
 * of the whole 1440x2960 panel (touch coordinates follow).
 * Returns the framebuffer device. Needs RAM at fb_base (17 MB).
 */
DeviceState *s8_e_devices_create(MemoryRegion *sysmem, uint64_t fb_base,
                                uint64_t fb_size, bool crop);

#endif
