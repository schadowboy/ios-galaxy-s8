# S8 RTC write-back (patch 0151)

Goal: a time set by iOS survives a reboot (today it is lost, because nothing writes the S2MPS17 RTC) and the boot time no
longer depends on the loader's `s8rtc=` boot-arg alone. Everything is behind boot-arg `s8rtcwrite` (default off).

## Files

| File | Purpose |
| --- | --- |
| `xnu/patches/0151-s8-rtc-writeback.patch` | new `iokit/Kernel/arm/S8Rtc.cpp`, hooks in `IOPlatformExpert.cpp` (PEGetUTCTimeOfDay, PESetUTCTimeOfDay), line in `iokit/conf/files.arm64` |
| `tools/rtc/S8Rtc.cpp` | source of the new file (the patch is generated from it) |
| `tools/rtc/0151-hooks.part` | the two hand-written hunks |
| `tools/rtc/gen-patch.sh` | assembles the patch and dry-runs it (`patch -p1`) on the recovered `IOPlatformExpert.cpp` plus a `files.arm64` excerpt |
| `tools/rtc/test_s8rtc.cpp`, `test_s8rtc.sh` | host test of the pure epoch <-> register conversions (g++ -Wall -Wextra -Werror) |
| `tools/rtc/s8-speedy-rtc.c` | proposed replacement for `tools/s8-qemu/devices/s8-speedy.c` with the real layout and latch/commit (not applied) |

## Register map (RTC = SPEEDY slave 2, SPEEDY master at 0x15b50000)

Sources: `ref/s8kernel-tgp-p/drivers/rtc/rtc-s2mps17.c`, `include/linux/mfd/samsung/rtc-s2mp.h`
(the `#else` layout: `CONFIG_RTC_DRV_S2MP` is not set, only `CONFIG_RTC_DRV_S2MPS17=y` in `exynos8895-dreamlte_defconfig`),
`drivers/mfd/s2mps17_core.c`, `drivers/soc/samsung/acpm/acpm_mfd.c`, `drivers/i2c/busses/i2c-speedy.c`,
and the working loader `shim/dreamlte/pmic.c`.

| Reg | Name | Notes |
| --- | --- | --- |
| 0x00 | CTRL | bit0 BCD_EN (0 = binary), bit1 MODEL24 (1 = 24 h). Samsung programs 0x02 when it finds a reset RTC (`s2m_rtc_init_reg`: CTRL==0 or CAPSEL high nibble != 0xf) |
| 0x01 | WTSR_SMPL | not touched |
| 0x02 | UPDATE | bit0 RUDR (read update), bit1 WUDR (write update), bit2 FREEZE, bit4 AUDR (alarm update). Samsung keeps `update_reg = UPDATE & ~(WUDR\|FREEZE\|RUDR\|AUDR)` and writes `update_reg \| one command bit`, then `usleep_range(1000,1000)` |
| 0x03 | CAPSEL | `\|= 0xf0` only in the reset-init path |
| 0x04 | MSEC | exists in this layout; Samsung never reads or writes it (the `tm_msec` lines are commented out) |
| 0x05..0x0b | SEC MIN HOUR WEEKDAY DATE MONTH YEAR | Samsung bulk-reads/writes exactly these 7 (`NR_RTC_CNT_REGS`, first = `S2MP_RTC_REG_SEC` 0x05) |
| 0x0c..0x12 / 0x13..0x19 | alarm 0 / alarm 1 | not touched |

Encoding (`s2m_data_to_tm` / `s2m_tm_to_data`): binary (not BCD), SEC/MIN `& 0x7f`, HOUR: bits 4:0 hour 0..23 plus
**bit 6 (`HOUR_PM_MASK`) set when hour >= 12** (24 h mode keeps the flag), WEEKDAY one-hot `1 << tm_wday` (Sunday = 0; read with
`__fls(x & 0x7f)`), DATE `& 0x1f`, MONTH `& 0x0f` 1..12, YEAR `& 0x7f` = year - 2000 (Samsung refuses years < 2000).
This matches the loader's decode (`rtc_bootarg`: msec at 0x04, then sec, min, hour, weekday, day, month, year).

Sequences (Samsung):
* read: `UPDATE = update_reg | RUDR`, wait 1 ms, read 0x05..0x0b.
* write: write 0x05..0x0b, `UPDATE = update_reg | WUDR`, wait 1 ms. The write is held in a write buffer until WUDR;
  WUDR/RUDR clear themselves (comment in `s2m_rtc_check_rtc_time`: "clear signal of RUDR & WUDR is 1 clock delay").
* No explicit "stop" bit is used for a time write; FREEZE is only preserved as 0.

## SPEEDY protocol (copied from `pmic.c speedy()`, i2c-speedy.c bit names)

