/*
 * s8-fb: scan-out of the Galaxy S8 (Exynos 8895) panel buffer + DECON stub.
 *
 * The kernel (AppleCLCD, patch 0124/0046/0056) CPU-copies the composed frame
 * (1440x2960 XRGB8888, pitch 5760, B in the lowest byte) into DRAM at
 * 0xcc000000, which on the phone the DECON_F display controller scans out at
 * 60 Hz. There is no flip register: the kernel only waits for the DECON
 * frame-done interrupt flag before copying (to avoid tearing):
 *   INTERRUPT_ENABLE  0x40  |= (1<<13)|1
 *   INTERRUPT_PENDING 0x4c   write (1<<13) to clear, poll bit 13 (<= 20 ms)
 * This device models that (bit 13 set by a 60 Hz virtual-clock timer), keeps
 * the other DECON registers as a plain register file (SHADOW_REG_UPDATE_REQ
 * 0x60 reads 0 = "update done"), and shows the buffer in a QEMU console.
 * Bit 13 of 0x4c always reads 1 (a frame is always "just done"), so the
 * kernel's 20 ms wait per frame never happens (docs/platform.md 3.5).
 *
 * Properties: fb-base fb-size width height pitch  (guest buffer),
 *             crop-x crop-y crop-w crop-h        (visible window, 0 = all).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "ui/console.h"
#include "system/address-spaces.h"
#include "hw/display/framebuffer.h"
#include "migration/vmstate.h"
#include "s8-devices.h"

#define DECON_INT_EN      0x40
#define DECON_INT_PEND    0x4c
#define DECON_FRAME_DONE  (1u << 13)

OBJECT_DECLARE_SIMPLE_TYPE(S8FbState, S8_FB)

struct S8FbState {
    SysBusDevice parent_obj;

    MemoryRegion decon_mmio;
    QemuConsole *con;
    MemoryRegionSection fbsection;
    uint32_t regs[S8_DECON_SIZE / 4];
    int invalidate;

    uint64_t fb_base, fb_size;
    uint32_t width, height, pitch;
    uint32_t crop_x, crop_y, crop_w, crop_h;
};

static uint64_t s8_decon_read(void *opaque, hwaddr off, unsigned size)
{
    S8FbState *s = opaque;

    switch (off) {
    case DECON_INT_PEND: /* frame-done always pending: the kernel never waits */
        return s->regs[off / 4] | DECON_FRAME_DONE;
    case 0x60: /* SHADOW_REG_UPDATE_REQ: self-clearing, always done */
        return 0;
    default:
        return s->regs[off / 4];
    }
}

static void s8_decon_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    S8FbState *s = opaque;

    switch (off) {
    case DECON_INT_PEND: /* write 1 to clear */
        s->regs[off / 4] &= ~(uint32_t)v;
        break;
    case 0x60:
        break;
    default:
        s->regs[off / 4] = v;
    }
}

static const MemoryRegionOps s8_decon_ops = {
    .read = s8_decon_read,
    .write = s8_decon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s8_fb_row(void *opaque, uint8_t *dest, const uint8_t *src,
                      int width, int deststep)
{
    memcpy(dest, src, (size_t)width * 4);
}

static void s8_fb_invalidate(void *opaque)
{
    S8FbState *s = opaque;

    s->invalidate = 1;
}

static void s8_fb_update(void *opaque)
{
    S8FbState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    int first = 0, last = 0;
    hwaddr base = s->fb_base + (hwaddr)s->crop_y * s->pitch + s->crop_x * 4;

    if (!surf || surface_bits_per_pixel(surf) != 32) {
        return;
    }
    framebuffer_update_memory_section(&s->fbsection, get_system_memory(),
                                      base, s->crop_h, s->pitch);
    if (!s->fbsection.mr) {
        return;
    }
    framebuffer_update_display(surf, &s->fbsection, s->crop_w, s->crop_h,
                               s->pitch, surface_stride(surf), 4,
                               s->invalidate, s8_fb_row, s, &first, &last);
    s->invalidate = 0;
    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, s->crop_w, last - first + 1);
    }
}

static const GraphicHwOps s8_fb_ops = {
    .invalidate = s8_fb_invalidate,
    .gfx_update = s8_fb_update,
};

static void s8_fb_realize(DeviceState *dev, Error **errp)
{
    S8FbState *s = S8_FB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->crop_w) {
        s->crop_x = 0;
        s->crop_w = s->width;
    }
    if (!s->crop_h) {
        s->crop_y = 0;
        s->crop_h = s->height;
    }
    if (s->crop_x + s->crop_w > s->width ||
        s->crop_y + s->crop_h > s->height || s->pitch < s->width * 4 ||
        (uint64_t)s->pitch * s->height > s->fb_size) {
        error_setg(errp, "s8-fb: inconsistent geometry");
        return;
    }
    memory_region_init_io(&s->decon_mmio, OBJECT(dev), &s8_decon_ops, s,
                          "s8-decon", S8_DECON_SIZE);
    sysbus_init_mmio(sbd, &s->decon_mmio);
    s->con = graphic_console_init(dev, 0, &s8_fb_ops, s);
    qemu_console_resize(s->con, s->crop_w, s->crop_h);
    s->invalidate = 1;
}

static const Property s8_fb_props[] = {
    DEFINE_PROP_UINT64("fb-base", S8FbState, fb_base, S8_FB_BASE),
    DEFINE_PROP_UINT64("fb-size", S8FbState, fb_size,
                       (uint64_t)S8_FB_PITCH * S8_FB_HEIGHT),
    DEFINE_PROP_UINT32("width", S8FbState, width, S8_FB_WIDTH),
    DEFINE_PROP_UINT32("height", S8FbState, height, S8_FB_HEIGHT),
    DEFINE_PROP_UINT32("pitch", S8FbState, pitch, S8_FB_PITCH),
    DEFINE_PROP_UINT32("crop-x", S8FbState, crop_x, 0),
    DEFINE_PROP_UINT32("crop-y", S8FbState, crop_y, 0),
    DEFINE_PROP_UINT32("crop-w", S8FbState, crop_w, 0),
    DEFINE_PROP_UINT32("crop-h", S8FbState, crop_h, 0),
};

static void s8_fb_reset(DeviceState *dev)
{
    S8FbState *s = S8_FB(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->invalidate = 1;
}

static void s8_fb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s8_fb_realize;
    device_class_set_legacy_reset(dc, s8_fb_reset);
    device_class_set_props(dc, s8_fb_props);
    dc->desc = "Galaxy S8 panel scan-out + DECON frame-done stub";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s8_fb_info = {
    .name = TYPE_S8_FB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S8FbState),
    .class_init = s8_fb_class_init,
};

static void s8_fb_register_types(void)
{
    type_register_static(&s8_fb_info);
}

type_init(s8_fb_register_types)
