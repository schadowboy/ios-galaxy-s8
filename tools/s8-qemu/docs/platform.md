# s8-qemu platform inventory (Agent A)

Scope: what the patched XNU 8796 kernel (`xnu/out-v100/xnu-kernel/kernel.development.a5xelte`, built with patches up to 0155) and its loader
(`shim/dreamlte/`) need from the Galaxy S8 (SM-G950F, Exynos 8895, "dreamlte") platform, so that a QEMU machine can boot it.
All paths are relative to `C:\github\ios-s8\upstream\.claude\worktrees\gpu-stress` (worktree "W") unless they start with `C:\github\ios-s8\...`.

Evidence tags: **[V]** verified (read in source, or seen in a real phone log / disassembly), **[I]** inferred, **[?]** open.
Source abbreviations: `LD` = `W/shim/dreamlte`; `XP` = `W/xnu/patches`; `RC` = kernel files reconstructed by applying the patches in order
(`W/tools/s8-qemu/docs/helpers/recon.sh` writes them to WSL `/root/qa-src`; the line numbers quoted for `RC` files are in that tree and are
reproducible with the script); `LOG-K` = `C:\github\ios-s8\logs\sweetlog-round8c.txt` (kernel console of the last phone boot, image v138),
`LOG-L` = `C:\github\ios-s8\logs\loader-big1.txt` / `loader-wifi8.txt` (loader console).

Corrections to PLAN.md (confirmed by overseer note and sources):
* UFS HCI is at **0x11120000** (+ vendor block **0x11121100**), NOT 0x15570000 (`tools/mkadt_dreamlte.py:129`, `LD/ufs_probe.c:19-20`, LOG-K "SWEET UFS").
* There is **no UART**: the console is a framebuffer text console plus RAM rings (section 3.12).
* The kernel uses the GIC only for the physical timer PPI (INTID 30) and SGI 1. **No SPI is used by any driver; everything is polled** [V, RC grep].

---------------------------------------------------------------------------------------------------------------------------------------
## 1. Boot protocol (loader -> kernel)

### 1.1 What the real loader does (phone) [V: LD/loader.c, LD/start.S, LOG-L]
S-Boot loads the Android boot image; its "kernel" is the loader (arm64 Image header, `LD/start.S:9-21`), its ramdisk (`/chosen/linux,initrd-*`,
LOG-L: `initrd=0x89000000 end=0x89ff9700`, DTB `0x8a000000`) is a `SWEETXNU` v1 payload built by `tools/mkpayload.py`. S-Boot enters the loader at
EL1, MMU off, CNTFRQ_EL0 = 0, boot CPU = Cortex-A53 MPIDR 0x100. The loader (`dl_main`, `LD/loader.c:299-590`):
1. checks EL == 1; parses the payload; zeroes the whole kernel RAM window (`zero(p->phys_base, p->mem_size)`, loader.c:358);
2. copies each Mach-O segment to `phys_base + (vmaddr - virt_base)` (loader.c:361-378);
3. places the ADT at `adt_pa = kernel_end_pa`, `boot_args` at the next 4 KiB boundary after the ADT (loader.c:380-386);
4. runs all the real-hardware pokes (UFS probe, PMIC RTC read -> appends ` s8rtc=<utc-seconds-at-counter-zero>` to the command line, touch/battery/display
   probes, ACPM/DVFS, big-core clocks, optional Wi-Fi/GPU probes) - **none of that is needed under QEMU**;
5. returns the kernel entry PA; `start.S:46-60` loads `x0 = dl_boot_args` (boot_args PA), clears SCTLR_EL1.{M,C,I}, `ic iallu; dsb sy; isb`, `br x16`.
   With `s8bigboot=1` (phone default since v115) the loader instead PSCI-CPU_ON's Mongoose core 0 with `mongoose_boot_entry`
   (`LD/start.S`, worktree version) and parks the A53 - **not applicable to QEMU; do not use s8bigboot**.

### 1.2 State the kernel entry requires (what loader_qemu must reproduce) [V unless noted]
| item | value | evidence |
|---|---|---|
| entry address | PA = `phys_base + (entry_va - virt_base)`. Current payload (header dump of `/root/work/upstream/shim/dreamlte/out/payload.bin`): virt_base `0xfffffff006000000`, entry_va `0xfffffff00743b488`, phys_base `0x880000000` -> **entry_pa 0x88143b488**. Recompute with `mkpayload.py` for any new kernel (LC_UNIXTHREAD pc) | `tools/mkpayload.py:152,184`; payload header bytes |
| x0 | PA of `boot_args` | `LD/start.S:48-49` |
| x1..x3 | unused (`start_first_cpu` takes only x0) | [I] |
| exception level | EL1 (loader refuses anything else; kernel secondary path also tolerates EL2 -> EL1) | `LD/loader.c:314`, `start.S` `mongoose_boot_entry` |
| MMU / caches | SCTLR_EL1.M = C = I = 0 (kernel builds its own bootstrap page tables) | `start.S:52-59` |
| interrupts | DAIF all masked (`msr daifset,#0xf` at loader entry) | `start.S:26` |
| CPUs running | exactly one (boot CPU = MPIDR_EL1 & 0xffff == 0, `A5_BOOT_MPIDR 0x000`, patch 0155). Others are off and are started by the kernel with PSCI `CPU_ON` (SMC conduit, function id `0xC4000003`) | `RC pexpert/pexpert/arm64/A5XELTE.h`, `RC iokit/Kernel/arm/SweetCPU.cpp` |
| stack / other regs | none required | [I] |

### 1.3 Load map of a real boot (phone, LOG-L `loader-big1.txt` lines 1-20) [V]
```
0x89000000  payload (initrd)            0x8a000000  S-Boot DTB (loader only)
0x880000000 kernel RAM base (phys_base) segments (v138 build):
  seg0 0x881004000 +0x164000 (__TEXT)         seg5 0x881b44000 +0x4000     seg1 0x881168000 +0x100000 (DATA_CONST)
  seg2 0x881268000 +0x8d4000 (__TEXT_EXEC)    seg6 0x881b48000 +0x954000   seg3/4 0x881b3c000/0x881b40000 +0x4000
  seg7 0x88249c000 +0x4c000 seg8 0x8824e8000 +0x4c000 seg9 0x882534000 +0x3d4000
0x882908000 ADT (adt_pa = kernel_end)    0x88290b000 boot_args (older build; current build: kernel_end_va 0xfffffff0088fc000
                                          => adt_pa 0x8828fc000, ADT 0x2fe4 bytes, boot_args 0x8828ff000)
```
Rule (mkpayload/loader): `virt_base` = (`__TEXT` vmaddr) rounded down to 32 MiB; `kernel_end` = end of highest segment rounded to 0x4000;
`adt_pa = phys_base + (kernel_end_va - virt_base)`; `ba_pa = round_up(adt_pa + adt_size, 0x1000)`; `topOfKernelData = ba_pa + 0x1000`.
A QEMU boot stage can simply `-device loader` the already relocated segments (output of `mkpayload.build()` step "apply_chained_fixups" - the kernel is
a PIE with chained fixups that **must be pre-applied**, XNU does not do it) at these PAs. Booting at the same addresses as the phone is recommended.

