# iOS 16 on Galaxy S8 (SM-G950F, Exynos 8895): requirement → provider → vehicle

Sources: `d22ap-adt-tree.txt` (iPhone X device tree, 259 nodes), `kexts-d22ap.txt` (239 kexts in the iPhone X
kernelcache, arm64/A11), `s8-devices.txt` (Samsung's S8 DT, 206 rows), `s8-adt-tree.txt` (the 22-node ADT we hand XNU
today), boot logs in `../logs`. Statements marked **[inference]** are mine, not read from a file.

## The vehicles (how a gap can be bridged)

| id | vehicle | what it is | cost |
|----|---------|------------|------|
| V1 | loader | our S-Boot payload: memory map, ADT, boot_args, early SoC setup (PMU, clocks, UFS wake, display unmask) | low; no OS needed |
| V2 | XNU built-in "SWEET" platform | code compiled into our XNU (open-source xnu + patches): CPU/GIC/timer/UFS/framebuffer + stand-in IOKit classes that userland expects | medium; each class needs its userland contract known |
| V3 | native kext | our own IOKit driver for S8 hardware, written against open-source IOKit; needs kext loading (prelinked kernelcache built in CI) **[inference]** | medium-high |
| V4 | reuse Apple kext | a SoC-independent kext from the iPhone X kernelcache, bound to a provider we supply | low if the provider contract is met |
| V5 | userland shim | replace/patch a daemon or framework (launchd plists, a Metal plugin, a CommCenter backend) | varies |

Today the platform is V1 + V2 only: no kext loading, 22-node ADT, stand-ins for AppleKeyStore, IOWatchdog,
IOCoreSurfaceRoot, HID (buttons, touch), AppleCLCD, battery (`logs/sweetlog-v4a.txt`).

## Requirement map

| # | iOS needs (iPhone X provider) | S8 provides (from DT) | vehicle | status |
|---|---|---|---|---|
| 1 | CPU cores: A11 Monsoon/Mistral, ADT `cpus`, AppleARMPlatform | 4× Cortex-A53 (MPIDR 0x100-3) + 4× Mongoose M2 (0x0-3), PSCI | V1+V2 | A53 up via PSCI+PMU. Mongoose: cores powered but **PLL_CON0 ENABLE=1 STABLE=0** at handoff (loader dump); PLL cycling test result still to be read from disk |
| 2 | Interrupts: AIC (`arm-io/aic`) | GICv2 (GIC-400) at 0x10201000 | V2 stand-in | works |
| 3 | Timer: arm timer + `aic-timebase` | arch timer 26 MHz (CNTFRQ=0 at handoff) | V2 | works |
| 4 | RAM + IOMMU: `mcc`, one DART per DMA block (`dart-disp0`, `dart-jpeg0`…) | DRAM banks 0x80000000 / 0xc0000000 / 0x880000000; SMMU only on cam/ISP | V1+V2 | 2 GiB bank given; full use of 4 GiB open. DARTs need a null/passthrough provider before any Apple kext that expects one can load **[inference]** |
| 5 | Storage: ANS2 NVMe (`ans`, `sart-ans`), IONVMeFamily | UFS host exynos-ufs 0x11120000 | V2 | read/write works (root mounted rw). Throughput not measured |
| 6 | Display: `disp0` + `mipi-dsim`, AppleCLCD, AppleMobileDispH10P | DECON0 0x12860000 + DSIM 0x12870000 + DPP×7, panel in command mode | V2 | works via S-Boot framebuffer + CPU copy at SwapEnd; no vsync, no hardware planes |
| 7 | GPU: AGXG10P kext (47 KB) + AGXFirmwareKextG10PRTBuddy; Metal plugin in userland picks it by IOAccelerator properties **[inference]** | Mali (arm,mali) 0x13900000, job/mmu/gpu IRQs 60/61/59, pd_g3d | V2/V3 + V5 | nothing. See "GPU" below |
| 8 | Touch: AppleMultitouchSPI/Driver (6140.x) over SPI | sec_ts (Y761) on hsi2c23 0x108E0000, attn IRQ | V3 | polled stand-in works; native IRQ-driven driver open |
| 9 | Power/PMIC/battery: ApplePMGR, AppleD5500, AppleSmartBatteryManagerEmbedded, AppleT8015PPM | S2MPS17 (SPEEDY), S2MPB02, S2DOS03, MAX77865 | V2/V3 | battery % shown (fuel gauge read OK in log); no real power management |
| 10 | USB: `usb-complex`, IOUSBHostFamily | DWC3 OTG 0x10C00000 + usbdrd-phy | V3 | nothing; **would give a dev channel** (see priorities) |
| 11 | Wi-Fi: IO80211Family + AppleBCMWLANCore (535 KB) + AppleBCMWLANBusInterfacePCIe, provider AppleEmbeddedPCIE | WLAN via PCIe0 0x116A0000 (DT: `brcm-wlan` node; chip ID NOT in DT) | V4+V3 | candidate: Apple's Broadcom stack + our Exynos PCIe host. Needs chip ID + firmware match - **phone question** |
| 12 | Audio: AppleEmbeddedAudio, AppleCS42L75Audio, AOP audio | ABOX 0x13E50000, Cirrus CS47L93 codec (SPI), MAX98506 amp | V3 | nothing |
| 13 | Sensors: AppleSPU/AOP (IMU, ALS), AppleEmbeddedLightSensor | BCM4773 sensor hub (SPI), HRM, fingerprint | V3 | nothing |
| 14 | Camera: AppleH10CameraInterface, ISP | 4× CSI, FIMC-IS style | V3 | nothing; far |
| 15 | Cellular: CommCenter ↔ AppleBasebandPCI | Shannon CP over shd-ipc mailbox 0x15B40000; modem firmware in `RADIO` partition (backed up) | V3+V5 | nothing; last |
| 16 | Security: SEP, AppleKeyStore, AMFI trust caches, Image4 | no SEP; TEE differs | V2 stand-ins | permissive stand-ins work; Touch/Face ID impossible |
| 17 | Filesystem/security kexts: apfs, hfs, sandbox, AMFI, corecrypto, IOHIDFamily, IOSurface | n/a (software) | V2 now, V4 later | HFS/sandbox/AMFI built into our kernel; HID/IOSurface are stand-ins |

