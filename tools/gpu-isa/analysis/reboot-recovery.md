# S8 (SM-G950F, Exynos 8895) reboot into recovery from bare metal

Static analysis only. Confidence: HIGH = seen in sources, MED = inferred from a
sibling SoC that was tested on hardware, LOW = guess.

## 1. Reboot-reason register and magic values

Evidenced in our tree (sibling SoC, Exynos7580 on A5XELTE, device tested):
`upstream/shim/a5xelte/probe4k.S:108-117`

    /* Exynos7580 Linux recovery path: INFORM2, INFORM3, then SWRESET. */
    ldr x0,=0x10460000          ; 7580 PMU base
    str 0x12345678 -> [x0+0x808]  ; INFORM2
    str 0x12345674 -> [x0+0x80c]  ; INFORM3
    dsb sy
    str 1 -> [x0+0x400]           ; SWRESET
    dsb sy ; halt: wfe

`upstream/docs/a5xelte-bringup.md:23` says this is the "tested" path that lands
in TWRP on the A5. So on 7580: recovery = INFORM2 0x12345678 + INFORM3 0x12345674
(HIGH for 7580, written text says tested).

Exynos 8895 PMU base is 0x16480000 (HIGH):
`samsung-src/arch/arm64/boot/dts/exynos/exynos8895.dtsi:180` (`reboot { pmu_base = <0x16480000>; }`),
`:324-326` (`pmu_system_controller ... reg = <0x0 0x16480000 0x10000>`),
and our loader `upstream/shim/dreamlte/smp_probe.c:16` (`#define PMU 0x16480000UL`),
`upstream/xnu/patches/0124-dreamlte-s8.patch:834` (`ml_io_map(0x16480000, 0x10000)`).
(The task text said 0x10480000; that is wrong for 8895.)

NOT evidenced: the Samsung 8895 reboot driver (exynos-reboot.c / sec-reboot) is
NOT in `samsung-src` (greps for INFORM, SWRESET, SYSIP_DAT, SEC_DEBUG, reboot_mode,
RESET_REASON found nothing relevant; only the dtsi `reboot` node, which has just
`pmu_base`). So the 8895 register/magic are MED/LOW:
- Best guess (MED): same scheme as 7580 and other Samsung Exynos of that era:
  INFORM2 = PMU+0x808, INFORM3 = PMU+0x80C, i.e. 0x16480808 / 0x1648080C, with
  recovery INFORM3 = 0x12345674 (INFORM2 0x12345678). Download mode magic: not
  in sources (LOW guess: INFORM3 0x12345671 / "download" in Samsung's
  `sec_reboot`; unverified). On 8890/8895 Samsung moved some fields to
  SYSIP_DAT0 (PMU+0x810) (LOW guess, from memory of the Samsung tree).
- Safe approach: write all candidates (0x808, 0x80C, 0x810) the way A5 did; extra
  INFORM writes are harmless on a path that is then reset. Verify by reading
  back INFORM2/3 once in the loader or in TWRP (`devmem 0x16480808`) after a
  Linux `adb reboot recovery` to learn the true values on the S8 itself
  (TWRP is Linux with the Samsung driver, so this is the definitive check).
  That is a read-only action on a running recovery: but boot from TWRP and
  run `reboot recovery`; the value is lost on reset unless read from
  /sys; better: in TWRP `devmem` after the reboot has been requested is not
  possible. Alternative: read the 8895 `boot.img` / recovery kernel strings
  (`tools/twrp`) for the constant: search the recovery kernel binary for
  0x12345674 (see section 5).

## 2. How the reset is done

- A5/7580 evidence (HIGH, tested): write reason, `dsb sy`, then PMU SWRESET
  (PMU+0x400) = 1. The reason must be written BEFORE the reset; INFORM regs sit
  in the ALIVE domain and survive a SWRESET (that is the whole mechanism; the
  A5 probe relies on it, probe4k.S:108-117).
- 8895: the dtsi PSCI node is PSCI 0.1 with only cpu_suspend/off/on IDs
  (`exynos8895.dtsi:306-312`: 0xC4000001, 0x84000002, 0xC4000003, method smc),
  no system_reset ID. `NOTES.md` PSCI 1.1 lines are for the Qualcomm sweet, not
  the S8. So PSCI SYSTEM_RESET (0x84000009) on the S8 is untested (LOW guess,
  likely unsupported by the Samsung EL3 monitor).
- SWRESET at PMU+0x400 for 8895: not seen in the 8895 sources (only
  RESET_LPI_TIMEOUT 0x0420 in `pmucal_system_exynos8895.h:82-83`). Linux driver
  on 8895 uses it too (MED guess; 8895 Samsung exynos-reboot writes SWRESET
  0x400 = 0x1, same as 7580).