### 1.4 boot_args layout [V: LD/loader.c:168-180 static asserts, 736 bytes; values loader.c:386-399]
| off | field | value written by loader |
|---|---|---|
| 0x000 | u16 Revision | 2 |
| 0x002 | u16 Version | 2 |
| 0x008 | u64 virtBase | payload virt_base (`0xfffffff006000000`) |
| 0x010 | u64 physBase | `0x880000000` |
| 0x018 | u64 memSize | `0x80000000` (2 GiB; ADT `dram-size` equal) |
| 0x020 | u64 topOfKernelData | ba_pa + 0x1000 |
| 0x028..0x050 | Video {v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth} | **all 0** (loader zeroes the page and never fills it; the framebuffer comes from the ADT node `a5xelte-fbcon`) |
| 0x058 | u32 machineType | 0 |
| 0x060 | u64 deviceTreeP | **virtual** address: `virtBase + (adt_pa - physBase)` |
| 0x068 | u32 deviceTreeLength | ADT size (0x2fe4 in the current build) |
| 0x06c | char CommandLine[608] | payload cmdline (from `make-boot.sh`) + ` s8rtc=<n>` |
| 0x2d0 | u64 bootFlags | 0 |
| 0x2d8 | u64 memSizeActual | 0x80000000 |
The `s8rtc` value is "UTC seconds at system-counter zero" (loader.c:263-297; kernel adds the counter, patch 0124 `PEGetUTCTimeOfDay`). Under QEMU the boot stage must
append `s8rtc=<host epoch seconds - guest counter seconds at entry>`; with QEMU's counter starting at 0 at machine reset this is simply the host epoch at start.

### 1.5 The ADT (device tree) [V: tools/mkadt_dreamlte.py + tools/mkadt.py(qemu=True base) + tools/adt.py; dump of the real build checked]
Built by `python3 tools/mkadt_dreamlte.py OUT.adt --canvas 1440x2960` (`make-boot.sh` picks 1440x2960 when `sweetfb_native=1` is in the boot-args, else 1125x2436).
**Needs** `research/ipsw-x/d22ap.adt` (an Apple artifact, not in git; copy at WSL `/root/work/d22ap.adt`, 186 KB) for the `/product` node: MobileGestalt reads it. Without it the tool only warns and
the product identity is missing (iOS userland will misidentify the device) [V mkadt.py:47-57].
Properties the kernel really consumes (all listed in the real 12260-byte ADT):
* `/` : `compatible` = `samsung,dreamlte|samsung,a5xelte|samsung,exynos8895|AppleARM` (IOKit platform match is `samsung,a5xelte`, `KernelConfigTables.cpp` patch 0086), `model` iPhone10,3, `target-type` D22, `target-sub-type` D22AP, `#address-cells`/`#size-cells` = 2.
* `/chosen`: `dram-base` 0x880000000, `dram-size` (0x80000000 for the v138 build; `make-boot-kernel.sh` passes it as 3rd arg and `sed`s `DRAM_SIZE` in mkadt_dreamlte.py; the source default is 0x20000000), `random-seed` (64 B, `os.urandom`), `debug-enabled`, `firmware-version` "dreamlte-entry", `boot-manifest-hash` (48 B), `crypto-hash-method` sha2-384, `display-scale` 3, `unique-chip-id`, `chip-id` 0x8015, security-mode props; `/chosen/memory-map` (empty; `mkpayload --ramdisk` can add `RAMDisk`), `/chosen/asmb` `lp-sip0`/`lp-sip1`.
* `/defaults` `no-effaceable-storage`; `/buttons` (home-button-type 2); `/product` (from d22ap.adt).
* `/cpus/cpuN` N = 0..7: per `cpu_identity` in the worktree `tools/mkadt_dreamlte.py`: **logical 0-3 = Mongoose, MPIDR 0..3, cluster 1; logical 4-7 = Cortex-A53, MPIDR 0x100..0x103, cluster 0**; cpu0 `state`=running, others `stopped`; `timebase-frequency` etc. = 26000000 (0x18cba80); `clock-frequency` 1690000000 (A53) / 2314000000 (Mongoose). The `cpus=N` boot-arg selects the first N nodes; `SweetCPU::start` panics unless `reg == expected_mpidr` (`RC SweetCPU.cpp`: logical<4 -> MPIDR=logical, else 0x100+logical-4) and `phys_id(boot cpu) == 0`.
* `/arm-io`: `device_type` dreamlte-io, `ranges` = <0, 0x10000000, 0xf0000000> (SoC base 0x10000000: child `reg`s are offsets), child `wdt` (`no-pmu`), child `gic`
  (`compatible arm,cortex-a15-gic`, `reg` = <0x0201000 0x1000 0x0202000 0x1000> -> GICD 0x10201000, GICC 0x10202000; `virtual-timer-intid` = 30), child `disp0` (`device_type display-subsystem`, `compatible samsung,dreamlte-display|samsung,a5xelte-display`, `canvas-width/height`).
* `/a5xelte-fbcon` `reg` = <0xcc000000 0x1040000...(1440*2960*4)>, `width` 1440, `height` 2960, `stride` 5760 (kernel console: `pexpert/arm/pe_serial.c`).
* `/dreamlte-ufs` `reg` = <0x11120000 0x200 0x11121100 0x200> (absolute PAs, mapped with `ml_io_map`, `RC bsd/dev/sweet_ufs.c:732-734`).
For QEMU the ADT is identical to the phone's except: `timebase-frequency` / `peripheral-frequency` / `fixed-frequency` must equal the frequency QEMU's generic timer really ticks at (section 3.2), and `dram-size` as chosen.

### 1.6 Boot-arg string and length
`CommandLine` is 608 bytes, NUL terminated, `mkpayload.py` truncates to 607. Final strings from `tools/build-v138.sh` + `make-boot-kernel.sh` are in section 6.

---------------------------------------------------------------------------------------------------------------------------------------
## 2. Memory map [V unless noted]

