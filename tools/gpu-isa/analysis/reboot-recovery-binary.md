# S8 (SM-G950F, Exynos 8895) stock kernel: reboot reason handoff and reset (static analysis)

Inputs: backup/sda7.img (stock BOOT, kernel 4.4.111-17263988, DTB rev01), backup/sda8.img (TWRP RECOVERY, kernel 4.4.111, DTB rev06).
Work dir: C:\github\ios-s8\analysis\kernel-re\ (sda7_Image.bin, sda8_Image.bin, *.dtb/.dts, an.py, ann.py, d64.py).
All offsets below are file offsets in the raw Image; vaddr = 0xffffff8008080000 + offset - 0x80000 is NOT assumed, adrp math is page-relative so offsets are used consistently. Both kernels are raw (uncompressed) ARM64 Image (header "ARM\x64", text_offset 0x80000), not gzip; DTBs are in the dt region (sda7: 5 DTBs @0x22f1000.., sda8: 1 DTB @0x266c800). No kallsyms (CONFIG_KALLSYMS stripped; no kallsyms_* tables). Function names below come from log strings ("sec_reboot", "exynos_reboot_setup"); confidence high.

## Mechanism (high confidence)
1. Samsung `sec_reboot()` replaces `arm_pm_restart` (and `sec_power_off` replaces `pm_power_off`). Installed by init function at sda8 0x12efa8c: saves old arm_pm_restart ([0x1c090d0]) into [0x23bb5f8], stores 0x89d6e0 into [0x1c090c8] (pm_power_off) and 0x89d3c0 into [0x1c090d0] (arm_pm_restart). `machine_restart` (sda8 0x5ff0) calls `[0x1c090d0](reboot_mode, cmd)`.
2. `sec_reboot` (sda8 0x89d3c0..0x89d6e0; sda7 ~0x901af0..0x901e10) logs "%s (%d, %s)" with "sec_reboot", then:
   - `exynos_pmu_write(0x808, 0x12345678)` (always)
   - strcmp chain on cmd, then `exynos_pmu_write(0x80c, 0x12345670|reason)`
   - `bl 0x18390` (flush_cache_all), then calls the saved original restart handler `[0x23bb5f8](3, "sw reset")`, prints "waiting for reboot", spins.
3. `exynos_pmu_write(off,val)` = sda8 0x3ee384 (sda7 0x40fa70): `regmap_write(pmureg, off, val)` where pmureg is the syscon regmap of DT node system-controller@16480000. So registers are PMU base + 0x808 (INFORM2) and + 0x80c (INFORM3).
4. Original handler = Exynos reboot driver `exynos_reboot` (sda8 0x3ee824, sda7 ~0x40ffxx), registered by `exynos_reboot_setup` (sda8 0x12dc6xx, which ioremaps DT `reboot` node `pmu_base` 0x16480000, size 0x4000 into global [0x1e50718]). It does the final reset by a plain MMIO store, no SMC/PSCI:
```
3ee8e0: ldr x0, [x19, #0x718]      ; ioremapped PMU base
3ee8e4: mov w1, #1
3ee8e8: add x0, x0, #0x400         ; PMU SYSTEM SWRESET
3ee8ec: str w1, [x0]
```
   (string before it: "%s: Exynos SoC reset right now"; sda7 same at 0x410028-0x410034, offset 0x138.) It also does the chip-id check and RPM_STATUS poke at +0x500 first.

## (a) Uses of 0x1234567x
MOVZ #0x567x + MOVK #0x1234,lsl16 pairs (same set in both kernels; sda7 / sda8 file offsets): 0x5d1de0/0x59e258, 0x5db4d4/0x5a7550, 0x5dd0b4/0x5a90bc (0x5678, other users, likely sec_debug/dump magic), 0x828e88/0x7ddea4 (0x5678), 0x828ee8/0x7ddf00 (0x5679), 0x901b30/0x89d3fc (0x5678 in sec_reboot), 0x901dd4/0x89d6a0 (0x5670 default), 0x903098,0x9030f8/0x89e8b0,0x89e90c, 0x973bc4,0x973e00/0x909b78,0x909da8 (0x5678). Literal pool 0x12345678: two in sda7, one in sda8. The reason values 0x5671-0x567d in sec_reboot are materialized as `mov w1,#0x567x` and the shared tail at 0x89d6a4 does `movk w1,#0x1234,lsl 16` (hence only 0x5670 appears as an explicit pair elsewhere).

