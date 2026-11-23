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
 * RTC  (slave 2): see rtc_latch/rtc_commit below (proposed model with MSEC at 0x04, SEC..YEAR at 0x05..0x0b,
 * RUDR/WUDR latch/commit, settable), other registers a register file. Proposed replacement for s8-speedy.c (0151).
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
    uint8_t rtc_lat[8];
    time_t rtc_off;
};

/*
 * RTC model of the real S2MPS17 (Samsung rtc-s2mps17.c): 0x00 CTRL (reset 0x02 = binary, 24 h), 0x02 UPDATE,
 * 0x04 MSEC, 0x05..0x0b SEC MIN HOUR(PM = bit 6) WEEKDAY(1 << wday) DATE MONTH YEAR-2000.
 * Time registers are read from a latched image: UPDATE.RUDR (bit 0) latches host time + offset, then clears itself.
 * UPDATE.WUDR (bit 1) commits the written registers 0x05..0x0b as the new time (offset to the host clock), then clears itself.
 */
static void rtc_latch(S8SpeedyState *s)
{
    struct tm tm;
    time_t t = time(NULL) + s->rtc_off;

    gmtime_r(&t, &tm);
    s->rtc_lat[0] = 0;
    s->rtc_lat[1] = tm.tm_sec;
    s->rtc_lat[2] = tm.tm_min;
    s->rtc_lat[3] = tm.tm_hour | (tm.tm_hour >= 12 ? 0x40 : 0);
    s->rtc_lat[4] = 1u << tm.tm_wday;
    s->rtc_lat[5] = tm.tm_mday;
    s->rtc_lat[6] = tm.tm_mon + 1;
    s->rtc_lat[7] = tm.tm_year - 100;
}

static void rtc_commit(S8SpeedyState *s)
{
    struct tm tm = { 0 };

    tm.tm_sec = s->rtc[0x05] & 0x7f;
    tm.tm_min = s->rtc[0x06] & 0x7f;
    tm.tm_hour = s->rtc[0x07] & 0x1f;
    tm.tm_mday = s->rtc[0x09] & 0x1f;
    tm.tm_mon = (s->rtc[0x0a] & 0x0f) - 1;
    tm.tm_year = (s->rtc[0x0b] & 0x7f) + 100;
    s->rtc_off = timegm(&tm) - time(NULL);
}

static uint8_t rtc_read(S8SpeedyState *s, unsigned reg)
{
    if (reg >= 0x04 && reg <= 0x0b) {
        return s->rtc_lat[reg - 0x04];
    }
    return s->rtc[reg];
}

static void rtc_write(S8SpeedyState *s, unsigned reg, uint8_t v)
{
    if (reg == 0x02) {
        s->rtc[reg] = v & ~0x03;        /* RUDR and WUDR clear themselves */
        if (v & 0x02) {
            rtc_commit(s);
        }
        if (v & 0x01) {
            rtc_latch(s);
        }
        return;
    }
    s->rtc[reg] = v;
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
                rtc_write(s, reg, txbyte);
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
    s->rtc[0x00] = 0x02;
    s->rtc_off = 0;
    rtc_latch(s);
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
