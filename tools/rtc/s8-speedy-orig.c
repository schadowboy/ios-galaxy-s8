/*
 * s8-speedy: Exynos SPEEDY master (0x15b50000) with the S2MPS17 PMIC (slave 1)
 * and its RTC (slave 2) behind it. Only the real-hardware loader
 * (shim/dreamlte/pmic.c) uses it; the kernel takes the time from boot-arg
 * s8rtc. Kept so the stock loader also runs unmodified under QEMU.
 *
 * Registers (32-bit): 0x04 FIFO_CTRL rw (bit31 reset), 0x08 CMD, 0x0c INT_ENABLE rw,
 * 0x10 INT_STATUS w1c (bit0 DONE), 0x34 TX_DATA, 0x38 RX_DATA.
 * CMD write executes one random-access byte transfer: bit19 RANDOM, bit20 WRITE,
 * [18:15] slave, [14:7] register. A read puts the byte in RX_DATA, DONE is set
 * at once.
 * PMIC (slave 1): 256-byte register file, zero at reset (ID 0x00 unknown).
 * RTC  (slave 2): 0x04..0x0a = sec min hour weekday(1<<wday) day month year-2000,
 * binary, host UTC time at read time; other registers a register file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "s8-devices.h"

OBJECT_DECLARE_SIMPLE_TYPE(S8SpeedyState, S8_SPEEDY)

struct S8SpeedyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t fifo_ctrl, int_en, int_status, rx;
    uint8_t pmic[256];
    uint8_t rtc[256];
};

static uint8_t rtc_read(S8SpeedyState *s, unsigned reg)
{
    struct tm tm;
    time_t t = time(NULL);

    if (reg < 0x04 || reg > 0x0a) {
        return s->rtc[reg];
    }
    gmtime_r(&t, &tm);
    switch (reg) {
    case 0x04: return tm.tm_sec;
    case 0x05: return tm.tm_min;
    case 0x06: return tm.tm_hour;
    case 0x07: return 1u << tm.tm_wday;
    case 0x08: return tm.tm_mday;
    case 0x09: return tm.tm_mon + 1;
    default:   return tm.tm_year - 100;
    }
}

static uint64_t speedy_read(void *opaque, hwaddr off, unsigned size)
{
    S8SpeedyState *s = opaque;

    switch (off) {
    case 0x04: return s->fifo_ctrl;
    case 0x0c: return s->int_en;
    case 0x10: return s->int_status;
    case 0x38: return s->rx;
    default:   return 0;
    }
}

static void speedy_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    S8SpeedyState *s = opaque;
    static uint8_t txbyte;

    switch (off) {
    case 0x04:
        s->fifo_ctrl = v & ~(1u << 31);
        break;
    case 0x0c:
        s->int_en = v;
        break;
    case 0x10:
        s->int_status &= ~(uint32_t)v;
        break;
    case 0x34:
        txbyte = v;
        break;
    case 0x08: {
        unsigned slave = (v >> 15) & 0xf, reg = (v >> 7) & 0xff;
        bool write = v & (1u << 20);

        if (slave == 1) {
            if (write) {
                s->pmic[reg] = txbyte;
            } else {
                s->rx = s->pmic[reg];
            }
        } else if (slave == 2) {
            if (write) {
                s->rtc[reg] = txbyte;
            } else {
                s->rx = rtc_read(s, reg);
            }
        } else if (!write) {
            s->rx = 0xff;
        }
        s->int_status |= 1;
        break;
    }
    default:
        break;
    }
}

static const MemoryRegionOps speedy_ops = {
    .read = speedy_read,
    .write = speedy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void speedy_realize(DeviceState *dev, Error **errp)
{
    S8SpeedyState *s = S8_SPEEDY(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &speedy_ops, s, "s8-speedy",
                          S8_SPEEDY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void speedy_reset(DeviceState *dev)
{
    S8SpeedyState *s = S8_SPEEDY(dev);

    s->fifo_ctrl = s->int_en = s->int_status = s->rx = 0;
    memset(s->pmic, 0, sizeof(s->pmic));
    memset(s->rtc, 0, sizeof(s->rtc));
}

static void speedy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = speedy_realize;
    device_class_set_legacy_reset(dc, speedy_reset);
    dc->desc = "Exynos SPEEDY + S2MPS17 PMIC/RTC stub";
}

static const TypeInfo speedy_info = {
    .name = TYPE_S8_SPEEDY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8SpeedyState),
    .class_init = speedy_class_init,
};

static void speedy_register_types(void)
{
    type_register_static(&speedy_info);
}

type_init(speedy_register_types)