| range | what | evidence |
|---|---|---|
| 0x80000000-0xffffffff | low DRAM bank (2 GiB on the phone, parts reserved by firmware) | `mkadt_dreamlte.py` header comment, LD/loader.c:40-41 |
| 0x880000000-0x8ffffffff | high bank. **Kernel-managed RAM** = `dram-base`/`dram-size` of the ADT (2 GiB in v138; LOG-L `phys=0x880000000 mem=0x80000000`). `0x8f0000000` is used only by the loader's UFS probe DMA | loader.c:27,42-43 |
| 0x8f000000-0x8f000fff | **low-bank page used by the kernel**: CPU_ON trampoline (`ldr x1,#8; br x1; .quad entry`) written by `SweetCPU::startCPU` and per-core start breadcrumbs at +0x100+slot*0x20. **Must be plain RAM in the low bank** (so the low bank must cover at least 0x80000000-0x8f001000) | `RC SweetCPU.cpp` startCPU, `XP/0155` |
| 0x8f000200 | loader's parked-core mailbox (only with s8bigboot) | start.S worktree |
| 0x92080000-0x9207ffff+0x80000 | loader console ramoops zone (`DBGC` header); **0x920ff000** = 8-byte "XNUELTE1" entry marker written by `start_first_cpu` (`A5_ENTRY_MARKER_PHYS`); **0x92180000 size 0x80000** = kernel console ring ("klog") | loader.c:36-39,57; `RC A5XELTE.h`; XP/0124 pe_serial.c hunk |
| 0xcc000000 | **boot framebuffer**: 1440x2960, 32 bpp XRGB8888 little-endian (byte0 = B), **stride 5760 B (0x1680)**, size 17,049,600 B; the kernel maps 9 x 2 MiB blocks (`A5_FB_2M_BLOCKS`) VA=PA in its bootstrap tables, so 0xcc000000-0xcd1fffff must be RAM | `RC A5XELTE.h`, XP/0124 start.s hunk, `RC SweetFramebuffer.cpp:262-300` |
| 0xc1000000.. | optional "flip pool" 4 x 18 MiB (only with `sweetfb_flip=1`; default off) | `SweetFramebuffer.cpp` S8_POOL_DEFAULT_PA |
| 0x10000000-0x1fffffff | Exynos SoC MMIO window (ADT `arm-io` ranges) | mkadt_dreamlte.py:95 |
Other low-bank carve-outs of the real phone (modem, abox, TIMA, sec_debug, camera ION 0xc0400000-0xce7fffff) are not touched by the kernel; plain RAM is fine.

**Recommended QEMU RAM**: low bank 0x80000000-0xffffffff (2 GiB; or at least up to 0xd2000000 to cover ramoops, FB and the trampoline - simplest is the full 2 GiB), high bank 0x880000000-0x8ffffffff (2 GiB; 512 MiB is enough for a first boot, see open question Q1).
**Secondary bank 0x880000000**: kernel RAM proper. The kernel needs PA > 4 GiB DMA: the UFS driver allocates its DMA buffer inside it (LOG-K: `DMA at 0x883a88000`, patch 0034 `ufs-dma-above-4g`), so the emulated UFSHCI **must do 64-bit DMA** (CAP bit 24 set).
CPU requirement: PA size >= 36 bits (Cortex-A53 has 40) [I]; 4 KiB granule only (patches 0082/0084/0093: kernel is 4K-granule, 39-bit VA; A53 has no 16K granule) [V from patch titles, I for completeness].

---------------------------------------------------------------------------------------------------------------------------------------
## 3. Device table (everything the kernel touches before / around launchd)

Classification: **REQ** must be modelled with real behaviour; **STUB** blind reads/writes fine (RAZ/WI unless noted); **SKIP** switched off by a boot-arg.
Method: all `ml_io_map`/`*(volatile *)` users in the reconstructed kernel sources and patches were grepped (the kernel has **no device-tree driven IOKit hardware drivers** other than these hand-written ones), the 8 distinct MMIO consumers are listed. Anything else read blindly would be a bug in the kernel, not expected.

| # | base | size | device | users | class |
|---|---|---|---|---|---|
| 3.1 | 0x10201000 / 0x10202000 | 0x1000 / 0x1000 | GIC-400 (GICv2) | pe_init_fiq, SweetCPU, sleh.c | REQ |
| 3.2 | - | - | ARM generic timer (CNTP) | machine_routines.c | REQ |
| 3.3 | 0x11120000 / 0x11121100 | 0x200 / 0x200 | UFS HCI + Exynos vendor regs | bsd/dev/sweet_ufs.c | REQ |
| 3.4 | 0x16480000 | 0x10000 | PMU system controller | reset/poweroff, CPU power | REQ-lite |
| 3.5 | 0x12860000 (+0x128b1000..) | 0x1000 | DECON_F / IDMA | SweetFramebuffer | STUB+ |
| 3.6 | 0x108e0000 | 0x1000 | hsi2c_23 + S6SY761 touch (addr 0x48) | a5xelte_touch.c | SKIP (`s8touch=0`) or REQ for input |
| 3.7 | 0x10970000 | 0x1000 | hsi2c_32: MAX77865 fuel gauge 0x36 / charger 0x69 | dreamlte_battery_read | STUB |
| 3.8 | 0x164b0000 | 0x1000 | ALIVE GPIO (keys, touch INT) | a5xelte_keys.c, a5xelte_touch.c | STUB (value matters) |
| 3.9 | 0x13900000 | ? | Mali-G71 | s8_mali_drv.c | not touched at boot |
| 3.10 | 0x16440000 / 0x16500000 / 0x10080000 / 0x10084000 / 0x16900000 | | ACPM mailbox, APM SRAM, TMU, PLL | s8_acpm.c | SKIP (`s8dvfs=0`) |
| 3.11 | RTC / PMIC S2MPS17 / SPEEDY | | none in the kernel | - | not needed |
| 3.12 | 0x92180000 | 0x80000 | console ring in RAM (not a device) | pe_serial.c | RAM |

### 3.1 GICv2 [V: `XP/0086`, `XP/0112`, `XP/0114`, `RC SweetCPU.cpp`]
`pe_init_fiq()` (`XP/0086` pe_fiq.c hunk, changed by `XP/0124:1366`) maps GICD at `soc + reg[0]` = **0x10201000** and GICC at **0x10202000**, then (boot CPU only, but also run per CPU by `XP/0112`):
GICD_IGROUPR0 (0x080) |= bit30|bit1; GICD_IPRIORITYR0-7 (0x400..0x41c) = 0xa0a0a0a0; GICD_ISENABLER0 (0x100) = bit30|bit1 (INTID 30 = physical timer PPI 14, INTID 1 = SGI1);
GICC_PMR (0x004) = 0xff; GICC_BPR (0x008) = 0; GICC_CTLR (0x000) |= 1; GICD_CTLR (0x000) |= 1. IRQ entry: `GICC_IAR` (0x00c) read, `GICC_EOIR` (0x010) written (up to 3x for the timer), intid = IAR & 0x3ff (source CPU in bits 12:10, `XP/0114`).
IPI: `GICD_SGIR` (0xf00) = `(2u<<24)|1` (target-list-filter 2 = requesting CPU only, SGI 1) for self-IPI; other CPUs via `a5xelte_send_ipi` (target list). `SweetCPU::startCPU` for MPIDR<0x100 does `GICD_CTLR |= 1` before CPU_ON (phone-specific, harmless).
QEMU: `arm_gic` revision 2, **no security extensions** (otherwise IGROUPR semantics change) [I], num CPUs = number of cores, at least 64 IRQs. Wire the CPU's `GTIMER_PHYS` gpio to **PPI 14 (INTID 30)** (QEMU `virt` wires PHYS to PPI 14 as well). The ADT `virtual-timer-intid` = 30 selects it (`A5_USE_PHYS_TIMER`, `RC A5XELTE.h`). Only 3 interrupts exist in practice: INTID 30, SGI 1 (and the IAR spurious path 1020-1023). **Do not create SPI sources for devices; the kernel never enables an SPI.**

