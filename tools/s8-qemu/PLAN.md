# s8-qemu: boot our iOS 16.7 port (patched XNU + modified iPhone X root) on the PC, in QEMU

THE GOAL (user, 2026-11-18): iOS running on the PC. Not a GPU test tool: the whole system (our A5XELTE/dreamlte XNU kernel + the iOS root filesystem) boots in an
emulated copy of the Galaxy S8 platform, so kernel, userland, the AGX stand-in and every driver can be debugged on the PC (gdb, snapshots, logs from the disk image)
without flashing a phone. Everything else (GPU replay, ISA decoding) is subordinate: once iOS boots in QEMU, the stand-in executor simply runs inside it.

## What boots on the phone today (facts the emulator must reproduce)
- S-Boot loads our Android-style boot image: the loader (`shim/dreamlte/*.c|S`, aarch64, built by `shim/dreamlte/build.sh` with aarch64-linux-gnu-gcc) + the XNU kernel
  (`kernel.development.a5xelte`, arm64, Cortex-A53 / ARMv8.0 code, built in CI; no Apple-only system registers needed). Image assembled by `tools/make-boot-kernel.sh`.
- The loader initialises real hardware (UFS, PMIC over SPEEDY, ACPM/DVFS, display, touch rails, Wi-Fi/PCIe optional, big cores), builds Apple-style `boot_args` + an ADT (device tree,
  `tools/mkadt_dreamlte.py`), places everything in DRAM (DRAM base 0x80000000, 2 GiB mapped, second bank at 0x880000000) and jumps to the kernel entry.
- The kernel (patches `xnu/patches/00xx-*.patch`, notably 0086 gicv2 platform, 0124 dreamlte-s8, 0062 sweet touch/power/battery, 0133 s8-mali-iokit, 0150 AGX stand-in) uses: GICv2 (GICD 0x10201000),
  ARM generic timer, its own UFS driver on the Exynos UFSHCI (0x15570000) for the root (the root HFS+ image lives inside the UFS userdata LUN; the kernel also writes its console ring to the last 1025 blocks of that LUN),
  a CPU-copied framebuffer to the panel, polled I2C touch (S6SY761 on hsi2c_23 0x108e0000), PMIC/fuel gauge (MAX77865 on hsi2c 0x10970000), RTC seed from the boot-arg `s8rtc`, PSCI for secondary CPUs (smc), software AGX
  stand-in executing the GPU commands (needs the Mali service started: boot-arg `s8mali_drv=1`, `s8agx=1`).
- Boot args used on the phone: `vm_compression_limit=524288 s8rtc=<epoch> s8recov=420 s8prof=20 dvfsch=0 s8dvfs=1 s8dvfs_ms=500 s8mali_drv=1 s8gpudvfs=546000 s8gputmu=1 s8panicwait=30 sweetfb_hz=30 sweetfb_async=1 sweetfb_fpslog=1 ...`.
- Logs: the kernel's console ring is written to the end of the UFS userdata LUN; `tools/pull-kernlog.ps1` + `tools/decode-sweetlog.sh` decode it. With an image-file-backed emulated UFS the host can read that ring straight from the file.

## Architecture (one tool = `s8-qemu`)
```
tools/s8-qemu/
  PLAN.md                         (coordinator)
  docs/platform.md                (Agent A) device/memory/boot-protocol inventory, required vs stub-able
  qemu/                           (Agent B) patch set against upstream QEMU 10.x: hw/arm/s8-a5xelte.c (machine), meson/Kconfig hooks; build script
  qemu/hw/ufs-exynos.c            (Agent C) Exynos UFSHCI sysbus device backed by an image file
  boot/                           (Agent D) QEMU-friendly boot stage (loader_qemu) + payload packer: builds boot_args + ADT + loads kernel, no real-hardware pokes
  devices/                        (Agent E) framebuffer scan-out of the kernel's panel buffer, touch/I2C+PMIC+fuel-gauge+RTC stubs, console tap, run/debug scripts (`run.sh`, `gdb.sh`, `read-log.sh`)
  run.sh                          (Agent E) one command: build payload, start QEMU with the right -machine/-drive/-append equivalent, show the display
```
Everything builds in WSL (Ubuntu; 12 cores, 15 GB RAM). Build trees live in WSL at /root/work/s8-qemu (not on the Windows disk). Sources/scripts/patches live in the repo dir above.

