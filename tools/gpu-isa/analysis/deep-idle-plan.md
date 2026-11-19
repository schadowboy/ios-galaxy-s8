# Deep idle (C2) plan for XNU on Exynos 8895

Paths relative to C:\github\ios-s8\ref\s8kernel-tgp-p.

## 1. Wake timer
- [E] DT arch timer node has `use-clocksource-only; use-physical-timer;` (exynos8895.dtsi:575-584). Linux uses the arch timer only as a clocksource; clockevents come from the Samsung MCT (`samsung,exynos4210-mct`, dtsi:603). So Linux does NOT rely on the arch timer to wake from C2.
- [E] Idle states carry no `local-timer-stop` (dtsi:259-277), but the arch timer driver sets c3stop unless `always-on` (arm_arch_timer.c:767) and the DT has no always-on.
- [G] The per-core arch timer (CNTP, PPI 30) loses state in a core power-down. Linux avoiding it as clockevent is strong circumstantial evidence. Not proven by a doc.
- [G] XNU must use the MCT per-core local timers (or a broadcast/CPU0 timer) as the wake source in C2, and re-arm CNTP on resume.

## 2. PMU / powermode programming before CPU_SUSPEND
- [E] Path: cpuidle-exynos64.c:125-165 `exynos_enter_idle` -> `prepare_idle` -> `cpu_pm_enter()` + `exynos_cpu_pm_enter()` -> `arm_cpuidle_suspend`.
- [E] exynos-powermode.c:517-545: `cpu_disable(cpu)` (line 81) calls `cal_cpu_disable(phycpu)`, which runs the pmucal cpu_off sequence. Then it takes `c2_lock`. If the next event is long enough it also does `cluster_disable` (cluster power-down, PSCI_CLUSTER_SLEEP) or SICD (system idle). Plain C2 = core-only.
- [E] Per-core and per-cluster sequences are in cal-if/exynos8895/pmucal_cpu_exynos8895.h. Cluster0 off writes PMU 0x16480000+0x1438 (CMU_CPUCL0 SYS_PWR_REG), +0x1384, +0x13F8, +0x14B8, and CLUSTER0_CPUSEQUENCER_OPTION +0x2488 (lines 90-97). Cluster1 equivalents are at +0x143C, +0x13FC and +0x24A8 (lines 105-109). The per-core cpu_off sequence is in the same header and not fully read.
- [E] Cluster entry additionally calls `mngs_reset_control` (line 95 shows the enable side).
- [E] The PSCI call is issued by the EL3 firmware (SMC, dtsi:282-285 `method="smc"`, CPU_SUSPEND fn 0xC4000001). Param 0x0010000 is power_state: bit16 = state ID/type field and affinity level 0, i.e. core-level power-down (decoded per `psci_power_state_pack`, psci.c:212-219; not verified against the PSCI 0.2 bit layout in this tree).
- [G] Without Linux's `cal_cpu_disable` write, EL3 may still handle the core. Unknown: whether the secure monitor programs the PMU itself.

## 3. Context save/restore
- [E] For a context-losing state Linux calls `cpu_suspend(index, psci_suspend_finisher)` (psci.c:274), and passes `virt_to_phys(cpu_resume)` as the entry point (psci.c:201-202). Plain WFI uses `cpu_do_idle` (cpuidle-exynos64.c:131-134), with no save.
- [E] `__cpu_suspend_enter` saves callee-saved regs, SP, and via `__cpu_suspend_save` the sysregs (sleep.S:64-125). `cpu_resume` (sleep.S:158-181) runs with MMU off at a physical address, restores from `sleep_save_sp`, then `cpu_resume_mmu` re-enables the MMU.
- [G] XNU needs, per core:
  - a physical-address resume stub in identity-mapped or low memory;
  - saved TTBR0/TTBR1, TCR, MAIR, SCTLR, VBAR, SP_EL1, TPIDR_EL1/EL0, CPACR, CNTKCTL and any Apple-patched sysregs;
  - GIC CPU interface re-init (ICC_*) and redistributor state;
  - timer re-arm;
  - FP/NEON state saved by the caller;
  - the EL3 return must come back at the stub with the MMU off, so the stub rebuilds translation, branches to virtual addresses, and resumes on the saved stack.
- [G] A failed or shallow suspend returns to the caller with an error code, which is safe.

## 4. Wake sources
- [E] Not found in the files read: the GIC wake mask. The arm64 GIC state is core-local, so wake IRQs arrive via the GIC redistributor of the sleeping core (GICv3 expected).
- [G] The MCT local timer interrupt (per-core PPI/SPI via `mct_map`, dtsi:616) and IPIs wake a core. The Exynos PMU has its own wake mask for system sleep (PMU_EINT_WAKEUP_MASK 0x60C, exynos-powermode.c:624), but that is for system sleep, not C2.

## 5. Does plain WFI clock-gate?
- [E] The pmucal_cpu_exynos8895.h grep for STANDBYWFI/WFI/OPTION found only CLUSTERn_CPUSEQUENCER_OPTION (bit0), used for cluster on/off. It has no USE_STANDBYWFI-style field.
- [G] Per-core PMU CPU_OPTION (USE_STANDBYWFI / clock gating) is not visible in this header, so plain WFI clock-gating is unproven. Cortex-A53 clock-gates the core internally on WFI in the core itself (architectural clock gating, ARM TRM). Power is only reduced, not removed; voltage stays.

## Effort estimate
- Medium-large: 3-6 focused days if EL3 behaves, longer for debugging silent hangs (no UART visibility on resume).

## Safe first step
1. Loader-side probe (not XNU): on one secondary core, issue SMC CPU_SUSPEND with param 0 (standby, no context loss) and with 0x0010000 with entry point = a tiny physical stub that writes a marker to a known RAM word and WFEs/branches to a loop. Measure whether the SMC returns, or whether the marker is written (resume path alive). Run on core 0 last, if at all.
2. Prepare the MCT driver in XNU (local timer wake) before any deep entry.
3. Only then add `cal_cpu_disable`-equivalent PMU writes.