### 3.2 ARM generic timer [V: XP/0099, XP/0100, XP/0124 machine_routines hunks]
Physical EL1 timer only: `CNTP_TVAL_EL0`, `CNTP_CTL_EL0` (and `CNTV_CTL_EL0` set to masked); `CNTPCT_EL0` read for time; `cntfrq_el0` read once (disassembly: `mrs cntfrq_el0` at `0xfffffff00733ca54`).
The kernel's time base comes from the ADT (`timebase-frequency` = 26 MHz). S-Boot leaves CNTFRQ_EL0 = 0 on the phone and the hardware counter runs at 26 MHz (loader.c:276 divides CNTPCT by 26000000).
QEMU's Cortex-A53 counter ticks at 62.5 MHz by default (`tools/mkadt.py:33` already uses `QEMU_TIMEBASE = 62500000` for its QEMU variant). **Either** make QEMU tick at 26 MHz (property `cntfrq`/ `gt_cntfrq` if the QEMU version has it) **or** set the three ADT frequency properties to 62500000; otherwise guest time runs 2.4x fast (not fatal, but `s8rtc` math and USB/touch timeouts shift). [I]
The counter epoch is arbitrary: the kernel resets its base (`rtclock_base_abstime = 1 - ml_get_hwclock()`, XP/0100).

### 3.3 UFS host controller [V: `RC bsd/dev/sweet_ufs.c` (1083 lines), `LD/ufs_probe.c`, LOG-L, LOG-K]
**The driver does not initialise the link. It takes over a host that the bootloader left running** and talks to LUN 0 with polled SCSI. The emulated device must therefore **power up in the state S-Boot leaves**:
real readback (LOG-L lines 22-25): `CAP=0x1303ff0f VER=0x210 HCS=0xf HCE=0x0 TXPRDT=0xc RXPRDT=0xc NEXUS=0xffff UFSPRSECURITY=0xefe26492` (only used by the loader; kernel does not touch 0x11130000).
Standard registers (base 0x11120000, 32-bit accesses):
| reg | off | kernel use | emulation |
|---|---|---|---|
| CAP | 0x00 | read; needs bit 24 (64-bit addressing) if DMA PA >= 4 GiB (sweet_ufs.c "take_over_host") | return 0x1303ff0f (16 slots, 64AS) |
| VER | 0x08 | (loader only) | 0x210 |
| IS | 0x20 | `IS = IS` after every command (write-1-to-clear), also once at take-over | W1C, reflect UTRCS on completion (value not interpreted) |
| IE | 0x24 | written 0 (polled) | store |
| HCS | 0x30 | **must read with bit0 (DP) and bit1 (UTRLRDY) set** else "host not ready" and no root | constant 0xf |
| HCE | 0x34 | read only for logging (Exynos quirk: reads 0 while running) | 0 |
| UTRLBA/UTRLBAU | 0x50/0x54 | written with the DMA list PA (64-bit), **read back and compared** | store |
| UTRLDBR | 0x58 | **read: must be 0 at take-over; kernel writes 1 (slot 0 only) and polls bit0 until it clears (timeout 5 s, then ETIMEDOUT)** | on write: run the command, DMA the response, clear the bit |
| UTRLCLR | 0x5c | not used on Exynos (compiled out) | - |
| UTRLRSR | 0x60 | written 0 then 1; **read back, bit0 must be 1** | store |
Vendor block (base 0x11121100): `+0x40 HCI_UTRL_NEXUS_TYPE`: kernel does `|= 1` before ringing slot 0 (store it; no behaviour needed). `+0x00/+0x04 TXPRDT/RXPRDT entry size` read only by the loader (return 0xc).
**Descriptor formats (Exynos quirk UFSHCD_QUIRK_BROKEN_DWORD_UTRD - differs from the UFSHCI spec!)** (`sweet_ufs.c` ufs_scsi, lines 195-245 of RC; ufs_probe.c:87-140):
* UTRL (1 KiB aligned, slot 0 only, 32-byte UTRD): `dw0 = (1<<28 /*CT=UFS storage*/) | (DD<<25) | (1<<24 /*I*/)`, DD = 0 none / 1 host->device (write) / 2 device->host (read); `dw2` (OCS) pre-set to 0x0f, device writes 0 on success; `ucdba/ucdbau` (dw4/dw5) = UCD PA; **`resp = (512<<16)|512` = response offset and length in BYTES**; **`prdt = (1024<<16) | (n*16)` = PRDT offset and length in BYTES** (not dwords).
* UCD: command UPIU at +0 (byte0 = 0x01, byte1 flags 0x40 read / 0x20 write / 0 no data, byte2 = LUN, byte3 = tag, bytes 12-15 = expected data length big endian, CDB at +16), response UPIU at +512, PRDT at +1024, **16-byte entries** `{addr_lo, addr_hi, rsvd, dbc}` with `dbc = chunk-1`, **each entry <= 4096 bytes**, up to 256 entries (1 MiB bounce buffer).
* Device must fill the response UPIU: `resp[0] = 0x21` (RESPONSE), `resp[6]` = response (0), `resp[7]` = SCSI status (0 = GOOD); on CHECK CONDITION fill sense per `ufs_probe.c last_sense()` (offset 30 length, sense at 32..).
LUN/commands the kernel issues (all **polled, one at a time, under a mutex**): START STOP UNIT `0x1b` with power-condition ACTIVE (`cdb[4]=0x10`) to **W-LUN 0xD0** (must succeed, result only logged), TEST UNIT READY `0x00` (up to 3 tries), READ(10) `0x28`, WRITE(10) `0x2a` (log ring, root writes with `ufsrw=1`), SYNCHRONIZE CACHE(10) `0x35`. Block size **4096**; READ/WRITE lengths up to 256 blocks (1 MiB bounce). READ CAPACITY / INQUIRY / MODE SENSE are **not** used - geometry comes only from the GPT. LUN id in UPIU byte2: 0 for data, 0xD0 for the START STOP UNIT.
IRQ: none (IE = 0, completion polled). Classification **REQ** (Agent C). Interface details for the image: section 4.