Registers: FIFO_CTRL 0x004 (bit31 reset; `rx | tx << 8` = lengths), CMD 0x008, INT_ENABLE 0x00c, INT_STATUS 0x010,
TX_DATA 0x034, RX_DATA 0x038. One random-access byte: FIFO reset + 10 us, FIFO_CTRL = LEN(1,1), INT_ENABLE = `0x80300011`
(write) / `0x80070301` (read), INT_STATUS = 0xffffffff, CMD = RANDOM(bit19) | WRITE(bit20) | slave << 15 | reg << 7, for a write
TX_DATA = byte, then poll INT_STATUS bit0 DONE (tens of microseconds), clear it, read RX_DATA & 0xff for a read.
Error bits (16 RX mode, 17 glitch, 18 end bit, 20 TX line busy, 21 TX stop bit) are only logged. The driver polls with a 10 ms
timeout per transfer and retries twice; no interrupts, no long loops. The only deliberate waits are the 1 ms settle after RUDR
and after WUDR (spin on `mach_absolute_time`), so a write costs about 12 transfers + 3 ms, a read about 8 transfers + 1 ms.

## Contention with the APM / ACPM firmware

Samsung's Linux never touches this SPEEDY master from the AP: `s2mps17_write_reg()` etc. send ACPM IPC messages
(`exynos_acpm_bulk_write(type, reg, count, buf)` on the MFD channel) and the APM firmware does the SPEEDY transfers
("APM's speedy_rx fail"). Evidence that the direct path is usable with ACPM running: the loader reads and writes PMIC registers
over 0x15b50000 after attaching ACPM DVFS (`pmic_rails(..."A53FREQ final")`, `mngs_clk.c` and `gpu0.c` use `pmic_read8/write8`).
Still, the two masters share the line (TX_LINE_BUSY error bit exists): a transfer can lose to an APM transaction, hence the retry.
Nothing arbitrates, so a write can in principle collide with an APM DVFS voltage write; the readback line in the log shows it
(timeout -> `readback failed`/-1). If this ever misbehaves, the alternative is the ACPM IPC route (command words
`type | reg` and `FUNC_BULK_WRITE | count`, see `acpm_mfd.c`), which the loader already has plumbing for (`acpm_send`).

## Kernel side

* `s8_rtc_get_utc` (from `PEGetUTCTimeOfDay`): with `s8rtcwrite` 1/2 it reads the RTC once, requires epoch >= 2024-01-01 (like the
  loader's year >= 2024), caches `base = rtc_ns - mach_absolute_time()`, later calls extrapolate with the counter. If the read fails
  or is implausible it returns false and the existing `s8rtc=` boot-arg path runs unchanged. Log: `S8RTC: time from the PMIC RTC: epoch N`.
* `s8_rtc_set_utc` (from `PESetUTCTimeOfDay`): updates the cached base and writes the seconds to the RTC (skips a repeat of the same
  second). Log: `S8RTC: wrote epoch N, readback M` (IOLog). Sub-second part is dropped; the readback is accepted within 2 s
  because the RTC ticks between write and readback.
* `s8rtcwrite=2`: if CTRL is not binary/24 h and not BCD (i.e. the RTC was reset), first do Samsung's init (CTRL = 0x02,
  CAPSEL |= 0xf0, AUDR pulse). With `=1` such an RTC is not written (log `RTC control 0x.. is not binary/24h`). BCD is never touched.
* Range 2000-01-01 .. 2099-12-31; anything else is refused and logged.
* The lock is an `lck_mtx` (uncontended in practice); `PESetUTCTimeOfDay` is believed to be called after XNU drops its clock
  lock/spl in `clock_set_calendar_microtime` (not verified here; if it were called with interrupts masked a contended mutex would panic).
* No start hook is needed (lazy mapping with `ml_io_map(0x15b50000, 0x1000)` on first use), unlike patch 0150's `S8AgxStart`.

## Assumptions to confirm on the phone