## (b) Value map (INFORM3 = PMU+0x80c), identical in sda7 and sda8
| cmd string | value written to +0x80c |
|---|---|
| (null cmd / any unmatched) | 0x12345670 |
| download | 0x12345671 |
| upload | 0x12345672 |
| secure | 0x12345677 |
| recovery | 0x12345674 |
| fota | 0x12345675 |
| fota_bl | 0x12345676 |
| fwup | 0x12345679 |
| em_mode_force_user | 0x1234567a |
| bootloader | 0x1234567d |
| emergency | 0x00000000 |
| debug<hex> | 0xabcd0000 \| value (0xabcd = `mov w2,#-0x54330000`) |
| swsel / sud | other (0xabc?-style) values, see 0x89d5xx |
| cpmem_on / cpmem_off / mbsmem_on / mbsmem_off | 0xabcX0002-style (0xabc5.. seen) |
| panic | no write to 0x80c |

INFORM2 (PMU+0x808) is always set to 0x12345678 first (marks "sec_reboot ran"). Recovery evidence (sda8):
```
89d440: adrp x1,#0x11d4000 ; add x1,x1,#0xd96 -> "recovery"
89d44c: bl strcmp(0x2f5500)
89d450: mov w1,#0x5674
89d454: cbz w0,#0x89d6a4      ; -> movk w1,#0x1234,lsl16 ; mov w0,#0x80c ; bl exynos_pmu_write
```
sda7: 0x901b7c "recovery", 0x901b84 `mov w1,#0x5674`. Confidence: high that recovery = 0x12345674 and download = 0x12345671 in INFORM3 at PMU+0x80c.

## (c) Reset trigger
PMU+0x400 := 1 (SWRESET) via exynos_reboot, as above. Not PSCI SYSTEM_RESET: the DT psci node lists only cpu_suspend 0xc4000001, cpu_off 0x84000002, cpu_on 0xc4000003 (method "smc"), no system_reset; the code path contains no SMC. 18 `smc #0` words exist in sda7 (11 in sda8) but none are in the reboot path. Confidence high.

## (d) DT
Both kernels: `reboot { compatible = "exynos,reboot"; pmu_base = <0x16480000>; }`, `system-controller@16480000 { compatible = "samsung,exynos8895-pmu","syscon"; reg = <0 0x16480000 0x10000>; }` (phandle 0x1f, referenced by exynos-pmu `samsung,syscon-phandle`), `psci method = "smc"`. PMU base 0x16480000 confirmed. Physical addresses: INFORM2 0x16480808, INFORM3 0x1648080c, SWRESET 0x16480400. No sec-reboot node (sec_reboot is a platform/class device with no DT binding).

## (e) Strings
"sec_reboot" (sda7 0xdeaf78, sda8 0xccccc0), "exynos_reboot_setup" (0xcf98d8 / 0xbdf358), "sw reset", "waiting for reboot", "Exynos SoC reset right now". No "reboot reason", "exynos-reboot", "sec-reboot" strings.

## Practical implication
To reach recovery from a running kernel: have the reboot syscall carry cmd "recovery" (LINUX_REBOOT_CMD_RESTART2), or write 0x12345674 to PMU INFORM3 (phys 0x1648080c) and 0x12345678 to INFORM2 (0x16480808) then write 1 to 0x16480400. For download mode use 0x12345671. The bootloader side of the contract (how it reads INFORM3) was not analysed here.