### 3.4 PMU system controller 0x16480000 [V: SweetCPU.cpp, XP/0127, XP/0152, loader.c:600-607]
* Restart: kernel writes INFORM2 (`+0x808`) = 0x12345678 / INFORM3 (`+0x80c`) = 0x12345674 ('recovery', only the s8recov path) or 0/0 (normal), then **SWRESET `+0x400` = 1** (XP/0152 `real-halt-restart`, XP/0127). -> QEMU: treat as `system_reset`.
* Power-off: `PS_HOLD_CONTROL +0x330c` bit 8 cleared -> QEMU: `system_shutdown`.
* CPU power registers `+0x2000 + ((aff1<<2)|aff0)*0x80` (CONFIGURATION `+0`, STATUS `+4`) are only touched for **A53 secondary cores with MPIDR >= 0x101** (logical CPU 5-7), polled up to 10 ms each; not reached with `cpus<=4`. If cpus>5 are ever wanted: STATUS must mirror CONFIGURATION&0xf.
* G3D status `+0x40E4` read only by the Mali driver on open.
STUB otherwise (RAZ/WI); implement SWRESET + PS_HOLD so `reboot`/panics restart or exit QEMU cleanly.

### 3.5 Display: DECON_F 0x12860000 and how frames reach the panel [V: `RC SweetFramebuffer.cpp:135-160,395-440,664-700`]
Default path (`sweetfb_flip` unset, as on the phone): **the kernel CPU-copies each presented frame into RAM at 0xcc000000** (XRGB8888, stride 5760) - the panel scans that buffer. QEMU's display device only needs to scan out guest RAM at 0xcc000000 (1440x2960, stride 5760, pixel = 0xAARRGGBB little endian). The kernel text console writes the same buffer from the very first instruction of `start_first_cpu` (coloured breadcrumb blocks at FB+88/120/152/184/216 rows, `XP/0124` start.s hunk) - a good "kernel started" visual signal.
Register accesses that exist in the default path:
* `AppleCLCD` present thread, `sweet_fb_wait_frame_done()`: `DECON+0x40 |= (1<<13)|1` once (INTERRUPT_ENABLE); each frame: `DECON+0x4c = 1<<13` (W1C), then polls `DECON+0x4c` bit 13 up to 200 x 100 us = **20 ms per frame** (`IODelay`). With RAZ the wait always times out -> each frame costs 20 ms (not a hang, but `sweetfb_hz`/animation slows). **Recommended model: make INTERRUPT_PENDING (+0x4c) bit 13 read as 1 (or set it ~every 16 ms of virtual time).** STUB+.
* `flipProbe()` / tryFlip (only when `sweetfb_flip=1`): IDMA channels at 0x128b1000..0x128b5000 and 0x128b7000 (`SRC_SIZE 0x10`, `IMG_SIZE 0x18`, `IN_BASE_ADDR_Y 0x40`), `DECON+0x60` SHADOW_REG_UPDATE_REQ must self-clear. **Leave `sweetfb_flip` off** under QEMU.
Panel properties for the guest: logical presentation 1440x2960 when `sweetfb_native=1` (else 1125x2436 centred); ADT `disp0` canvas must match (make-boot.sh handles it).
Class: **STUB+** (RAM scan-out device for Agent E; optional DECON pending-bit model).

