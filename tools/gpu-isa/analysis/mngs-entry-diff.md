# Mongoose secondary entry: Linux vs our loader (static, read-only, 2026-11-12)
[E]=evidence [G]=guess

## Linux path (cpu4..7) [E]
- smp.c:87-95 boot_secondary -> cpu_ops->cpu_boot; __cpu_up (smp.c:100-115) only sets secondary_data (stack/task) and flushes it, then calls cpu_boot.
- arch/arm64/kernel/psci.c:119-122 cpu_psci_cpu_boot: psci_ops.cpu_on(cpu_logical_map(cpu), __pa_symbol(secondary_entry)); nothing else before/after (postboot only debug-monitor restore, :126-134). No holding pen, no mailbox, no extra SMC, no PMU write in this path.
- drivers/firmware/psci.c:171-178: invoke_psci_fn(fn, cpuid, entry_point, 0) -> context id (x3) is always 0. fn id from DT (exynos8895.dtsi:281-287): method="smc", compatible "arm,psci" (v0.1), cpu_on=<0xC4000003>, cpu_suspend 0xC4000001, cpu_off 0x84000002. No extra args.
- Target id: DT big cores reg=<0x0 0x0..0x3> (exynos8895.dtsi:218-251), A53 0x100..0x103, so Linux passes target_cpu 0x0..0x3 (our loader also uses 0x000.., smp_probe.c:126). Entry is a kernel image physical address, 4 KB aligned in practice; ours is smp_entry (loader image, 4 B aligned).
- Cluster/core power is done by Linux BEFORE CPU_ON? No: PMU only touched by the cpuidle/hotplug code (exynos-pmu.c cal calls); no SMC in exynos-pmu.c (grep: none). So EL3 PSCI CPU_ON itself must do the work for cpu4..7 at first boot.
- Entry state [G]: standard PSCI: EL2 (or EL1 if EL3 drops to EL1 as Linux boots at EL1 on this phone), MMU/caches off, x0 = context id (0). Our stub already assumes only that.

## S-Boot (backup/sdb.img, 4 MB, "Samsung S-Boot 4.0" at 0x105888) [E]
- Not in sda*; sda7/sda8 are kernel/ramdisk (strings mngs = DVFS/ECT only). sdb contains S-Boot; sda1-6,9,11-16,sdc have no psci/mngs/cpu_on strings.
- No literal 0x82000xxx / 0xC20000xx constants as text; no "PSCI"/"cpu_on" strings in sdb. EL3 monitor (likely in LDFW/other partition, not found as readable strings) is not visible here.
- S-Boot has own SMP code: function names at 0xF5C20..: smp_handler, boot_exynos, next_available_cpu, read_clusterid, smp_init, smp_boot, smp_boot_test (offset 1006048 region). Messages: "Core %d: Power Off at previous time", "ERROR: Core %d: failed to power on : 0x%x", "MNGS L2 power off : %d" (0xF9E48 area), "MNGS STAT : cpu0..3 / l2 / noncpu" (0x1068D0). So S-Boot itself powers down the Mongoose L2 / cores at boot ("MNGS L2 power off") i.e. the big cluster is deliberately OFF at handoff.

## Candidate differences, ranked
1. [G, high] Cluster-level state: S-Boot leaves MNGS L2/NONCPU powered OFF ("MNGS L2 power off", smp strings). Linux cluster0_on (pmucal_cpu 4 steps) runs in the cal driver before CPU_ON; our loader mimics it but never checks L2 status 0x2604 / NONCPU 0x2404 (mngs-bringup.md fact 3): PMU cpu status 0xF can be true while the L2/NONCPU domain is down. Dump + wait for those to read ON after cluster0_on, before CPU_ON.
2. [G, high] Also Linux runs pmucal_lpm_init option writes (CPU/NONCPU/L2 OPTION bits, pmucal_system:2-35) and exynos_cpu_reset_enable (exynos-pmu.c:187-213; reg 0x200C bits 8/9/12) at boot; our loader does neither. A reset-disable bit would hold the core in reset while PMU shows it powered.
3. [G, med] Ordering of CPU_ON vs power: Linux has no pre-power; EL3 gets CPU_ON on a cluster whose sequencer is already enabled (opt bit0 cleared by cluster0_on). We power-cycle cfg 0->0xF around CPU_ON (copied from A53 trick); for Mongoose try plain CPU_ON only after cluster0_on settled (seq status 0x2484 bit0 must reach 1; ours stays 0x60000).
4. [G, med] Clock: CMU_CPUCL0 PLL unlocked and mux_pll=0 selects it (mngs-bringup.md fact 2). Linux cpufreq tunes a live PLL only after the first core is up; EL3 may expect OSC/switch path. Select MUX_CLK_CPUCL0_PLL=1 (switch path) before CPU_ON.
5. [G, low] x3 context id: we pass slot (smp_probe.c:163,181); Linux passes 0. Firmware might interpret it. Pass 0 for a test.
6. [G, low] Entry address: Linux gives a kernel .text PA in normal DRAM; ours 0x800b4000 loader image. If EL3 rejects/ignores entry outside a range it would still return 0. Try entry in a Linux-like location (e.g. 0x80080000+).
7. [G, low] Cache/MMU: Linux flushes secondary_data to PoC (smp.c:112-113); ours already uses caches-off flag; unlikely.
Not found: any extra SMC/PMU write around CPU_ON in Linux; any custom CPU/cluster SMC id in S-Boot strings. Not examined: S-Boot disassembly (smp_boot, read_clusterid) and the EL3 image: best next step is to disassemble sdb.img smp_boot near 0xF5C20 to see the exact PMU/L2 sequence S-Boot uses to boot a big core (it must do the same thing EL3 CPU_ON does).
