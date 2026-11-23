# Item #13 Haptics: buzz the S8 motor from the loader

Status: code written and compile-checked (loader flags, no ABS64 relocations), NOT yet run on the phone.
Boot-arg: `hapticprobe=1` (150 ms, full), `=2` (300 ms, full), `=3` (150 ms, half strength).

## 1. Hardware path (Samsung kernel `ref/s8kernel-tgp-p`)

dreamlte has two motor variants. Revision 08/09/10 boards (`exynos8895-dreamlte_eur_open_08/09/10.dts`, include
`exynos8895-dreamlte_motor.dtsi`) use the MAX77865 haptic block. Revisions 05/07 (engineering boards) include
`exynos8895-dreamlte_motor_drv2624.dtsi` and use a TI DRV2624 on hsi2c_15 (0x10860000, slave 0x5A, gpe7-4 reset,
gpf1-6 mode). A retail G950F is the MAX77865 variant (ASSUMED from the revision numbering; step 1 of the log
identifies the chip and the DRV2624 path is the fallback, section 7).

```
Exynos8895 PWM timer 1 (0x104c0000) --TOUT1 gpd0-5 fn2--> MAX77865 MPWM --/128--> LRA H-bridge
Exynos8895 hsi2c_32 (0x10970000) --I2C 0x66--> MAX77865 MCONFIG/MAINCTRL1/BOOSTCONTROL
supply of the motor driver = MAX77865 boost converter (BOOSTCONTROL1/2)
```

Sources:
- dts: `exynos8895-dreamlte_motor.dtsi` (`haptic,pwm_id = <1>`, period 48828 ns for "alert", duty 31738 ns, so
  20.48 kHz and 65 %; `haptic,regulator_name = "VDD_MOTOR_3P3"`);
  `exynos8895-dreamlte_common.dtsi:934` (`hsi2c@10970000 { max77865@66 { reg = <0x66>; pinctrl-0 = <&max77865_irq
  &muic_irq &pwm_tout1>; ...`); `exynos8895-pinctrl.dtsi:739` (`pwm_tout1`: gpd0-5, function 2, pud 1, drv 0);
  `exynos8895.dtsi:3188` (`sec_pwm: pwm@104c0000`, 5 outputs, `samsung,s3c6400-pwm`).
- driver: `drivers/motor/max77865_haptic.c` (`max77865_haptic_init_reg`, `max77865_haptic_i2c`,
  `max77865_motor_boost_control`, `haptic_enable`, `haptic_work`, `haptic_homekey_press`);
  `include/linux/mfd/max77865-private.h` (register enum, "Slave addr = 0xCC" = 7-bit 0x66);
  `drivers/pwm/pwm-samsung.c` (`pwm_samsung_config/enable/disable/init`, `REG_TCNTB(chan) = 0x0c + chan*0xc`,
  TCON channel 1 sits in the bits of "channel index 2": START 8, MANUAL 9, INVERT 10, AUTORELOAD 11);
  `drivers/clk/samsung/clk-exynos-pwm.c` and `clk-exynos8895.c:700` ("pwm-clock" = fixed 1:1 of fin_pll, 26 MHz;
  prescaler and divider are left at 0 = /1);
  `drivers/pinctrl/samsung/pinctrl-exynos.c:1822` (PERIC0 pin controller 0x104d0000, bank gpd0 at +0x000);
  `drivers/soc/samsung/cal-if/exynos8895/cmucal-sfr.c:1780` (`QCH_CON_PWM_QCH` = PERIC0 0x10400000 + 0x3010).
- The "VDD_MOTOR_3P3" regulator is NOT used: `max77865_haptic_probe` has the `regulator_get` commented out and
  `motor_vdd_en()` is dead code. So no S2MPS17 (SPEEDY) register is touched at all.

Why the numbers: the driver programs the PWM period in ns. 20.48 kHz / 128 (MCONFIG PDIV = `DIVIDER_128`) = 160 Hz,
the LRA drive frequency (`haptic,period = 78125000 / freq(0.1 Hz)`: 1600 -> 48828 ns). Duty 50 % = zero drive,
duty 65 % = Samsung's "alert" strength (the driver's own maximum for the alert frequency); `strength` 0..10000 maps
linearly from 50 % to 65 %. Polarity is irrelevant for an LRA bridge (35 % gives the same amplitude, opposite phase).

## 2. Registers touched (everything else is left alone)

MAX77865, I2C 7-bit 0x66 on hsi2c_32:

| reg | name | what and why |
|---|---|---|
| 0x00, 0x01 | PMICID1, PMICREV | read only: proves the chip answers (the driver does the same read with PMICREV) |
| 0x02 | MAINCTRL1 | bit 7 BIASEN = 1 (haptic bias, as `max77865_haptic_init_reg`); original restored at the end |
| 0x10 | MCONFIG | bit 6 MEN = 0 first, bit 7 LRA = 1, bits 1:0 PDIV = 10 (/128; the driver sets bit 1 only, bit 0 is 0 at reset), then MEN = 1 for the pulse, MEN = 0 at the end; original (with MEN = 0) restored |
| 0x4c | BOOSTCONTROL1 | bits 1:0 BSTOUT = 0 (as `BOOST_ON`); original restored |
| 0x4f | BOOSTCONTROL2 | bit 3 FORCE_EN = 1 (as `BOOST_ON`); original restored |

Exynos8895:

| address | name | what and why |
|---|---|---|
| 0x10970000 + regs | hsi2c_32 | the same controller set-up as `battery_probe()` (USI13: PERIC1 gates 0x108020cc/d0, user mux 0x108002c0, SYSREG 0x1082102c = 0xa, gpe5 pins 6/7 fn 2); it already works for the fuel gauge at 0x36 |
| 0x10403010 | PERIC0 QCH_CON_PWM | CLOCK_REQ = 1, ENABLE = 0 (force the PWM block clock; HWACG gate). Original restored |
| 0x104c0000 TCFG0, TCFG1 | PWM prescaler/mux | timer-1 bits cleared to /1 (as the Linux clock driver); originals restored |
| 0x104c0018/1c/20 | TCNTB1, TCMPB1, TCNTO1 | period/compare, down-counter read-back |
| 0x104c0008 | TCON | timer-1 nibble only (bits 11:8): manual update, then autoreload + start + invert, as `pwm_samsung_enable`; stopped and restored at the end |
| 0x104d0000 +0/+8 | gpd0 CON, PUD | pin 5 -> function 2 (TOUT1), pull-down; originals restored (pin returns to its old function) |

Not touched: S2MPS17 PMIC, any other MAX77865 block, any other GPIO, any other timer channel.

PWM arithmetic (as `pwm_samsung_config`): the timer rate is MEASURED at run time against cntpct (step 3) instead of
assumed to be 26 MHz; `tcnt = rate * 48828 ns` (about 1270 at 26 MHz), `tcmp = tcnt - tduty`, hardware gets
`tcnt-1` and `tcmp-1` (the hardware counts one hidden tick).

## 3. Sequence (all logged as `HAPTIC ...` lines)

1. bus set-up + read PMICID1/PMICREV of 0x66. On failure: read-only probes of 0x36 and 0x69 (`HAPTIC diag`).
2. read the original MAINCTRL1, MCONFIG, BOOST1, BOOST2 (logged), flush log.
3. PWM clock: QCH forced on (flush), read TCON/TCFG, run timer 1 as a one-shot down-counter for 2 ms, derive
   `tin_hz`. Fails cleanly if the timer does not count.
4. gpd0-5 = function 2.
5. MAINCTRL1.BIASEN, MCONFIG (MEN 0, LRA, /128), BOOST1/2 on; 6 ms settle.
6. TCNTB1/TCMPB1, TCON manual update, then start with autoreload and invert; log TCNTO1 twice (a counting timer
   shows two different values) and gpd0 DAT.
7. flush log, MCONFIG.MEN = 1, hold `ms`, log TCNTO1 and MCONFIG mid-pulse, finish.
8. cleanup (also reached on every error): MEN = 0, MCONFIG/MAINCTRL1 restore, timer idle and TCON/TCFG restore,
   pin restore, boost restore, QCH restore, flush log.

The log is flushed (gpu0_flush) before the first PWM access, before MOTOR ON, and at the end.

## 4. Build and test

```
wsl -u root -d Ubuntu -- bash tools/haptic/_check.sh full    # compile check, flags of shim/dreamlte/build.sh
wsl -u root -d Ubuntu -- bash tools/haptic/_append.sh        # re-run after every edit of tools/haptic/haptic.c
```
`_append.sh` copies haptic.c into `shim/dreamlte/smp_probe.c` between `BEGIN/END haptic.c` markers (before the
`wifi_w2.c` block, so `tools/wifi-w2/_append.sh`, which deletes from its marker to EOF, keeps working). It has
ALREADY been run: the worktree smp_probe.c and loader.c contain the code and both compile with `-Werror`. The usual
build script copies smp_probe.c, loader.c, start.S and mkadt_dreamlte.py, so nothing else needs copying
(`touch_probe.c`, whose `battery_probe` the code mirrors, is already in `/root/work/upstream/shim/dreamlte`).