### Interfaces (so agents can work in parallel)
- Machine name `s8-a5xelte`. CPUs: cortex-a53 x4 (`-smp 4`), RAM at 0x80000000 (size from `-m`), optional second bank at 0x880000000. GICv2 (GICD/GICC addresses taken from docs/platform.md; QEMU `arm_gic` device).
  ARM generic timer, PSCI (conduit smc, handled by QEMU). Everything in the Exynos MMIO window 0x10000000-0x20000000 not claimed by a modelled device is RAZ/WI (`create_unimplemented_device`) so blind probes do not fault.
- Disk: `-drive file=<userdata image>,if=none,id=ufs0,format=raw` + the sysbus UFS device property `drive=ufs0` at 0x15570000 with the IRQ the kernel driver expects. The image is a raw dump of the phone's UFS LUN layout as the kernel sees it
  (partition offsets documented by Agent A/C from the kernel's UFS driver and `tools/flash-delta.ps1` / `images/ios16-root-*.json` manifests).
- Boot: QEMU loads three blobs with `-device loader`: the kernel (`kernel.development.a5xelte`), the ADT, boot_args+loader_qemu; the CPU starts at loader_qemu's entry (`-device loader,addr=...,cpu-num=0`). Agent D defines the load addresses and writes them to `docs/platform.md` section "Load map".
- Display: a QEMU device (Agent E) exposing the guest framebuffer region (address/pitch/format from the kernel's sweetfb code and the ADT) as a QEMU console window.
- Input: touch events injected by QEMU mouse/keyboard events into a stub I2C touch device (S6SY761 protocol the kernel polls).

## Ground rules (all agents)
- Work ONLY in `tools/s8-qemu/` (worktree: C:\github\ios-s8\upstream\.claude\worktrees\gpu-stress\tools\s8-qemu) and WSL /root/work/s8-qemu. Never touch the phone/adb/fastboot, kernel patches, CI, or git (the coordinator commits).
- Do not execute any file downloaded from the internet except by building official upstream QEMU source from a pinned release (git tag v10.2.1 from https://gitlab.com/qemu-project/qemu.git or the tarball from https://download.qemu.org with its published sha256) or distro packages via apt. No pip-installing unknown packages, no curl|bash.
- Read-only inputs: repo `C:\github\ios-s8\upstream` (loader `shim/dreamlte`, `tools/`, `xnu/patches`), `C:\github\ios-s8\images` (boot images, root manifests), `C:\github\ios-s8\logs`, `C:\github\ios-s8\analysis`, `C:\github\ios-s8\ref\s8kernel-tgp-p` (Samsung kernel source: dts/register names),
  kernel binary `C:\github\ios-s8\upstream\xnu\out-v100\xnu-kernel\kernel.development.a5xelte`, current root image `/root/work/ios16-phone-mtl16.img` (WSL).
- Keep shell commands simple (the sandbox guard rejects complex compound commands); put multi-step shell in script files and run those.
- Every agent ends with a report: what was built, how to run/test it, what is verified (with evidence: command output), what is NOT done, and open questions for the coordinator.

## Milestones (coordinator integrates)
M0 QEMU machine boots a bare-metal test payload with GIC + timer + RAZ/WI window (Agent B).
M1 Kernel image starts: first kernel log bytes visible (Agents B, D, E).
M2 Kernel reaches its UFS driver, reads the root from the emulated UFS (Agents C, D).
M3 launchd starts; panics/hangs are diagnosed one by one (coordinator + agents).
M4 SpringBoard / the first UI frame appears in the QEMU window; AGX stand-in executing GPU work inside the emulation.
