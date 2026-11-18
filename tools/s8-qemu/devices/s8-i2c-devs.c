/*
 * I2C slaves of the Galaxy S8 that the kernel polls:
 *   s8-s6sy761      Samsung S6SY761 touch controller (0x48 on hsi2c_23)
 *   s8-max77865-fg  MAX77865 fuel gauge (0x36 on hsi2c_32)
 *   s8-max77865-chg MAX77865 charger   (0x69 on hsi2c_32)
 *
 * Touch: events come from the QEMU absolute pointer (left button = finger 0,
 * drag = move). The INT line (GPA1-0, active low while events are queued) is
 * the qdev GPIO output "int"; s8-alive-gpio consumes it.
 * Protocol (see kernel s8ts_*, mainline s6sy761.c):
 *   0x55 BOOT_STATUS -> 0x20 (application)      0x52 DEVICE_ID -> ac 37 61
 *   0x30 TOUCH_FUNCTION (+2 bytes), 0x10 SENSE_ON: accepted, no readback
 *   0x60 READ_ONE_EVENT -> 8 bytes, 0x61 READ_ALL_EVENT -> queued events 8 B each
 *   event: b0 = (touch_id+1)<<2 | state<<6 (state 1 press, 2 move, 3 release),
 *          b1 = x>>4, b2 = y>>4, b3 = (x&15)<<4 | (y&15), b4..6 = area/pressure,
 *          b7 = events still queued (6 bits). x,y raw 0..4095 across the panel.
 * Fuel gauge (16-bit LE registers, byte read = lo then hi):
 *   0x06 SOCREP (1/256 %), 0x09 VCELL (78.125 uV), 0x0a CURRENT (156.25 uA,
 *   + = charging). Defaults 97 %, 4.25 V, +500 mA. QOM props (runtime
 *   settable, qom-set): soc (percent), mv, ma, external (0/1).
 * Charger (8-bit): 0xb2 INT_OK bit6 CHGIN_OK = external power, 0xb4 DETAILS_01
 *   low nibble 0-2 charging / 3-4 done; other registers read 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "ui/input.h"
#include "s8-devices.h"

/* ---------------------------------------------------------------- touch */

#define TS_QLEN 64

OBJECT_DECLARE_SIMPLE_TYPE(S8TouchState, S8_S6SY761)

struct S8TouchState {
    I2CSlave parent_obj;
    qemu_irq int_out;
    QemuInputHandlerState *hs;
    uint8_t cmd;
    bool have_cmd;
    uint8_t q[TS_QLEN][8];
    unsigned q_head, q_n;
    uint8_t cur[8];
    unsigned cur_pos;
    /* pointer state */
    int abs_x, abs_y;      /* 0..0x7fff over the visible window */
    bool down, was_down;
    int last_x, last_y;    /* raw 0..4095 of last sent position */
    /* geometry: panel and visible window, in panel pixels */
    uint32_t panel_w, panel_h, crop_x, crop_y, crop_w, crop_h;
};

static void ts_update_int(S8TouchState *s)
{
    qemu_set_irq(s->int_out, s->q_n ? 0 : 1);
}

static void ts_push(S8TouchState *s, unsigned state, int x, int y)
{
    uint8_t *e;

    if (s->q_n == TS_QLEN) {
        return;
    }
    e = s->q[(s->q_head + s->q_n++) % TS_QLEN];
    e[0] = (0 + 1) << 2 | state << 6;
    e[1] = x >> 4;
    e[2] = y >> 4;
    e[3] = (x & 15) << 4 | (y & 15);
    e[4] = 8;
    e[5] = 8;
    e[6] = state == 3 ? 0 : 40;
    e[7] = 0;
    ts_update_int(s);
}

static bool ts_pop(S8TouchState *s)
{
    if (!s->q_n) {
        memset(s->cur, 0, 8);
        return false;
    }
    memcpy(s->cur, s->q[s->q_head], 8);
    s->q_head = (s->q_head + 1) % TS_QLEN;
    s->q_n--;
    s->cur[7] = s->q_n > 0x3f ? 0x3f : s->q_n;
    ts_update_int(s);
    return true;
}

static int ts_to_raw(S8TouchState *s, int abs, bool xaxis)
{
    uint32_t off = xaxis ? s->crop_x : s->crop_y;
    uint32_t len = xaxis ? s->crop_w : s->crop_h;
    uint32_t tot = xaxis ? s->panel_w : s->panel_h;
    uint64_t px = off + (uint64_t)abs * len / 0x8000;
    int raw = px * 4096 / tot;

    return raw < 0 ? 0 : (raw > 4095 ? 4095 : raw);
}

