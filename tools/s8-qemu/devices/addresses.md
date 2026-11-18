# Agent E devices: address table and register maps

Single table for the display / input / sensor devices. `devices/s8-devices.h` has the same numbers; the machine only
calls `s8_e_devices_create(sysmem, fb_base, fb_size, crop)` (hook in `qemu/hw/arm/s8-a5xelte.c`). No IRQs are used: the
kernel polls all of these. Sources: kernel patch 0124 (a5xelte_touch.c / a5xelte_keys.c / SweetFramebuffer), loader
`shim/dreamlte/{touch_probe,pmic,display_probe}.c`.

| Device (QOM type)       | Base        | Size    | Kernel uses it for                                   | Fidelity |
|-------------------------|-------------|---------|------------------------------------------------------|----------|
| panel buffer (RAM, not a device) | 0xcc000000 | 1440*2960*4 = 17 049 600 (9 x 2 MiB) | AppleCLCD CPU-copies frames here (XRGB8888, B lowest byte, pitch 5760) | in DRAM (low bank) |
| `s8-fb` DECON_F stub    | 0x12860000  | 0x1000  | frame-done wait before each copy                     | behavioural |
| `s8-hsi2c` (touch bus)  | 0x108e0000  | 0x1000  | S6SY761 @0x48 polled xfers                           | auto-mode subset |
| `s8-hsi2c` (battery bus)| 0x10970000  | 0x1000  | MAX77865 FG @0x36, charger @0x69                     | auto-mode subset |
| `s8-alive-gpio`         | 0x164b0000  | 0x10000 | keys GPA0/GPA2 DAT, touch INT GPA1-0                 | DAT regs live, rest reg file |
| `s8-speedy`             | 0x15b50000  | 0x1000  | loader only (S2MPS17 PMIC + RTC); kernel uses boot-arg s8rtc | stub |

Not mine, listed because they share the window: PMU 0x16480000 (kernel maps it), UFS 0x11120000/0x11121100, UART 0x13800000.

## s8-fb (display)
- Properties: `fb-base` (0xcc000000) `fb-size` `width` (1440) `height` (2960) `pitch` (5760) `crop-x/y/w/h` (0 = whole panel).
  The machine property `crop=on` selects the centred 1125x2436 area (x 157, y 262) the iOS UI uses (patch 0061/0124 centre it).
- Scan-out: QEMU console, XRGB8888 copied row by row from guest RAM with dirty-page tracking, refreshed by the UI timer (~30 Hz).
- DECON registers (32-bit): all plain read/write except
  - `0x40` INTERRUPT_ENABLE rw (kernel sets bit13|bit0);
  - `0x4c` INTERRUPT_PENDING: bit13 DPU_FRAME_DONE always reads 1 (no 20 ms wait per frame, platform.md 3.5), write 1 clears the rest;
  - `0x60` SHADOW_REG_UPDATE_REQ reads 0 (update done), writes ignored.
  IDMA/DPP/SYSMMU blocks (0x128b1000.., 0x12900000) are NOT modelled; they belong to the loader's `display_probe`,
  which the QEMU boot stage does not run. The generic RAZ/WI window covers them.
- No flip register exists: the kernel writes pixels into the buffer the loader left scanned by DECON (boot_args.Video is empty; the
  address is the compile-time constant A5_FB_PHYS in patch 0124, also in the ADT node `a5xelte-fbcon` per Agent A/D).

## s8-hsi2c (Exynos HSI2C master, auto mode)
`0x00` CTL (bit31 SW_RST) | `0x04` FIFO_CTL | `0x08` TRAILING | `0x20` INT_ENABLE | `0x24` INT_STATUS (w1c; bit7 DONE, bit9 NO_DEV_ACK,
bit10 NO_DEV) | `0x30` FIFO_STATUS (ro; TX level [6:0], TX empty b22, RX level [14:8], RX empty b24) | `0x34` TX_DATA | `0x38` RX_DATA |
`0x40` CONF | `0x44` AUTO_CONF (bit31 RUN, b17 STOP, b16 READ, [15:0] length) | `0x48` TIMEOUT | `0x60-0x6c` timing | `0x70` ADDR (7-bit << 10).
A write transfer executes once `length` TX bytes arrived; a read transfer executes at once and fills the RX FIFO (max 64 bytes).
Absent slave: INT_STATUS gets bits 9 and 10 (the kernel treats 0xf00 as error).

## s8-s6sy761 (touch, I2C 0x48)
Commands: `0x55` BOOT_STATUS -> 0x20; `0x52` DEVICE_ID -> ac 37 61; `0x30` TOUCH_FUNCTION, `0x10` SENSE_ON accepted; `0x60` READ_ONE_EVENT (8 bytes);
`0x61` READ_ALL_EVENT (remaining events). Event: b0 = (id+1)<<2 | state<<6 (1 press, 2 move, 3 release), b1 = x>>4, b2 = y>>4,
b3 = (x&15)<<4|(y&15), b4..b6 area/pressure, b7 = events left. x/y raw 0..4095 over the whole 1440x2960 panel (the kernel maps them to the
centred 1125x2436 UI, so the crop window handles the offset). INT (GPA1-0, bit0 of ALIVE+0x44) is low while events are queued.
Input: QEMU pointer in absolute mode, left button = finger 0 (press/drag/release). One finger only.

## s8-max77865-fg (I2C 0x36) and -chg (0x69)
FG 16-bit LE registers (low byte first, auto-increment): `0x06` SOCREP = soc*256, `0x09` VCELL = mV*12.8 (78.125 uV/LSB), `0x0a` CURRENT = mA/0.15625
(signed). Defaults: 97 %, 4250 mV, +500 mA. Runtime QOM props on the FG: `soc`, `mv`, `ma`, `external` (qom-set works). Everything else reads 0.
Charger 8-bit: `0xb2` INT_OK = 0x40 when `external`, `0xb4` DETAILS_01 = 1 (charging) / 3 (soc >= 100), 0 without external power.

## s8-alive-gpio (pinctrl ALIVE)
Plain register file except the DAT registers (input pins, idle 0xff, active low): `0x24` GPA0 (bit3 Vol+, bit4 Vol-, bit6 Home/Bixby),
`0x44` GPA1 (bit0 touch INT), `0x64` GPA2 (bit4 Power). Host keys in the QEMU window: P = power, H = home, PageUp/PageDown = volume.

## s8-speedy (SPEEDY master + S2MPS17 + RTC)
`0x04` FIFO_CTRL | `0x08` CMD (bit19 RANDOM, bit20 WRITE, [18:15] slave, [14:7] reg; executes immediately) | `0x0c` INT_ENABLE | `0x10` INT_STATUS
(w1c, bit0 DONE) | `0x34` TX_DATA | `0x38` RX_DATA. Slave 1 = PMIC register file (zeros), slave 2 = RTC: regs 0x04..0x0a = sec, min, hour,
weekday bit, day, month, year-2000 (binary) from host UTC.

## Notes (aligned with docs/platform.md)
- Boot args under QEMU use `sweetfb_native=1`: iOS then draws the whole 1440x2960 panel, so run WITHOUT `--crop` (crop only matters for the non-native 1125x2436 layout).
  Touch raw 0..4095 spans the whole panel in both modes; the kernel does the mapping.
- `s8touch=0` (platform.md bring-up advice) makes the kernel skip the touch controller entirely; the model works with `s8touch=1` (default) too.
- Not modelled here (Agent B): PMU 0x16480000 SWRESET/PS_HOLD. Not modelled by anyone: IDMA/DPP/SYSMMU, TMU, ACPM (RAZ/WI window).