## APM / ACPM findings (Mongoose bring-up, 2026-11-11) - paused, not solved

Measured on the phone with the loader (logs\loader-v9..v16.txt), sources in samsung-src\drivers\soc\samsung\acpm:
- The APM co-processor (SRAM 0x16500000, mailbox 0x16440000, framework table at SRAM+0x2850) is alive; my loader-side
  client (tools-new\acpm.c) speaks its protocol: loopback on the framework channel 4 works.
- Plugin 3 `exynos8895_acpm_dvfs.fw` (23552 B at SRAM 0x3800) is NOT loaded at boot. The blob lives as a builtin firmware in
  the recovery kernel (tools\extract_builtin_fw.py -> analysis\fw\dvfs_recovery.fw; proprietary, never commit). The loader detaches
  plugin 2, copies the blob, attaches plugin 3 exactly like Samsung's plugins_init(); the APM firmware log confirms it
  (`dyn_plug`, `attachid 3`, `dyn_done`).
- The DVFS plugin owns channel 3 (owner mask 0x8), not channel 5 as Samsung's DT says. DVFS commands are fire-and-forget
  (no reply). The APM log shows `ipc_dvfs` for each request.
- fvmap in SRAM (0x9400): CPUCL0 18 levels 741-2808 MHz (init level 14 = 1066 MHz at 675 mV), CPUCL1 12 levels (init 1053 MHz, 762.5 mV).
- PMIC rails BUCK2 (Mongoose) 675 mV and BUCK3 (A53) 762.5 mV are already programmed and match the table; CTRL enable bits are `01`
  (gated by the SoC's PWREN). Requesting a different level (741 MHz) changed neither the PLL nor the rail while the cluster is off.
- Mongoose CMU_CPUCL0 PLL_CON0 = 0x81ec0312 (ENABLE=1, STABLE=0, 1066 MHz) never locks; ENABLE cycling does not help; PSCI CPU_ON
  returns 0 and the PMU reports the core powered (cpu st 0x10070f0f) but the core never runs.
- Open: what makes the PLL lock / releases the core from reset (secure-monitor behaviour for cluster 0? PWREN path? PLL supply?).
  Resume only after higher-value items (boot stability, display speed, dev channel). The four A53 cores run iOS today.

## GPU (the biggest unknown)

- The A11 GPU driver pair is `AGXG10P` (kernel) + a userland Metal plugin. Userland never talks to Mali; it submits AGX
  command streams through an IOAccelerator user client **[inference]**.
- Option A (V5 first): ship our own Metal plugin (an `MTLDevice` implementation) selected via the IOAccelerator
  stand-in's properties, translating Metal → Mali (Bifrost) jobs through a small kernel Mali driver (kbase-like). Needs: Metal
  plugin loading rules on iOS 16 (to verify on the iPhone X root image), Mali job submission, Mali MMU/GPU VA setup.
- Option B (stop-gap): keep CPU rendering, make it fast (all 8 cores, NEON, dirty-rects, hardware plane via DPP).
- Option B is needed regardless and is the next display step.

## What blocks what (dependency order)

1. **Dev channel**: without a way into the running iOS, every experiment costs a TWRP trip. Candidates: DWC3 device-mode
   driver exposing a serial/ECM interface (V3, self-contained), or Wi-Fi + sshd (needs #11, bigger). USB first.
2. **Boot stability**: an intermittent kernel panic after AMFI init (`write_pte_fast: PTE write is unaligned`,
   caller `pmap_enter_pte`) hits roughly 5 of 6 recent boots. Diagnostic patch 0125 is building in CI.
3. **CPU**: Mongoose PLL (needs the v8 loader log), then big-core start-up, then SMP in XNU (`cpus=4`/`8`).
4. **Display speed**: DPP/plane scanout instead of CPU copy.
5. **Native touch**, **battery/PM**, then **Wi-Fi**, **GPU (Metal plugin)**, **audio**, **sensors**, **camera**, **cellular**.

## Open questions only the phone can answer

- Mongoose PLL lock result (already on disk from the v8 boot, needs TWRP to read).
- Wi-Fi chip ID and firmware file names (read `/sys/bus/pci/devices/*` and `/vendor/firmware` equivalents from TWRP or the
  Android backup; the Android system partition is gone from USERDATA but `sda17 SYSTEM` is untouched).
- Actual usable RAM in XNU (add a one-line kernel print of `max_mem`, `sane_size`).

## Open questions analysis can answer (no phone needed)

- Exactly which classes SpringBoard/backboardd/mediaserverd look up at startup (from the iPhone X root image): decides the
  stand-in list. Source: binaries in the root image + `launchd` plists.
- Metal plugin discovery on iOS 16 (which bundle, which IOAccelerator properties).
- Which Apple kexts load without SEP/AIC/DART providers, and whether a prelinked collection can be built in CI.