static void ts_input_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    S8TouchState *s = S8_S6SY761(dev);
    InputMoveEvent *move;
    InputBtnEvent *btn;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS:
        move = evt->u.abs.data;
        if (move->axis == INPUT_AXIS_X) {
            s->abs_x = move->value;
        } else if (move->axis == INPUT_AXIS_Y) {
            s->abs_y = move->value;
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        btn = evt->u.btn.data;
        if (btn->button == INPUT_BUTTON_LEFT) {
            s->down = btn->down;
        }
        break;
    default:
        break;
    }
}

static void ts_input_sync(DeviceState *dev)
{
    S8TouchState *s = S8_S6SY761(dev);
    int x = ts_to_raw(s, s->abs_x, true), y = ts_to_raw(s, s->abs_y, false);

    if (s->down && !s->was_down) {
        ts_push(s, 1, x, y);
    } else if (s->down && (x != s->last_x || y != s->last_y)) {
        ts_push(s, 2, x, y);
    } else if (!s->down && s->was_down) {
        ts_push(s, 3, s->last_x, s->last_y);
    }
    if (s->down) {
        s->last_x = x;
        s->last_y = y;
    }
    s->was_down = s->down;
}

static const QemuInputHandler ts_input = {
    .name = "S8 touchscreen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = ts_input_event,
    .sync = ts_input_sync,
};

static int ts_event(I2CSlave *i2c, enum i2c_event event)
{
    S8TouchState *s = S8_S6SY761(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->have_cmd = false;
        break;
    case I2C_START_RECV:
        s->cur_pos = 0;
        memset(s->cur, 0, 8);
        if (s->cmd == 0x60 || s->cmd == 0x61) {
            ts_pop(s);       /* first event; 0x61 pops more as it is read */
        }
        break;
    default:
        break;
    }
    return 0;
}

static int ts_send(I2CSlave *i2c, uint8_t data)
{
    S8TouchState *s = S8_S6SY761(i2c);

    if (!s->have_cmd) {
        s->cmd = data;
        s->have_cmd = true;
    }
    return 0;
}

static uint8_t ts_recv(I2CSlave *i2c)
{
    S8TouchState *s = S8_S6SY761(i2c);

    switch (s->cmd) {
    case 0x55:
        return 0x20;
    case 0x52: {
        static const uint8_t id[3] = { 0xac, 0x37, 0x61 };
        uint8_t v = s->cur_pos < 3 ? id[s->cur_pos] : 0;
        s->cur_pos++;
        return v;
    }
    case 0x60:
    case 0x61:
        if (s->cur_pos >= 8) {
            if (s->cmd == 0x60 || !ts_pop(s)) {
                return 0;
            }
            s->cur_pos = 0;
        }
        return s->cur[s->cur_pos++];
    default:
        return 0;
    }
}

static void ts_realize(DeviceState *dev, Error **errp)
{
    S8TouchState *s = S8_S6SY761(dev);

    qdev_init_gpio_out_named(dev, &s->int_out, "int", 1);
    if (!s->crop_w) {
        s->crop_x = 0;
        s->crop_w = s->panel_w;
    }
    if (!s->crop_h) {
        s->crop_y = 0;
        s->crop_h = s->panel_h;
    }
    s->hs = qemu_input_handler_register(dev, &ts_input);
}

static void ts_reset(DeviceState *dev)
{
    S8TouchState *s = S8_S6SY761(dev);

    s->q_head = s->q_n = 0;
    s->down = s->was_down = false;
    s->have_cmd = false;
    s->cmd = 0;
    if (s->int_out) {
        qemu_set_irq(s->int_out, 1);
    }
}

static const Property ts_props[] = {
    DEFINE_PROP_UINT32("panel-w", S8TouchState, panel_w, S8_FB_WIDTH),
    DEFINE_PROP_UINT32("panel-h", S8TouchState, panel_h, S8_FB_HEIGHT),
    DEFINE_PROP_UINT32("crop-x", S8TouchState, crop_x, 0),
    DEFINE_PROP_UINT32("crop-y", S8TouchState, crop_y, 0),
    DEFINE_PROP_UINT32("crop-w", S8TouchState, crop_w, 0),
    DEFINE_PROP_UINT32("crop-h", S8TouchState, crop_h, 0),
};

static void ts_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->realize = ts_realize;
    device_class_set_legacy_reset(dc, ts_reset);
    device_class_set_props(dc, ts_props);
    k->event = ts_event;
    k->send = ts_send;
    k->recv = ts_recv;
}

static const TypeInfo ts_info = {
    .name = TYPE_S8_S6SY761,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S8TouchState),
    .class_init = ts_class_init,
};

/* ------------------------------------------------------ MAX77865 gauge */