- Fallback (MED-LOW): PS_HOLD_CONTROL PMU+0x330C, bit 8 data, bit 31 (set in
  `pmucal_system_exynos8895.h:76-77`) - Samsung's power-off uses it
  (`PS_HOLD` low = power off, not reset), so avoid it for reboot.

## 3. What happens today on reboot / panic

- Loader (`upstream/shim/dreamlte/loader.c:429-444`): exception handler prints
  and loops `wfe` forever: HANG. `start.S:61,82,120`: `wfe` loops; start.S:79-81
  issues PSCI CPU_OFF (0x84000002).
- XNU patch 0124: no override of PE_halt_restart / no SYSTEM_RESET / no SWRESET
  anywhere in `xnu/patches` (grep found none). The only PMU use is the CPU_ON
  helper (0124 patch lines 832-858). `PLATFORM_PANIC_LOG_DISABLED` is defined
  (patch :1505). Stock xnu `PE_halt_restart` for this platform is therefore
  whatever the generic arm pexpert does (LOW guess: spins / panics, no real
  reset). Observed behaviour per notes: forced reboot is done by hand (power
  button hold) - see loader.c:12,47,351 comments ("forced reboot into TWRP").
  Consequence: today iOS reboot/panic HANGS, no watchdog reset is wired.
- No wdt code found in shim/dreamlte or in 0124 (HIGH, grep).

## 4. Code sketch

PMU mapping already exists as a pattern in 0124 (SweetCPU.cpp:832-838,
`ml_io_map(0x16480000, 0x10000)`). Put this in a new function in the same
patch (e.g. `bsd/dev/sweet_sample.c` or `a5xelte_touch.c` area, or
`pexpert/arm/pe_serial.c`) and call it from the timer/thread:

    static void
    s8_reset_to_recovery(void)
    {
        static vm_offset_t pmu;
        if (!pmu) pmu = ml_io_map(0x16480000, 0x10000);
        volatile uint32_t *r = (volatile uint32_t *)pmu;
        r[0x808 / 4] = 0x12345678;   /* INFORM2  (7580 values, verify on 8895) */
        r[0x80c / 4] = 0x12345674;   /* INFORM3  recovery */
        __builtin_arm_dsb(DSB_SY);
        r[0x400 / 4] = 1;            /* SWRESET */
        __builtin_arm_dsb(DSB_SY);
        for (;;) __asm__ volatile("wfe");
    }

Boot-arg hook (pattern exists: `PE_parse_boot_argn("s8touch", ...)` at
0124 patch :368; kernel thread with `clock_interval_to_deadline` /
`assert_wait_deadline` / `thread_block` in `s8ts_wait`/`s8ts_thread`,
0124 patch :262-275, file `bsd/dev/a5xelte_touch.c`). Add a tiny kernel thread
started from the same init function:

    uint32_t secs = 0;
    if (PE_parse_boot_argn("s8recov", &secs, sizeof(secs)) && secs) {
        kernel_thread_start(s8recov_thread, (void *)(uintptr_t)secs, &th);
    }
    static void s8recov_thread(void *a, wait_result_t r) {
        uint64_t dl;
        clock_interval_to_deadline((uint32_t)(uintptr_t)a, NSEC_PER_SEC, &dl);
        assert_wait_deadline(&s8recov_ev, THREAD_UNINT, dl);
        thread_block(THREAD_CONTINUE_NULL);
        s8_reset_to_recovery();
    }

Better for hang-proof use (kernel may deadlock before threads run): also do the
check in `sweet_early.c` / a hardware path independent of the scheduler, e.g.
poll the arch counter in the kprintf/serial path (`pexpert/arm/pe_serial.c`
or `osfmk/console/serial_console.c`, both touched by 0124) and reset when
`mach_absolute_time()` passes the deadline - this fires even if the scheduler
is stuck, as long as something still prints.

## 5. Next verification steps (not done, would need no phone access)

- Search the TWRP recovery kernel / ramdisk under `C:\github\ios-s8\tools\twrp`
  and any 8895 kernel images in `backup\` for the byte pattern 0x12345674 /
  0x12345678 and for "SWRESET"/"reboot reason" strings, plus decode the
  `exynos-reboot` function from the vmlinux symbol table to confirm offsets
  0x808/0x80C/0x400 on 8895. This was not done here.
- First hardware test of the sketch should be with the real recovery values
  confirmed; a wrong INFORM3 makes S-Boot do a normal boot (loop into our own
  XNU) rather than brick, so risk is low (guess).