1. The RTC counts UTC (the loader treats the bytes as UTC; Samsung's Linux sets it from `tm` of system time, which is UTC).
2. Binary mode and 24 h are set (the loader prints `RTC ctrl`; expect 0x02).
3. The AP-side SPEEDY write path to slave 2 works as for slave 1 (only reads of slave 2 and writes to slave 1 are proven so far).
   Writes to 0x02 with RUDR were already done by the loader on slave 2.
4. WUDR self-clears; if not, the log shows `WUDR did not clear` (harmless).
5. `LCK_GRP_DECLARE`/`LCK_MTX_DECLARE` compile in this C++ file (patch 0133 uses them in C); if not, replace with `lck_grp_alloc_init`/`lck_mtx_alloc_init` on first use.

## Test procedure: phone

1. Build with 0151, boot the test image with `s8rtcwrite=1` (keep the real `s8rtc=` that the loader adds).
2. Boot log should show `S8RTC: time from the PMIC RTC: epoch N` (N close to the loader's `rtc unix` + uptime at that moment).
3. Set a distinctive time from iOS (or a test scaffold calling `settimeofday`): `1947589617` = 2031-09-19 13:06:57 UTC (Friday).
   Expect `S8RTC: wrote epoch 1947589617, readback 1947589617` (or +1/+2).
4. Reboot with the **stock** image (no `s8rtcwrite`). The loader prints `rtc unix` = RTC time minus counter seconds; add the counter seconds
   (about 1.. s after reset) and expect about 1947589617 plus the elapsed seconds; the raw `sec..year` line shows `.. 39 06 4d 20 13 09 1f`
   style bytes (sec 0x39, min 0x06, hour 0x4d = 13 | 0x40, weekday 0x20 = Friday, day 0x13, month 0x09, year 0x1f).
5. Restore the real time afterwards (write the current time the same way, or boot with an NTP-set value).
6. If the readback is off or the RTC reads invalid afterwards, power-cycle; the RTC has its own backup cell and Samsung's
   `s2m_rtc_init_reg` initialisation is what `s8rtcwrite=2` repeats.

## Test procedure: QEMU emulator (`tools/s8-qemu`)

**The current model does not support this test.** `s8-speedy.c`:
* time registers are at 0x04..0x0a (SEC at 0x04) while the real chip (and the loader, and Samsung's driver) has MSEC at 0x04 and SEC..YEAR at 0x05..0x0b;
  reading 0x0b returns 0, so S8Rtc decodes garbage ("invalid RTC contents" or a wrong date) and the loader's `rtc_bootarg` bails out (year 2000);
* there is no RUDR/WUDR logic: UPDATE (0x02) is a plain byte (bits are stored, never act, never clear);
* writes to the time registers are stored in `rtc[]` but reads always return `gmtime(time(NULL))`, so a write can never be read back;
* CTRL resets to 0, so `s8rtcwrite=1` would refuse to write (`not binary/24h`), `=2` would initialise it.

`tools/rtc/s8-speedy-rtc.c` is a proposed replacement (not applied, I was restricted to `tools/rtc`): real layout, CTRL reset 0x02, RUDR latches host time + offset
into a read image and clears itself, WUDR commits regs 0x05..0x0b via `timegm` as an offset to the host clock and clears itself.
With it, run the kernel with `s8rtcwrite=1` and expect: `S8RTC: time from the PMIC RTC: epoch <host time>`, then after the guest sets time
`S8RTC: wrote epoch N, readback N` (+0..2 s; the model has 1 s resolution). Without WUDR support in the emulator the expected log is
`readback` = host time and `WUDR did not clear`. To test deterministically, call `s8_rtc_write_epoch(1947589617)` from a debug path or set the
time with `date -s`-equivalent in the guest; then the next `S8Rtc` read shows the offset applied, and the loader's `rtc unix` stays on the host clock only
across a QEMU restart (the model keeps the offset in memory only).

## Result on the phone (2026-11-23, kernel CI run 45236517973, patch 0151)
* Boot 1 (`s8rtcwrite=1 s8rtctest=<real epoch + 3600>`): the kernel read the PMIC RTC (epoch 1795459438 = real time), wrote 1795462934 and read back exactly 1795462934: AP-side SPEEDY writes to RTC slave 2 work, WUDR/RUDR and the register map from Samsung's driver are right.
* Boot 2 (restore): the kernel read 1795463252 = the test value plus the ~5 min elapsed: the written time PERSISTED across a reboot and kept ticking; it then wrote the real epoch back (read back exactly). Residual error about 35 s (the epoch was baked into the image); iOS corrects it when it gets network time.
* The default image has the gate off (`s8rtcwrite` absent): `s8-ios-boot-v174-stock`. With `s8rtcwrite=1` iOS's own clock changes are written through (`PESetUTCTimeOfDay`), not yet exercised on the phone.

## Risks

* The first real write path to RTC slave 2 from the kernel; a wrong time lands in a battery-backed RTC (recoverable by writing again).
* Collision with APM-originated PMIC traffic (see above). All writes target only RTC registers 0x02, 0x05..0x0b (and 0x00/0x03 with `=2`), never the PMIC (slave 1).
* `PESetUTCTimeOfDay` call context and the C++ lock macros are unverified without a build (the patch only dry-runs with `patch`, the pure functions are host-tested).
* Writes happen for every distinct second iOS sets (NTP-style small steps included); there is no rate limit besides skipping identical seconds.