OBJECT_DECLARE_SIMPLE_TYPE(S8FgState, S8_MAX77865_FG)

struct S8FgState {
    I2CSlave parent_obj;
    uint32_t soc, mv, ma, external;
    uint8_t ptr;
    unsigned byte;       /* 0 = low byte next, 1 = high byte next */
    bool have_ptr;
};

static uint16_t fg_reg(S8FgState *s, uint8_t r)
{
    switch (r) {
    case 0x06: /* SOCREP */
        return s->soc > 100 ? 100 * 256 : s->soc * 256;
    case 0x09: /* VCELL */
        return s->mv * 1000 * 10 / 781 ;
    case 0x0a: /* CURRENT, signed */
        return (uint16_t)(int16_t)((int)s->ma * 100000 / 15625);
    default:
        return 0;
    }
}

static int fg_event(I2CSlave *i2c, enum i2c_event e)
{
    S8FgState *s = S8_MAX77865_FG(i2c);

    if (e == I2C_START_SEND) {
        s->have_ptr = false;
        s->byte = 0;
    } else if (e == I2C_START_RECV) {
        s->byte = 0;
    }
    return 0;
}

static int fg_send(I2CSlave *i2c, uint8_t d)
{
    S8FgState *s = S8_MAX77865_FG(i2c);

    if (!s->have_ptr) {
        s->ptr = d;
        s->have_ptr = true;
    }
    return 0;
}

static uint8_t fg_recv(I2CSlave *i2c)
{
    S8FgState *s = S8_MAX77865_FG(i2c);
    uint16_t v = fg_reg(s, s->ptr);
    uint8_t r = s->byte ? v >> 8 : v & 0xff;

    if (s->byte) {
        s->ptr++;
    }
    s->byte ^= 1;
    return r;
}

static void fg_init(Object *obj)
{
    S8FgState *s = S8_MAX77865_FG(obj);

    s->soc = 97;
    s->mv = 4250;
    s->ma = 500;
    s->external = 1;
    object_property_add_uint32_ptr(obj, "soc", &s->soc, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "mv", &s->mv, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "ma", &s->ma, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "external", &s->external,
                                   OBJ_PROP_FLAG_READWRITE);
}

static void fg_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = fg_event;
    k->send = fg_send;
    k->recv = fg_recv;
}

static const TypeInfo fg_info = {
    .name = TYPE_S8_MAX77865_FG,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S8FgState),
    .instance_init = fg_init,
    .class_init = fg_class_init,
};

/* ------------------------------------------------------- MAX77865 charger */

OBJECT_DECLARE_SIMPLE_TYPE(S8ChgState, S8_MAX77865_CHG)

struct S8ChgState {
    I2CSlave parent_obj;
    S8FgState *fg;       /* optional: follows its 'external' property */
    uint8_t ptr;
    bool have_ptr;
};

static int chg_event(I2CSlave *i2c, enum i2c_event e)
{
    S8ChgState *s = S8_MAX77865_CHG(i2c);

    if (e == I2C_START_SEND) {
        s->have_ptr = false;
    }
    return 0;
}

static int chg_send(I2CSlave *i2c, uint8_t d)
{
    S8ChgState *s = S8_MAX77865_CHG(i2c);

    if (!s->have_ptr) {
        s->ptr = d;
        s->have_ptr = true;
    }
    return 0;
}

static uint8_t chg_recv(I2CSlave *i2c)
{
    S8ChgState *s = S8_MAX77865_CHG(i2c);
    bool ext = s->fg ? s->fg->external : true;
    uint8_t v = 0;

    switch (s->ptr) {
    case 0xb2:
        v = ext ? 0x40 : 0;
        break;
    case 0xb4:
        v = ext ? (s->fg && s->fg->soc >= 100 ? 3 : 1) : 0;
        break;
    }
    s->ptr++;
    return v;
}

static void chg_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = chg_event;
    k->send = chg_send;
    k->recv = chg_recv;
}

static const TypeInfo chg_info = {
    .name = TYPE_S8_MAX77865_CHG,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(S8ChgState),
    .class_init = chg_class_init,
};

/* Link the charger to the gauge (called by s8_e_devices_create). */
void s8_chg_link_fg(DeviceState *chg, DeviceState *fg);
void s8_chg_link_fg(DeviceState *chg, DeviceState *fg)
{
    S8_MAX77865_CHG(chg)->fg = S8_MAX77865_FG(fg);
}

static void s8_i2c_devs_register_types(void)
{
    type_register_static(&ts_info);
    type_register_static(&fg_info);
    type_register_static(&chg_info);
}

type_init(s8_i2c_devs_register_types)