### 3.6 Touch: hsi2c_23 (Exynos USI v1 I2C) 0x108e0000 + S6SY761 @0x48 [V: `RC a5xelte_touch.c:620-835`]
Polled thread: INT line = `ALIVE + 0x44` bit 0 (GPA1 DAT, active low) read every 8 ms; if low: I2C command 0x60 READ_ONE_EVENT (8 bytes), 0x61 READ_ALL_EVENT; init: BOOT_STATUS cmd `0x55` -> must return 0x20, DEVICE_ID `0x52` (3 bytes, phone: ac 37 61), then write {0x30,0x01,0x00} TOUCH_FUNCTION and {0x10} SENSE_ON (retries 8x with 20 ms gaps). Event byte format (`s8ts_report`): byte0 bits 1:0 = 0 (touch event), bits 5:2 = id+1, bits 7:6 = state (1 press, 2 move, 3 release); x = b1<<4|b3>>4 (0..4095), y = b2<<4|b3&0xf. Controller register protocol used (`s8i2c_xfer`): reset via CON(0x00) bit31, timing regs 0x60/0x64/0x68/0x6c, CON 0x00 = 0x9 | 0x80 (tx) / 0x40 (rx), TRANS_CTL 0x44 (bit31 enable, bit17 stop, bit16 rx, low bits length), TXDATA 0x34, RXDATA 0x38, FIFO_STAT 0x30 (bit7 = tx full, bit24 = rx empty), INT_STATUS 0x24 (bit7 trans-done, bits 8-11 error; W1C), INT_ENABLE 0x20, ADDR 0x70 (addr<<10), 0x08/0x04 misc.
Without a model: the thread logs "no S6SY761" and stops (each transfer fails after 20 ms) - harmless. **Use `s8touch=0` for the first bring-up**; a model (Agent E) must then implement the above plus `ALIVE+0x44`.
### 3.7 Battery: hsi2c_32 0x10970000, MAX77865 FG 0x36 / charger 0x69 [V: a5xelte_touch.c:700-770, SweetBattery.cpp]
Read every 30 s: FG reg 0x06 (SOC, 1/256 %), 0x09 (VCELL, 78.125 uV/LSB), 0x0a (current); charger 0xb2 (bit6 CHGIN_OK), 0xb4 (state). With RAZ the transfer fails (20 ms polls), `SweetBattery` logs "static fallback, 100% 4200mV external=1 charging=0" (seen in LOG-K) and continues. **STUB** (or model for a battery indicator).
### 3.8 ALIVE GPIO 0x164b0000 [V: a5xelte_keys.c:22-105, a5xelte_touch.c:635]
Keys are polled every 10 ms: `KEYS_SAMPLE = (reg 0x24 & 0xff) | (reg 0x64 & 0xff) << 8` (GPA0 DAT, GPA2 DAT), pins {12,3,4,6}: Power = GPA2-4 (bit 12), Vol Up GPA0-3 (bit 3), Vol Down GPA0-4 (bit 4), Bixby/Home GPA0-6 (bit 6); **active low**. Initial sample = "stable" state, so RAZ (all 0 = all pressed) does not generate events, but **returning 0xff for both registers gives the correct released state**. Also `ALIVE+0x44` bit 0 = touch INT (see 3.6; keep 1 = inactive). STUB with 0xff defaults (inject presses by clearing bits).
### 3.9 Mali-G71 0x13900000, `s8mali_drv` / `s8agx` [V: RC s8_mali_drv.c, S8Mali.cpp, S8Agx.cpp]
* `s8mali_drv=0` (absent): no Mali service, no AGX service. iOS userland finds no `IOAccelerator` -> no Metal.
* `s8mali_drv=1`: `s8mali_drv_init()` starts a thread that calls `S8MaliStart()` (registers IOKit service `S8Mali`; **no MMIO access**) and then `S8AgxStart()` (needs `s8agx=1`). The Mali registers (GPU_ID at 0x13900000+0, PMU G3D status 0x16480000+0x40E4, `wait_for(...)` loops of 50-100 ms) are only touched in `bringup()` from `s8mali_gpu_open()`, i.e. when a user client opens the Mali service (Mesa userland, not the iOS Metal path). `S8Agx.cpp` references no `s8mali_gpu_*` function (grep) - the AGX stand-in is a pure software executor registered as `AGXAcceleratorG10P_B0` (LOG-K lines 136-141: "service S8Mali registered", "service AGXAcceleratorG10P_B0 (IOAccelerator) registered", no GPU access).
=> **`s8mali_drv=1 s8agx=1` start fine without any GPU hardware in QEMU** [V for boot, I for later Metal work]. Keep `s8gpudvfs` / `s8gputmu` OUT (they read TMU 0x10084000 and ACPM, 3.10). If a Mali user client is ever opened without hardware: `bringup()` reads PMU G3D status (RAZ -> "G3D domain not on, not touching the GPU") and returns false, so even that is safe.
### 3.10 ACPM / DVFS / TMU [V: RC s8_acpm.c:6-45,355-430,640-720]
`s8dvfs=1` starts a thread polling the AP<->APM mailbox (**0x16440000**, SRAM **0x16500000** with a `struct acpm_framework` at +0x2850), TMU **0x10080000** (CPU sensors) and **0x10084000** (GPU), PLL **0x16900000**, expecting real firmware. **Do not enable**: omit `s8dvfs`, `s8dvfs_ms`, `dvfsch`, `s8gpudvfs`, `s8gputmu`, `mngsfreq` (the thread is off unless `s8dvfs` is nonzero: s8_acpm.c:709). Class **SKIP-BY-BOOTARG**.
### 3.11 RTC / PMIC / SPEEDY / UART / watchdog
* RTC: **the kernel has no RTC driver**; wall-clock = `s8rtc` boot-arg + counter (loader reads the S2MPS17 PMIC RTC through SPEEDY and appends the arg). PMIC (S2MPS17 on SPEEDY), ACPM plugin, rail control are **loader-only** (`LD/pmic.c`, `LD/acpm.c`, `LD/mngs_clk.c`, `LD/touch_probe.c`, `LD/display_probe.c`, `LD/smp_probe.c`, `LD/gpu0.c`): not needed once a QEMU boot stage replaces the loader.
* UART: none. `serial=3` / `debug=0xc` only select kernel-internal console routing; `pe_serial.c` registers a **framebuffer console** (`/a5xelte-fbcon`). No MMIO UART exists to model.
* Watchdog: `IOWatchdog` is a pure software service ("accepts check-ins, never fires", XP/0040/0045, LOG-K); ADT `/arm-io/wdt` has `no-pmu`. Boot-args `wdt=-1 wdt_test=no_user_checkin`. No hardware.
### 3.12 Console / log channels (the QEMU "serial") [V: XP/0124 pe_serial.c hunk, RC sweet_ufs.c:840-1060, LD/ufs_probe.c:159-194]
1. **Framebuffer text console** at 0xcc000000 (`kprintf`, `printf`, `IOLog`) until `a5fb_noconsole`/`sweetfb_noconsole`/first frame hides it (phone boots use `a5fb_noconsole=1`: then it is not drawn).
2. **RAM klog ring at PA 0x92180000, size 0x80000** in `persistent_ram` format: `u32 sig = 0x43474244 ("DBGC")`, `u32 start` (next write offset), `u32 size` (valid bytes), data at +12, capacity `0x80000-12`, wraps modulo capacity; it continues after loader text if the header is sane, else re-inits and writes `"\n--- XNU console ---\n"`. **Best live console for QEMU**: read guest PA with the monitor (`pmemsave 0x92180000 0x80000 klog.bin`) or gdb; no device needed, only RAM there.
3. **UFS console ring** ("sweetlog"): kernel keeps a 4 MiB ring and every `ufslogsecs` (default 10 s; boot-arg 1..60) writes it to the **last 1025 blocks of the USERDATA partition**: block B0 = `base = first_block + nblocks - 1025` holds header `"SWEETLOG" | u32 version=1 | u32 ring size=0x400000 | u64 total bytes | u64 flush seq`, blocks B0+1.. hold the ring (byte n at ring offset n % 4 MiB). Decoder: `tools/decode-sweetlog.sh`, puller `tools/pull-kernlog.ps1`. With a file-backed UFS the host reads it straight from the image (offset `(first+nblocks-1025)*4096`). Needs `ufsrw=1` and `sweetlog` not 0.
4. The loader's own log goes to 16 blocks before that ring (`S8LOGv1`), irrelevant under QEMU.

---------------------------------------------------------------------------------------------------------------------------------------
## 4. UFS layout and disk image recipe

### 4.1 What the kernel does to find its root [V: RC sweet_ufs.c, LOG-K, LOG-L]
1. `sweet_ufs_early_init()` (from `SweetCPU::startAll`, before secondaries start; boot-arg `s8earlyufs=0` disables) and later `sweet_ufs_init()` at root mount (`rd=ufsroot0`).
2. LUN 0, 4096-byte blocks: reads **LBA 1** and requires signature `"EFI PART"` at +0; reads `entry_lba = u64@72`, `nent = u32@80` (<=... `nent*esz <= 1 MiB`), **`esz = u32@84` must be 128**; reads `ceil(nent*128/4096)` blocks from `entry_lba`.
3. Finds the entry whose UTF-16LE name (offset 56, up to 36 chars) equals **`USERDATA`** (boot-arg `ufsroot=USERDATA`; case-sensitive); first/last LBA at offsets 32/40 (4 KiB units). **CRCs are not checked.**
4. Exposes the partition as BSD disk `ufsroot0` with 512-byte logical sectors: `rd=ufsroot0` mounts the **HFS+ (journaled) volume that starts at byte 0 of the partition** (volume header at partition byte 1024; LOG-K: "jnl: replay_journal ... journal replay done", "hfs: mounted iOSRoot", then `/sbin/launchd`). Writable with `ufsrw=1 rootrw=1`.
Real values (LOG-K/LOG-L): GPT of the phone's LUN 0 has BOOT first=0x3800 last=0x5fff, SYSTEM first=0xd780 last=0x11d57f, **USERDATA first=0x141f80 last=0xee437f** (55844 MiB). Only the USERDATA entry matters.
Root image source: the HFS+ volume image is **16 GiB (512 chunks x 32 MiB)** (`tools/flash-delta.ps1` checks `chunk_mib == 32`, 512 chunks), flashed at offset 0 of USERDATA by `dd ... seek=<chunk>*32MiB`. The remaining ~39 GiB of the phone's partition are unused by the kernel except the last 4100 KiB for the log ring. So a faithful LUN is not needed: **a sparse disk with a minimal GPT and a ~16 GiB partition is enough** (apparent 16.02 GiB, ~13 GiB allocated - the root image itself is already a sparse 17179869184-byte file, 13 GB used).

