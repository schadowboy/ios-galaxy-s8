/*
 * s8-alive-gpio: Exynos 8895 pinctrl ALIVE block (0x164b0000) as polled by the
 * kernel for the hardware keys (bsd/dev/a5xelte_keys.c) and the touch INT line.
 *
 * Bank layout (per bank 0x20): +0 CON, +4 DAT, +8 PUD, +0xc DRV, ...
 *   GPA0 0x20: DAT 0x24, GPA1 0x40: DAT 0x44, GPA2 0x60: DAT 0x64
 * Everything else is a plain register file (reads back what was written).
 * DAT bits (active low, idle = 1):
 *   GPA0 0x24: bit3 Volume Up, bit4 Volume Down, bit6 Home (Bixby)
 *   GPA1 0x44: bit0 touch INT (driven by the qdev input GPIO "gpa1-0")
 *   GPA2 0x64: bit4 Power
 * Host keys (QEMU window focused): P = Power, H = Home, PageUp = Vol+,
 * PageDown = Vol-.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "ui/input.h"
#include "s8-devices.h"

#define GPA0_DAT 0x24
#define GPA1_DAT 0x44
#define GPA2_DAT 0x64

OBJECT_DECLARE_SIMPLE_TYPE(S8AliveState, S8_ALIVE)

struct S8AliveState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[S8_ALIVE_SIZE / 4];
    uint32_t gpa0, gpa1, gpa2;      /* current DAT low bytes */
    QemuInputHandlerState *hs;
};

static uint64_t alive_read(void *opaque, hwaddr off, unsigned size)
{
    S8AliveState *s = opaque;

    switch (off) {
    case GPA0_DAT:
        return s->gpa0;
    case GPA1_DAT:
        return s->gpa1;
    case GPA2_DAT:
        return s->gpa2;
    default:
        return s->regs[off / 4];
    }
}

static void alive_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    S8AliveState *s = opaque;

    if (off == GPA0_DAT || off == GPA1_DAT || off == GPA2_DAT) {
        return;         /* input pins */
    }
    s->regs[off / 4] = v;
}

static const MemoryRegionOps alive_ops = {
    .read = alive_read,
    .write = alive_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void alive_gpa1_0(void *opaque, int n, int level)
{
    S8AliveState *s = opaque;

    s->gpa1 = level ? (s->gpa1 | 1) : (s->gpa1 & ~1u);
}

static void alive_key(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    S8AliveState *s = S8_ALIVE(dev);
    InputKeyEvent *key;
    uint32_t *reg;
    unsigned bit;

    if (evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    key = evt->u.key.data;
    switch (qemu_input_key_value_to_qcode(key->key)) {
    case Q_KEY_CODE_P:
        reg = &s->gpa2; bit = 4; break;
    case Q_KEY_CODE_H:
        reg = &s->gpa0; bit = 6; break;
    case Q_KEY_CODE_PGUP:
        reg = &s->gpa0; bit = 3; break;
    case Q_KEY_CODE_PGDN:
        reg = &s->gpa0; bit = 4; break;
    default:
        return;
    }
    if (key->down) {
        *reg &= ~(1u << bit);
    } else {
        *reg |= 1u << bit;
    }
}

static const QemuInputHandler alive_input = {
    .name = "S8 keys",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = alive_key,
};

static void alive_realize(DeviceState *dev, Error **errp)
{
    S8AliveState *s = S8_ALIVE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &alive_ops, s, "s8-alive",
                          S8_ALIVE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    qdev_init_gpio_in_named(dev, alive_gpa1_0, "gpa1-0", 1);
    s->hs = qemu_input_handler_register(dev, &alive_input);
}

static void alive_reset(DeviceState *dev)
{
    S8AliveState *s = S8_ALIVE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->gpa0 = s->gpa1 = s->gpa2 = 0xff;
}

static void alive_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = alive_realize;
    device_class_set_legacy_reset(dc, alive_reset);
    dc->desc = "Exynos 8895 ALIVE pinctrl: keys + touch INT";
}

static const TypeInfo alive_info = {
    .name = TYPE_S8_ALIVE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8AliveState),
    .class_init = alive_class_init,
};

static void alive_register_types(void)
{
    type_register_static(&alive_info);
}

type_init(alive_register_types)
