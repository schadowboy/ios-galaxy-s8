/*
 * s8-hsi2c: Exynos HSI2C master (i2c-exynos5.c "auto mode" subset) as used by
 * the kernel's polled s8i2c_xfer() (patch 0124, bsd/dev/a5xelte_touch.c).
 *
 * Owns one QEMU I2CBus ("i2c"); slaves are normal I2CSlave devices.
 *
 * Register map (32-bit, LE). Unlisted offsets < 0x100 are a plain register file.
 *   0x00 CTL        bit31 SW_RST (resets FIFOs/status), bit7 TXCHON, bit6 RXCHON
 *   0x04 FIFO_CTL   rw
 *   0x08 TRAILING   rw
 *   0x20 INT_ENABLE rw
 *   0x24 INT_STATUS w1c. bit7 TRANSFER_DONE, bit9 NO_DEV_ACK, bit10 NO_DEV
 *   0x30 FIFO_STATUS ro: [6:0] TX level, bit7 TX full, bit22 TX empty,
 *                        [14:8] RX level, bit24 RX empty
 *   0x34 TX_DATA    wo byte (write transfers; ignored when no write pending)
 *   0x38 RX_DATA    ro byte (pops RX FIFO)
 *   0x40 CONF       rw (bit31 auto mode)
 *   0x44 AUTO_CONF  bit31 RUN, bit17 STOP_AFTER, bit16 READ, [15:0] length.
 *                   Write: completes when 'length' TX bytes arrived. Read:
 *                   completes immediately, bytes appear in the RX FIFO.
 *   0x48 TIMEOUT, 0x60..0x6c timing: rw
 *   0x70 ADDR       slave address << 10
 * A transfer without STOP followed by one with START is replayed as stop+start
 * (slave models keep their register pointer across it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/qdev-properties.h"
#include "s8-devices.h"

#define R_CTL 0x00
#define R_INT_STATUS 0x24
#define R_FIFO_STATUS 0x30
#define R_TX 0x34
#define R_RX 0x38
#define R_AUTO 0x44
#define R_ADDR 0x70
#define INT_DONE (1u << 7)
#define INT_NOACK (1u << 9)
#define INT_NODEV (1u << 10)
#define FIFO_N 64

OBJECT_DECLARE_SIMPLE_TYPE(S8Hsi2cState, S8_HSI2C)

struct S8Hsi2cState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    I2CBus *bus;
    uint32_t regs[0x100 / 4];
    uint8_t rx[FIFO_N];
    unsigned rx_n, rx_pos;
    uint8_t tx[FIFO_N];
    unsigned tx_n, tx_want;
    bool tx_stop;
};

static void rx_clear(S8Hsi2cState *s) { s->rx_n = s->rx_pos = 0; }

static unsigned slave_addr(S8Hsi2cState *s)
{
    return (s->regs[R_ADDR / 4] >> 10) & 0x7f;
}

static void do_write(S8Hsi2cState *s)
{
    if (i2c_start_send(s->bus, slave_addr(s))) {
        s->regs[R_INT_STATUS / 4] |= INT_NOACK | INT_NODEV;
    } else {
        for (unsigned i = 0; i < s->tx_n; i++) {
            i2c_send(s->bus, s->tx[i]);
        }
        i2c_end_transfer(s->bus);
        s->regs[R_INT_STATUS / 4] |= INT_DONE;
    }
    s->tx_n = s->tx_want = 0;
}

static void do_read(S8Hsi2cState *s, unsigned len)
{
    rx_clear(s);
    if (len > FIFO_N) {
        len = FIFO_N;
    }
    if (i2c_start_recv(s->bus, slave_addr(s))) {
        s->regs[R_INT_STATUS / 4] |= INT_NOACK | INT_NODEV;
        return;
    }
    for (unsigned i = 0; i < len; i++) {
        s->rx[i] = i2c_recv(s->bus);
    }
    s->rx_n = len;
    i2c_end_transfer(s->bus);
    s->regs[R_INT_STATUS / 4] |= INT_DONE;
}

static uint64_t s8_hsi2c_read(void *opaque, hwaddr off, unsigned size)
{
    S8Hsi2cState *s = opaque;

    switch (off) {
    case R_FIFO_STATUS: {
        unsigned rxl = s->rx_n - s->rx_pos;
        return (s->tx_n & 0x7f) | (s->tx_n ? 0 : 1u << 22) |
               (rxl << 8) | (rxl ? 0 : 1u << 24);
    }
    case R_RX:
        return s->rx_pos < s->rx_n ? s->rx[s->rx_pos++] : 0;
    case R_TX:
        return 0;
    default:
        return s->regs[off / 4];
    }
}

static void s8_hsi2c_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    S8Hsi2cState *s = opaque;

    switch (off) {
    case R_CTL:
        if (v & (1u << 31)) {
            rx_clear(s);
            s->tx_n = s->tx_want = 0;
            s->regs[R_INT_STATUS / 4] = 0;
        }
        s->regs[off / 4] = v & ~(1u << 31);
        break;
    case R_INT_STATUS:
        s->regs[off / 4] &= ~(uint32_t)v;
        break;
    case R_TX:
        if (s->tx_want && s->tx_n < FIFO_N) {
            s->tx[s->tx_n++] = v;
            if (s->tx_n >= s->tx_want) {
                do_write(s);
            }
        }
        break;
    case R_AUTO:
        s->regs[off / 4] = v;
        if (v & (1u << 31)) {
            unsigned len = v & 0xffff;
            if (v & (1u << 16)) {
                do_read(s, len);
            } else if (len) {
                s->tx_n = 0;
                s->tx_want = len > FIFO_N ? FIFO_N : len;
            }
        }
        break;
    case R_FIFO_STATUS:
    case R_RX:
        break;
    default:
        s->regs[off / 4] = v;
    }
}

static const MemoryRegionOps s8_hsi2c_ops = {
    .read = s8_hsi2c_read,
    .write = s8_hsi2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s8_hsi2c_realize(DeviceState *dev, Error **errp)
{
    S8Hsi2cState *s = S8_HSI2C(dev);

    s->bus = i2c_init_bus(dev, "i2c");
    memory_region_init_io(&s->mmio, OBJECT(dev), &s8_hsi2c_ops, s,
                          "s8-hsi2c", S8_HSI2C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void s8_hsi2c_reset(DeviceState *dev)
{
    S8Hsi2cState *s = S8_HSI2C(dev);

    memset(s->regs, 0, sizeof(s->regs));
    rx_clear(s);
    s->tx_n = s->tx_want = 0;
}

static void s8_hsi2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s8_hsi2c_realize;
    device_class_set_legacy_reset(dc, s8_hsi2c_reset);
    dc->desc = "Exynos HSI2C master (auto mode)";
}

static const TypeInfo s8_hsi2c_info = {
    .name = TYPE_S8_HSI2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8Hsi2cState),
    .class_init = s8_hsi2c_class_init,
};

static void s8_hsi2c_register_types(void)
{
    type_register_static(&s8_hsi2c_info);
}

type_init(s8_hsi2c_register_types)