### 4.2 Recipe
Inputs: the current root image in WSL, e.g. `/root/work/ios16-phone-mtl16.img` (17179869184 bytes; per-chunk manifest `C:\github\ios-s8\images\ios16-root-mtl16.json`, `.changed.txt` lists the delta vs the previous image; `images\chunks-mtl16\` only holds the CHANGED chunks, so the full image is the WSL file, assembled by `tools/assemble-root.sh` / `tools/mtl-install.sh` = base image + delta chunks `dd ... bs=33554432 seek=<i> conv=notrunc`).
```
# 1. (only if you need a newer root) assemble: tools/assemble-root.sh <base> <chunk-dir> <changed.txt> <out.img> <mnt>
# 2. wrap it into a UFS LUN image with a GPT (sparse, 4096-byte LBAs, USERDATA first LBA 256):
python3 -I tools/s8-qemu/docs/helpers/make_gpt.py /root/work/ios16-phone-mtl16.img /root/work/s8qemu-ufs0.img
```
`make_gpt.py` (written and tested on a 8 MiB image, this task) writes LBA0 protective MBR, LBA1 header (valid CRCs), LBA2-5 the 128 x 128 B entry array with the single entry `USERDATA` (first 0x100, last 0x4010ff = 0x100 + 4194304 root blocks + 4096 spare blocks - 1), copies only non-zero 1 MiB runs of the root image into the partition (holes stay holes). Result is 16.02 GiB apparent. The 4096 spare blocks hold the kernel's 1025-block log ring (`base = last - 1024`). Mounting for inspection: `losetup -o $((256*4096))` + `mount -t hfsplus -o loop,force,ro` (see `tools/assemble-root.sh`; `/root/load-hfsplus.sh` loads the module).
Reading the console ring back: `dd if=s8qemu-ufs0.img bs=4096 skip=$((last-1024)) count=1025 | tools/decode-sweetlog.sh` (check the script's expected input).
UFS device properties Agent C must report: LUN 0 only (+ W-LUN 0xD0 accepting START STOP UNIT), logical block size 4096, capacity = image size / 4096, writes persistent to the image file, SYNC CACHE = flush. [V: the commands above are the only ones issued]

---------------------------------------------------------------------------------------------------------------------------------------
## 5. IRQ map
| source | INTID | how used | evidence |
|---|---|---|---|
| ARM physical timer CNTP (EL1) | **30 (PPI 14)** | per-CPU decrementer | XP/0124 `A5XELTE_VTIMER_INTID 30`, `A5_USE_PHYS_TIMER` |
| SGI 1 | 1 | IPI / self-IPI (also verifies secondary startup, `SGI1 OK`) | XP/0086, 0112-0114 |
| CNTV (virtual timer, PPI 11 = INTID 27) | 27 | **not used** (masked); EL1 virtual timer stays disabled | XP/0124 |
| UFS HCI (SPI?), DECON frame-done, I2C, GPIO, Mali, TMU | - | **not routed; all polled** (`IE=0`; DECON "the SPI is not routed"; I2C "no XNU IRQ"; keys/touch polled) | RC sweet_ufs.c:488, SweetFramebuffer.cpp:670, a5xelte_touch.c:90 |
Real-phone SPI numbers (for completeness; Samsung DT `ref\s8kernel-tgp-p\arch\arm64\boot\dts\exynos\`): not needed since no driver registers them [I]; I did not extract them.

---------------------------------------------------------------------------------------------------------------------------------------
## 6. Boot-args for QEMU
Phone set (`tools/build-v138.sh` + `make-boot-kernel.sh`, LOG-L):
`debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=USERDATA unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 a5touchprobe=0 a5fb_keep_on=1 a5fb_noconsole=1 sweetfb_native=1 vm_compression_limit=524288 s8rtc=<n> s8recov=420 s8prof=20 dvfsch=0 s8dvfs=1 s8dvfs_ms=500 s8mali_drv=1 s8gpudvfs=546000 s8gputmu=1 s8panicwait=30 sweetfb_hz=30 sweetfb_async=1 sweetfb_fpslog=1 mngsfreq=1469 s8bigboot=1 s8agx=1 s8agxskip=59`

Recommended for QEMU (first milestone, then add back):
```
debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1
unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin wdt=-1 launchd_unsecure_cache=1 a5touchprobe=0
a5fb_keep_on=1 a5fb_noconsole=1 sweetfb_native=1 vm_compression_limit=524288 s8rtc=<host epoch> s8touch=0
sweetfb_async=1 s8mali_drv=1 s8agx=1 s8agxskip=59 s8panicwait=30
```
Drop / never set: `dvfsch`, `s8dvfs`, `s8dvfs_ms`, `s8gpudvfs`, `s8gputmu`, `mngsfreq`, `a53freq`, `s8bigboot`, `mngsexp`, `wifiprobe`, `gpuprobe` (all hardware pokes: ACPM/TMU/Mongoose clock/PCIe/Mali; `s8dvfs` is off unless set), `s8recov=N` (resets the "phone" to recovery N seconds after start via PMU SWRESET INFORM regs - drop; or keep only as a watchdog test), `s8prof=20` (profiler thread; optional), `sweetfb_fpslog`/`sweetfb_hz` (cosmetic, optional).
Needs of the args kept: `rd=ufsroot0 ufsroot=USERDATA ufsrw=1 rootrw=1` -> section 3.3/4; `s8rtc` -> section 1.4; `sweetfb_native=1` -> display 1440x2960; `a5fb_noconsole=1` -> console not drawn in the FB (use the klog ring or remove it to see text on screen); `s8mali_drv=1 s8agx=1` -> nothing in hardware (3.9); `s8agxskip=59` stand-in tuning, `s8touch=0` until Agent E models the I2C touch; `cpus=N`: N=1 first, then up to 4 (only the 4 Mongoose-numbered ADT cpus MPIDR 0..3 map to QEMU's default Cortex-A53 MPIDRs Aff0 = 0..3; cpus>4 needs the A53 MPIDR 0x100+ nodes = a second cluster, and the PMU CPU-power registers of 3.4).
Kernel-boot-arg parse used elsewhere (hardware-neutral): `ufsroot`, `ufsrw`, `ufscache`, `ufswait`, `ufslogsecs`, `sweetlog`, `s8earlyufs`, `a5keys`, `s8touch`, `sweetfb_*`, `cpus`.

---------------------------------------------------------------------------------------------------------------------------------------
## 7. Risks / open questions
**CPU / sysregs** [V: disassembly of the whole `__TEXT_EXEC` of the v100 kernel, `tools/s8-qemu/docs/helpers/scan_sysregs.py`, output in WSL `/root/qa-sys/sysregs.txt`]:
* Only **one implementation-defined register** appears: `mrs/msr s3_5_c15_c2_6` (4 instructions at `0xfffffff0072b42b4/42bc/42f8/4300`, gated by a flag byte at `0xfffffff00716d528` and a per-CPU byte at `percpu+0x10`; Apple core/CPU-extension code path). It is never executed on the phone's A53/Mongoose boots (the kernel boots there) [I: flag false]; in QEMU it would UNDEF exactly as on a real A53.
* Everything else is architectural ARMv8.0: `MPIDR_EL1` (masked `& 0xffff` - QEMU's bit 31 does not matter), `MIDR_EL1`, `CurrentEL`, `CLIDR/CCSIDR/CSSELR` (cache geometry: QEMU's A53 provides them), `ID_AA64ISAR0/1, MMFR0/2, PFR0/1, DFR0`, `AIDR_EL1`, `ACTLR_EL1` (RW; QEMU a53 has it), `OSLAR_EL1`, `MDSCR_EL1`, `DBGBVR/BCR0-15, DBGWVR/WCR0-15` (loops over the count read from ID_AA64DFR0 [I]; only on debug use), `CNTP_*`, `CNTKCTL_EL1`, `CNTFRQ_EL0`, `ISR_EL1`, `SPSel`, `TPIDR*`, `PAR_EL1`, `CONTEXTIDR_EL1`. EL2 registers appear once (in the EL2->EL1 drop path, not executed at EL1 entry).
* `commpage` uses LL/SC atomics (patch 0096), no LSE; `cortex-a53` codegen (patch 0094), `PAN` disabled (0095), no PAC. QEMU `-cpu cortex-a53` is the right model; `-cpu max` would advertise LSE/PAC/PAN the kernel never expects [I].
* PSCI: SMC conduit at EL1 (`smc #0`, `0xC4000003` CPU_ON; the loader also uses CPU_OFF `0x84000002`). QEMU handles `smc` as PSCI only if the CPU property `psci-conduit` = SMC and no EL3 is emulated - **Agent B must verify that an EL1 `smc` reaches QEMU's PSCI handler** (QEMU `arm_is_psci_call`); if it does not, a stub at the SMC vector is not possible because the kernel has no EL3. [?]
* Kernel needs `MPIDR_EL1.Aff0 = 0..3, Aff1 = 0` for logical cpus 0..3 (ADT `reg`), PSCI `CPU_ON(target_mpidr)` is called with exactly that MPIDR value (0,1,2,3) and entry **PA 0x8f000000** (trampoline in RAM) with context = logical id. QEMU's PSCI matches `target` against `mp-affinity` masked to Aff[3:0]; default QEMU MPIDR for cpu N is `N` (+ bit 31). [I]
**Hang risks (polled waits)**: (1) UFS doorbell `UTRLDBR` bit0 must clear (5 s -> -ETIMEDOUT, root mount fails); (2) UFS take-over checks HCS bits 0/1, UTRLBA/UTRLBAU/UTRLRSR readback; (3) secondary CPU start: 10 s per CPU then **panic** ("startup timeout") - first boot with `cpus=1`; (4) DECON `+0x4c` bit13: 20 ms per frame penalty only; (5) I2C touch/battery polls: 20 ms each, never fatal; (6) `SweetCPU::startAll` waits up to 10 s for all `cpus=` IOKit CPU nodes to register; (7) Mali/ACPM/TMU only with the boot-args above (not set).
**Open questions**
* Q1: kernel RAM size. Payload/ADT say 2 GiB (LOG-L `mem=0x80000000`), but LOG-K shows `vm_page_bootstrap: 127902 free pages, 3170 wired` = 131072 x 4 KiB = 512 MiB and the overseer notes 512 MiB. Perhaps the v138 image used `DRAM=0x20000000` after all (`build-v138.sh` passes `0x80000000`, while `make-boot-kernel.sh` seds `DRAM_SIZE`; the ADT inside `/root/work/upstream/shim/dreamlte/out/dreamlte.adt` dumps `dram-size = 0x80000000`). Use 512 MiB first (the kernel panicked in `kmem_range_init` with the whole 2 GiB bank on boot 1 per the `mkadt_dreamlte.py` comment; 2 GiB was later made to work) [?].
* Q2: `/product` node requires the Apple `d22ap.adt` (not in git). Agent D must copy it from WSL `/root/work/d22ap.adt`; never commit it.
* Q3: whether QEMU's generic timer can tick at 26 MHz (ADT frequency vs real) - section 3.2.
* Q4: real SPI numbers of UFS/DECON etc. are irrelevant today (all polled); extract from the Samsung DTS only if interrupt-driven drivers are added later.
* Q5: a5xelte `a5fb_noconsole=1` hides the text console on the FB; for bring-up drop it to see text, or read the klog ring.
* Q6: reconstructed kernel sources (`recon.sh`) lack hunks that modify Apple files (e.g. start.s, sleh.c, machine_routines.c); those were read from the patches directly (patch 0124, 0086, 0099, 0100), and the final kernel was cross-checked only by disassembling sysregs and by the phone logs.

### Five most likely things to block a QEMU boot
1. **PSCI/SMC at EL1** (and secondary MPIDR matching) - only matters for `cpus>1`, but the loader-less entry must also start at EL1 with MMU off and x0 = boot_args PA (QEMU `-device loader` CPU reset state).
2. **UFS take-over contract**: device must power up "already running" (HCS=0xf, CAP 64AS, 64-bit DMA above 4 GiB, Exynos byte-unit UTRD/PRDT quirk, 16-byte PRDT entries, 4 KiB LBA GPT) - any deviation = "host not ready"/timeouts = no root.
3. **Memory layout**: RAM needed at 0x880000000 (kernel), the low bank at 0x8f000000 (trampoline), 0x920ff000 (entry marker), 0x92180000 (klog), 0xcc000000 (framebuffer mapped with 9 x 2 MiB blocks) - a missing bank is a synchronous abort in `start_first_cpu` before any output.
4. **Pre-relocated kernel + exact ADT/boot_args** (chained fixups, virtual `deviceTreeP`, `/product` from the Apple ADT, matching MPIDRs, timebase vs QEMU's counter frequency, GIC reg and `virtual-timer-intid` 30 wired to PPI 14).
5. **GICv2 behaviour** (non-secure/no-security-extension model, GICD_SGIR self-target, IAR source bits) and unmodelled-device behaviour: blind RAZ in 0x10000000-0x1fffffff is fine, but `0x164b0000` keys should read 0xff and `0x12860000+0x4c` bit 13 should read 1 to avoid stalls.