Test: build a test image with the extra boot-arg `hapticprobe=1` (a normal boot image otherwise; the pulse runs
once in the loader after the PMIC/battery/ACPM/GPU/Wi-Fi probes and just before "JUMP TO XNU"). Hold the phone in
your hand with the screen off or on; expect one short, strong buzz (about 150 ms, like a notification tick) at the
point where the loader log shows `HAPTIC step7 MOTOR ON`. Then try `hapticprobe=2` (longer) and `=3` (half
strength, noticeably softer) to prove amplitude control.

## 5. Reading a log

- `HAPTIC FAIL step1 ... does not answer`: bus problem, not the motor. `diag fuelgauge 0x36 st=0` with `0x66` dead
  means the PMIC top address is wrong or the part is the other variant; both dead means the hsi2c_32 set-up
  failed (compare with the existing `BATTERY` log lines, which use the identical set-up).
- `step1 ok PMICID1/PMICREV`: chip identified. Record both values here once seen.
- `step2 original`: starting values (is BOOST2 bit 3 already set? is MCONFIG already 0xc2?).
- `FAIL step3 ... does not count`: PWM clock or base address wrong; look at `QCH_PWM` old/rb lines (rb must show
  bit 1 = 1) and `tcnto@2ms` (0x00ffffff means the timer did not start).
- `step3 tin_hz` about 26000000 confirms the Linux clock tree reading.
- Missing `rb` equal to `wr` in `pmic wr` lines = the write did not stick (bus ok, register protected).
- `step6 TCNTO1 a/b` equal = timer not running (TCON write ignored); different = PWM generated. `gpd0 dat` bit 5
  toggling is not guaranteed to be seen in two reads.
- Everything logged OK but no buzz: electrical path is the suspect (boost not enabled: check `BOOST2.FORCE_EN rb`,
  or pin not routed: check `gpd0.CON rb` = fn 2 in nibble 5). Then try `hapticprobe=2`.
- A log that stops after `step3` flush: the PWM block access hung (clock gate); the last line says which write.

## 6. Certain vs assumed

Certain (read in Samsung source): MAX77865 haptic block and PMIC slave 0x66 on hsi2c@10970000, register numbers and
bits, PWM block address and channel 1, TOUT1 on gpd0-5 function 2, period/duty values, boost control registers,
PWM register layout and TCON bit positions, no S2MPS17 regulator is used by the driver, "pwm-clock" = 26 MHz.

Assumed: (a) the phone is the MAX77865 variant, not the DRV2624 engineering variant; (b) the PWM block can be
reached after forcing QCH_CON_PWM (CLOCK_REQ with ENABLE = 0); the Linux driver instead uses HWACG with ENABLE = 1;
S-Boot may already have it running (the original value is logged); (c) PERIC0 pin controller offset gpd0 CON = +0
(from `exynos8895_pin_banks6`), PUD at +8 with 2 bits per pin (same layout as the gpe5 bank the battery probe uses);
(d) the S-Boot state of MAX77865 haptic registers is the reset state (Linux assumes it too); (e) the QCH bit layout
is bit 0 ENABLE / bit 1 CLOCK_REQ per `cmucal-sfr.c`.

## 7. Risks

- Hang on the first access to 0x104c0000 or 0x10403010 if the clock model is wrong: mitigated by flushing the log
  right before and writing the QCH first; the boot would then stop with the last logged line identifying it.
- Wrong gpd0-5 function: only that pin, restored at the end.
- The MAX77865 boost output (BOOST2.FORCE_EN) is also near other rails of the chip: the driver toggles exactly these
  bits on every key press, and the original values are restored.
- If the motor was powered and the loader hung mid-pulse the motor could stay on: the pulse is at most 300 ms and
  the MEN-off write is the first cleanup step; there are no loops that can block after MEN = 1 other than I2C
  transfers, which have 50 ms timeouts.
- Fallback if the chip turns out to be the DRV2624 variant (`HAPTIC FAIL step1` with fuel gauge OK): hsi2c_15
  0x10860000 (USI05), slave 0x5A, `drivers/motor/drv2624.c`; reset gpe7-4, mode gpf1-6. Not implemented.
- Kernel IOKit stand-in later: the same sequence; replace the bring-up with the XNU i2c/PWM code and use the haptic
  parameters from section 1 (160 Hz frequency, 50 %..65 % duty).
