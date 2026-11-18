/*
 * s8_e_devices_create(): instantiate all Agent E devices on the system bus.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "s8-devices.h"

void s8_chg_link_fg(DeviceState *chg, DeviceState *fg);

/* Centred area the iOS UI (1125x2436) occupies on the 1440x2960 panel. */
#define CROP_X ((S8_FB_WIDTH - 1125) / 2)
#define CROP_Y ((S8_FB_HEIGHT - 2436) / 2)

static DeviceState *map(MemoryRegion *sysmem, DeviceState *dev, int idx,
                        hwaddr base)
{
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion(sysmem, base,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), idx));
    return dev;
}

DeviceState *s8_e_devices_create(MemoryRegion *sysmem, uint64_t fb_base,
                                uint64_t fb_size, bool crop)
{
    DeviceState *fb, *alive, *i2c_ts, *i2c_bat, *ts, *fg, *chg, *speedy;
    uint32_t cx = crop ? CROP_X : 0, cy = crop ? CROP_Y : 0;
    uint32_t cw = crop ? 1125 : 0, ch = crop ? 2436 : 0;

    fb = qdev_new(TYPE_S8_FB);
    qdev_prop_set_uint64(fb, "fb-base", fb_base);
    qdev_prop_set_uint64(fb, "fb-size", fb_size);
    qdev_prop_set_uint32(fb, "crop-x", cx);
    qdev_prop_set_uint32(fb, "crop-y", cy);
    qdev_prop_set_uint32(fb, "crop-w", cw);
    qdev_prop_set_uint32(fb, "crop-h", ch);
    map(sysmem, fb, 0, S8_DECON_BASE);

    alive = qdev_new(TYPE_S8_ALIVE);
    map(sysmem, alive, 0, S8_ALIVE_BASE);

    i2c_ts = qdev_new(TYPE_S8_HSI2C);
    map(sysmem, i2c_ts, 0, S8_HSI2C_TS_BASE);
    ts = DEVICE(i2c_slave_new(TYPE_S8_S6SY761, S8_TS_ADDR));
    qdev_prop_set_uint32(ts, "crop-x", cx);
    qdev_prop_set_uint32(ts, "crop-y", cy);
    qdev_prop_set_uint32(ts, "crop-w", cw);
    qdev_prop_set_uint32(ts, "crop-h", ch);
    i2c_slave_realize_and_unref(I2C_SLAVE(ts),
                                (I2CBus *)qdev_get_child_bus(i2c_ts, "i2c"),
                                &error_fatal);
    qdev_connect_gpio_out_named(ts, "int", 0,
                                qdev_get_gpio_in_named(alive, "gpa1-0", 0));

    i2c_bat = qdev_new(TYPE_S8_HSI2C);
    map(sysmem, i2c_bat, 0, S8_HSI2C_BAT_BASE);
    fg = DEVICE(i2c_slave_create_simple(
        (I2CBus *)qdev_get_child_bus(i2c_bat, "i2c"), TYPE_S8_MAX77865_FG,
        S8_FG_ADDR));
    chg = DEVICE(i2c_slave_create_simple(
        (I2CBus *)qdev_get_child_bus(i2c_bat, "i2c"), TYPE_S8_MAX77865_CHG,
        S8_CHG_ADDR));
    s8_chg_link_fg(chg, fg);

    speedy = qdev_new(TYPE_S8_SPEEDY);
    map(sysmem, speedy, 0, S8_SPEEDY_BASE);
    return fb;
}
