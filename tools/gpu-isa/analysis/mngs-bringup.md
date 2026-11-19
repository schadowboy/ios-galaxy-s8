# Mongoose (CPUCL0) bring-up: why the PLL never locks and the PSCI core never runs (static analysis, 2026-11-12)

Tags: [E] = evidence (file:line, log line, or arithmetic), [G] = guess / inference. Nothing here was run on the phone.
Log reference: logs\loader-v31.txt, section "MNGS: begin" .. "MNGS clk dump after".
Sources: samsung-src\drivers\soc\samsung\cal-if\exynos8895\{cmucal-sfr.c,cmucal-node.c,pmucal_cpu_exynos8895.h,pmucal_system_exynos8895.h},
samsung-src\drivers\soc\samsung\exynos-pmu.c, arch\arm64\kernel\{psci.c,smp.c}, upstream\shim\dreamlte\{smp_probe.c,start.S,pmic.c},
WSL /root/work/upstream/shim/dreamlte/mngs_clk.c (staging copy tools\mngs-stage has acpm.c, loader.c; mngs_clk.c exists only in WSL).

## 0. Facts that frame everything

1. [E] CPUCL0 PLL registers (cmucal-sfr.c:799-800, 4320-4325; cmucal-node.c:57-63, 88): CMU_CPUCL0 base 0x16800000,
   LOCKTIME +0x000 (LOCK_TIME [19:0]), PLL_CON0 +0x120 (S [2:0], P [13:8], M [25:16], STABLE b29, ENABLE b31), type PLL_1050X
   (the A53 PLL is PLL_1051X, a different macro), rate table (M,P,S): 2860 MHz (330,3,0), 2160.6 (831,5,1), 1924 (444,3,1),
   1066 (492,3,2), 400 (200,13,0). The only other CPUCL0 PLL registers Samsung touches are PLL_CON2 +0x128 bits [29:28] (pmucal_system:150,
   170, 187: written to 0x1 in the FAPO sleep paths, same for every PLL) and the PLL_CON1/3/4 words (save/restore only, no field names).
   There is NO LOCK_EN / BYPASS / AFC field in the CPUCL0 sfr table: ENABLE + STABLE is the whole documented handshake. So
   "BYPASS -> ENABLE -> wait lock" does not exist as separate bits; the bypass is the mux (next item).
2. [E] The clock path of CPUCL0 (cmucal-node.c:525-527, 301-305, 621-624, 1001, 1143-1148; cmucal-sfr.c:797-803, 586-588):
   `MUX_CLK_CPUCL0_PLL` (+0x1000, SELECT b0, BUSY b16) has parents {PLL_CPUCL0, MUX_CLKCMU_CPUCL0_SWITCH_USER}. SELECT=0 is the PLL itself,
   SELECT=1 is the switch path. The switch path is `PLL_CON0_MUX_CLKCMU_CPUCL0_SWITCH_USER` (+0x100, MUX_SEL b4, BUSY b7; parents
   {OSCCLK_CPUCL0 = 26 MHz, CLKCMU_CPUCL0_SWITCH}) fed from CMU_TOP (base 0x15A80000): `MUX_CLKCMU_CPUCL0_SWITCH` +0x1038 (SELECT [1:0], parents
   {PLL_SHARED0/2, PLL_SHARED1/2, PLL_SHARED2, PLL_SHARED4}), `DIV_CLKCMU_CPUCL0_SWITCH` +0x1838 ([2:0]), gate +0x2040.
   The loader log says `mux_pll=0x0 sw_user=0x0`. The loader's header comment calls this "OSC selected"; by the cmucal tables that is wrong:
   mux_pll=0 means **the cluster clock is the PLL**, and sw_user=0 only says the (unused) switch path would be the 26 MHz oscillator.
   For the A53 cluster the same values (mux_pll=0) are fine because its PLL is STABLE. For CPUCL0 this means the cluster CMU feeds the
   unlocked PLL to the core domain. [E for table, G that the HW then delivers no usable clock]
3. [E] PMU cluster registers (exynos-pmu.c:84-123, pmucal_cpu:98-112, loader dump): per cluster c: CPUSEQUENCER_STATUS 0x2484+0x20c,
   OPTION 0x2488+0x20c, NONCPU status 0x2404+0x20c (cfg 0x2400), L2 status 0x2604+0x20c (cfg 0x2600). `cluster_state()` = L2 status & 0x7 and NONCPU status & 0xF.
   Stock `cluster1_on` WAITs for status bit0 == 1 (pmucal_cpu:102). `cluster0_on` has no wait (pmucal_cpu:82-89), but `cluster0_status`
   reads the same bit (0x2484 bit0, :99). Loader: A53 cluster `cl1 seq st=0x1 opt=0x0`; Mongoose `cl0 seq st=0x60000 opt=0x1001`,
   and AFTER the probe still `st=0x60000 opt=0x1000` with cpu0 `cfg=0x8000000f st=0x10070f0f` (log "MNGS clk dump after").
   [E] So the sequencer bit0 that stock reads as "cluster 0 enabled" never became 1, even after option bit0 was cleared and a core
   was powered. The loader reads 0x2484 right after the option write and never again, and never reads NONCPU/L2 status in this boot.
4. [E] Rail gating: `B2CTRL=0x78`, `B3CTRL=0x58` (S2MPS17 reg 0x1b/0x1d, pmic.c:110-115). pmic.c:37 defines LDO_EN = 3<<6 for "on";
   BUCK CTRL bits [7:6] = 01 in both, so BUCK2 (VDD_MNGS) and BUCK3 (A53) are "on only while the SoC's PWREN is asserted" (mapping.md:54
   says the same). The APM can change B2OUT (0x3c -> 0x35) regardless of that; the readback only proves the voltage SETTING, not that
   the output is enabled. The A53 cluster has a running sequencer (cl1 seq st=1) so PWREN for BUCK3 is up; for BUCK2 nothing shows it is.
5. [E] The A53 cores start only with "PSCI CPU_ON (records entry) -> PMU power edge 0 -> 0xF -> CPU_ON again" (smp_probe.c comments, mapping.md:24).
   Plain CPU_ON returns 0 and nothing runs (v3-v6): the EL3 PSCI on this unit does not power cores itself.
   The Mongoose attempt (v31) already uses this order, so the CPU_ON order itself is probably not the bug.

## 1. Ranked hypotheses

### (a) CPUCL0 PLL STABLE never sets (also not in the boot state)

| # | Hypothesis | Evidence for | Evidence against / unknown | Conf. |
|---|---|---|---|---|
| a1 | The CPUCL0 power sequencer is not running (`cl0 seq st` bit0 = 0, opt bit0 = 1 at boot), so the PWREN pin of BUCK2 is low, VDD_MNGS is physically off, and the PLL macro / lock detector (supplied from, or clocked through, the cluster domain) cannot lock. APM tunes the PMIC voltage register and the PLL words happily (they are plain registers) but the analog block has no supply. | [E] cl0 vs cl1 sequencer status 0x60000 vs 0x1; [E] B2CTRL EN=01 (PWREN-gated) like BUCK3; [E] STABLE=0 even in the untouched boot state while the A53 PLL (same layout, rail gated by a running sequencer) is stable; [E] STABLE never set after APM reprogramming although the APM clearly executed (PLL and B2OUT changed). | [G] we do not know whether the CPUCL0 PLL is on the VDD_MNGS rail or on the INT/ALIVE rail. CMU_CPUCL0 registers read fine with the rail presumably off, so the CMU logic is on another supply; the PLL macro may be too. | 40% |
| a2 | The reference/"CLKSTOP" gating: the PLL reference OSCCLK_CPUCL0 or the CMU_CPUCL0 internal controller is gated by the embedded-CMU / CLKSTOP state that is only released by the cluster power sequence (EMBEDDED_CMU_CPUCL0 QCH/CLKSTOP, `CPUCL0_CMU_CONTROLLER_OPTION` +0x800, `CPUCL0_SHORTSTOP` +0x820, `CPUCL0_HCHGEN_CLKMUX` +0x850; pmucal_system:86,1493,1499). | [E] these exist and are saved/restored; [E] qch/clkstop cfg/st are already 1/1 (so the two documented gates are open). | [G] the other CMU_CPUCL0 controller words (+0x800, +0x820, +0x850) are not dumped yet; they are cheap to read. | 15% |
| a3 | STABLE is simply not reported while the cluster domain is off (the lock detector lives in the cluster domain, reads back 0), the PLL may actually be running. Then the core problem is not the PLL lock but the clock/reset of the core. | [G] A "stuck 0" bit with a correct M/P/S is consistent with that; PLL_1050X vs 1051X difference suggests a cluster-local PLL. | No way to tell without a frequency measurement. | 20% |
| a4 | PLL_1050X needs another control word (PLL_CON1/3/4 +0x124/0x12c/0x130) in a non-reset state; the APM's own procedure does it, but it is bypassed when the APM believes the cluster is off. | [G] the APM accepted the request, wrote M, and "failed" silently (rc=0, echo). | [E] The APM changed the rail with the PLL, i.e. it ran a full set_rate path; APM not waiting for lock is likelier than a missing config. | 10% |
| a5 | Ordering: stock calls `cal_cluster_enable` (cluster0_on) and powers the first core BEFORE cpufreq/APM touches CPUCL0, so the APM only ever tunes a live PLL. We tune first, then power. | [E] cluster0_on contains no PLL step; stock Linux brings cpu4 up only after cpufreq init of the small cluster. [G] the APM may be sequence-aware. | Our probe powered the cluster AFTER the PLL step and never retried the PLL, so a5 is untested rather than disproved (the log's "after" dump shows the PLL untouched by probe). | 15% (overlaps a1) |

Key observation: the loader does the PLL step first and the power-up step second, and never re-reads or retries the PLL after the power-up.
Whatever (a1/a5) the right cause is, "power the cluster, then poll PLL STABLE" has not been tested yet.

### (b) PSCI-started Mongoose core never runs the stub (MONGOOSE STARTED=0)

| # | Hypothesis | Evidence | Conf. |
|---|---|---|---|
| b1 | The core is powered but has no clock: CMU_CPUCL0 still selects the unlocked PLL (`mux_pll=0`, fact 0.2), and nothing moved it to the switch path / locked PLL. A53 works because its PLL is locked. | [E] mux_pll=0 selects PLL_CPUCL0 (cmucal-node.c:525); [E] STABLE=0 throughout; [E] PMU status changed to 0x10070f0f (power accepted) but no instruction retired. | 35% |
| b2 | The cluster sequencer/L2/NONCPU never came up (cl0 seq st unchanged 0x60000, NONCPU/L2 never read). The core cell is "powered" per its own status but the cluster fabric (NONCPU = ACE/CCI interface, GIC redistributor path, L2) is down, so the core is held in reset or its fetch cannot leave the cluster. | [E] st=0x60000 before and after; `cluster_state()` in stock (exynos-pmu.c:151) needs L2 & NONCPU; [G] `cpu st` 0x10070f0f differs from the running A53 boot core 0x7000f by bit 28 and bits [11:8] (meaning unknown; likely a pending/held state). | 30% |
| b3 | Memory visibility: stub writes the flag with caches off to DRAM, the boot core reads with MMU off (Device), that part is sound. But (i) the Mongoose cluster is a separate coherency island; if the loader image/stub code or the `smp_flags` line is dirty in the A53 cluster's caches (S-Boot loaded the image with D-cache on; loader never does `dc civac` on smp_entry/smp_flags), the Mongoose core fetches stale DRAM (zeros, `udf`) from `smp_entry`; or (ii) a stale dirty zero line for smp_flags in the A53 L2 is evicted after the stub's write. | [E] start.S:70-78 stub is MMU-off, caches off safe, uses `adr` (PIC, good); [E] no cache maintenance in smp_probe.c; [G] instruction fetch with MMU off is non-coherent on a different cluster unless the image is clean in DRAM. The A53 secondary starts in the SAME coherency domain, so it would not expose such a bug. | 20% |
| b4 | Entry address / PSCI args: MPIDR 0x000 is right (DT `cpu@0 reg = <0x0 0x0>`, exynos8895.dtsi:243-250; PMU offset ((aff1<<2)|aff0)*0x80 = 0x2000 for core 0, exynos-pmu.c:49-53). Entry is the loader image (0x800b4000 per start.S:6), within 4 GB, 4-byte aligned, the same address the A53 starts at. | [E] correct on every checked point. rc=0. Not a likely cause; keep unchanged. | 5% |
| b5 | Reset vector (RVBAR) for the Mongoose core is programmed by EL3 only when EL3 itself drives the cluster power sequence (there is no RVBAR write in pmucal or exynos-pmu: they never program a reset vector, the secure monitor does it from the PSCI call). Because we raise PMU power ourselves while EL3 apparently did not (A53 needs the manual PMU edge too), EL3 may have recorded the entry for a different mechanism (mailbox + warm-boot ROM) that the Mongoose cluster does not use. | [G] no source; follows from "A53 needs manual power". | 10% |
| b6 | Core starts at a different EL / needs SCR/GIC setup so the stub dies before the flag store. | [E] stub touches only daif, mpidr_el1, a store; cannot fault (MMU off). Only an alignment-irrelevant Device store. Very unlikely. | 3% |
| b7 | Stub is not valid for M2. | [E] plain ARMv8.0 A64 (msr daifset, mrs, ubfx, and, orr, adr, str, dsb, smc). Valid. | 1% |

### CCI / coherency (question 5)
[E] There is no CCI snoop/DVM enable code in the Samsung tree we have (grep over samsung-src: only VCLK_CCI/clock entries in cmucal-vclk.h:224,229,258).
[G] The EL3 monitor does "CCI slave enable for the cluster" inside CPU_ON/cluster power-up. With MMU/caches off on the secondary, none of it is
needed for the stub's flag store to reach DRAM; it matters only for b3 (stale dirty lines on our side). Treat CCI as: either EL3 handles it, or nothing we can see.

### cluster0_on[] vs the loader (question 4)
Stock cluster0_on = (1) PMU 0x3C20 bit0 := 1, wait 0x3C24 bit0; (2) 0x45C0 bit0 := 1, wait 0x45C4 bit0; (3) 0x16840000+0x1000 (DD4_DD_DA_INPUT_0) bit0 := 1;
(4) 0x2488 bit0 := 0. [E] Loader mngs_probe (smp_probe.c:96-122) does exactly these four with correct addresses and masks (log: qch/clkstop already 1/1
before the writes, so steps 1-2 are no-ops; opt 0x1001 -> 0x1000, dd0 = 1 later). cpu00_on = CLUSTER0_CPU0_CONFIGURATION 0x2000 := 0xF; DD0 := 1 (pmucal_cpu:2-5): matches.
Missing relative to stock: nothing from cluster0_on/cpu00_on. There is no later "enable" step. Things stock does elsewhere that the loader does not:
`pmucal_lpm_init` (pmucal_system:2-35) sets CLUSTERx_CPUy_OPTION bit12 := 0, the low option bits (0x3f) := 0x1F, bit28 := 1,
NONCPU_OPTION bits[13:8] := 0x1F, L2_OPTION bit17 := 0, and EMBEDDED_CMU_CPUCL0 SYS_PWR/QCH/CLKSTOP_OPEN sleep configuration. [G] these set the
power-sequencer behaviour (sleep/wake policy bits) and S-Boot may not have programmed them for the unused big cluster (cpu cfg shows bit31 = 1 default only).
Also the per-core reset-disable register 0x200C (exynos_cpu_reset_enable/disable, exynos-pmu.c:187-213: bits 12/9/8 = DISABLE_WDT_CPUPORESET/CORERESET/CPUPORESET) is never inspected.

## 2. Experiments (each one boot, each readable from the log)

Common helper: reuse `rd`, `wr`, `ticks`, `delay_us` from mngs_clk.c; `pmic_read8/pmic_write8` (pmic.h) for BUCK2.
All three are independent; do them in this order inside one loader build if you want, each gated by a boot-arg (suggested `mngsexp=1|2|3`).

### Experiment 1 (cheapest, decides a1/a5): force BUCK2 on, power the cluster FIRST, then retry the PLL and poll everything

What it tests: a1 and a5 together. Nothing is written except BUCK2 CTRL EN bits (already 01, set to 11), the PMU steps that stock does, and PLL ENABLE cycling.
New function in mngs_clk.c, called from loader.c right after `mngs_clk_dump(... "after acpm")` and BEFORE `mngs_probe` (or as part of it):

```diff
--- mngs_clk.c
+#include "pmic.h"
+#define PMU_ 0x16480000UL
+#define TOP  0x15a80000UL
+
+/* dump all registers we have not looked at yet */
+static void mngs_more(void (*kv)(const char *, uint64_t), const char *tag)
+{
+	kv(tag, 0);
+	for (unsigned c = 0; c < 2; c++) {
+		uintptr_t cl = c ? 0x16900000UL : 0x16800000UL;
+		kv(" pll_con1", rd(cl + 0x124));
+		kv(" pll_con2", rd(cl + 0x128));
+		kv(" pll_con3", rd(cl + 0x12c));
+		kv(" pll_con4", rd(cl + 0x130));
+		kv(" ctl_opt", rd(cl + 0x800));
+		kv(" shortstop", rd(cl + 0x820));
+		kv(" hchgen", rd(cl + 0x850));
+		kv(" mux_busy", rd(cl + 0x1000));
+		kv(" swu", rd(cl + 0x100));
+		kv(" noncpu cfg", rd(PMU_ + 0x2400 + c * 0x20));
+		kv(" noncpu st", rd(PMU_ + 0x2404 + c * 0x20));
+		kv(" noncpu opt", rd(PMU_ + 0x2408 + c * 0x20));
+		kv(" l2 cfg", rd(PMU_ + 0x2600 + c * 0x20));
+		kv(" l2 st", rd(PMU_ + 0x2604 + c * 0x20));
+		kv(" l2 opt", rd(PMU_ + 0x2608 + c * 0x20));
+		kv(" seq st", rd(PMU_ + 0x2484 + c * 0x20));
+		kv(" seq opt", rd(PMU_ + 0x2488 + c * 0x20));
+	}
+	kv(" cpu0 opt", rd(PMU_ + 0x2008));
+	kv(" cpu0 reset", rd(PMU_ + 0x200c));
+	kv(" top sw mux", rd(TOP + 0x1038));
+	kv(" top sw div", rd(TOP + 0x1838));
+	kv(" top sw gate", rd(TOP + 0x2040));
+}
+
+/* wait for PLL_CON0.STABLE up to ms; returns elapsed ms or -1 */
+static int pll_wait(unsigned ms)
+{
+	const uintptr_t con0 = 0x16800000UL + 0x120;
+	for (unsigned t = 0; t < ms; t++) {
+		if (rd(con0) & (1u << 29))
+			return (int)t;
+		delay_us(1000);
+	}
+	return -1;
+}
+
+void mngs_exp1(void (*kv)(const char *, uint64_t), void (*out)(const char *))
+{
+	const uintptr_t con0 = 0x16800000UL + 0x120;
+	out("MNGS EXP1\n");
+	mngs_more(kv, " before");
+	/* 1. stock cluster0_on step 4: sequencer option bit0 := 0, then watch status */
+	wr(PMU_ + 0x2488, rd(PMU_ + 0x2488) & ~1u);
+	delay_us(100000);
+	kv("seq t100ms", rd(PMU_ + 0x2484));
+	/* 2. first core power (cpu00_on), then watch NONCPU/L2/seq for 200 ms */
+	wr(PMU_ + 0x2000, rd(PMU_ + 0x2000) | 0xf);
+	for (unsigned i = 0; i < 8; i++) {
+		delay_us(25000);
+		kv(" t+25ms seq", rd(PMU_ + 0x2484));
+		kv(" noncpu", rd(PMU_ + 0x2404));
+		kv(" l2", rd(PMU_ + 0x2604));
+		kv(" cpu0", rd(PMU_ + 0x2004));
+	}
+	/* 3. BUCK2 always on (EN b7:6 = 11), then PLL ENABLE cycle */
+	int b2 = pmic_read8(0x1b);
+	kv("B2CTRL was", (uint64_t)b2);
+	pmic_write8(0x1b, (uint8_t)(b2 | 0xc0));
+	kv("B2CTRL now", (uint64_t)pmic_read8(0x1b));
+	delay_us(5000);
+	kv("con0 start", rd(con0));
+	wr(con0, rd(con0) & ~(1u << 31));
+	delay_us(200);
+	wr(con0, rd(con0) | (1u << 31));
+	kv("PLL STABLE after ms", (uint64_t)pll_wait(50));	/* -1 = 0xfff... if never */
+	kv("con0 end", rd(con0));
+	mngs_more(kv, " after");
+}
```
Notes:
no PSCI; the existing mngs_probe stays after it for the CPU_ON test (it will see an already-powered core: harmless, and it tests "PLL stable + core powered, then CPU_ON").
If the sequencer-status write order matters, A53 analogy is "option := 0, then CONFIGURATION := 0xF".

Outcomes (log lines):
- `PLL STABLE after ms=0x<n small>` and con0 bit29 set, ideally with `seq t.. = ...1`: a1/a5 confirmed (rail/sequencer). Then go to Experiment 2.
- Only the BUCK2 step changes it (STABLE came after `B2CTRL now=0xb8`, not before): pure rail gating (a1 proven, PWREN not asserted by the sequencer).
- `seq st` stays 0x60000 and noncpu/l2 stay 0 after 200 ms while cpu0 st = 0x10070f0f: the sequencer needs another trigger (lpm_init bits, b2); look at `noncpu opt`/`l2 opt` and `cpu0 opt`/`cpu0 reset` values printed by mngs_more.
  Next write would be the `pmucal_lpm_init` words for cluster 0 (see experiment 3 variant).
- STABLE still 0xfff... (never) and `B2CTRL now=0xb8`, noncpu/l2 up: a1 rejected, a2/a3/a4 remain; the con1..4, ctl_opt, shortstop, hchgen dumps decide the next step (compare with CPUCL1).

### Experiment 2: put the cluster clock on the switch path so the core has a clock even when the PLL is not locked (b1)

What it tests: b1 (core powered but clocked from an unlocked PLL). Run after Experiment 1 step 2 (cluster powered) and before CPU_ON.
Does not need PLL lock. Register values from cmucal (fact 0.2). CMU_TOP switch mux selects a shared PLL; read its current value first and only change it if the dump shows it unset.

```diff
--- mngs_clk.c
+/* route CMU_CPUCL0 cluster clock through the CMU_TOP switch path (PLL_SHARED*), not the CPUCL0 PLL */
+void mngs_switch_clk(void (*kv)(const char *, uint64_t), void (*out)(const char *))
+{
+	const uintptr_t cl = 0x16800000UL;
+	out("MNGS switch path\n");
+	kv(" top mux", rd(TOP + 0x1038));
+	kv(" top div", rd(TOP + 0x1838));
+	kv(" top gate", rd(TOP + 0x2040));
+	/* TOP switch source: DIV_PLL_SHARED1_DIV2 (sel 1) and /2 (DIVRATIO 1) as a conservative start;
+	 * only if the stock/S-Boot value is not already sane */
+	wr(TOP + 0x1038, (rd(TOP + 0x1038) & ~3u) | 1u);
+	for (unsigned i = 0; i < 1000 && (rd(TOP + 0x1038) & (1u << 16)); i++)
+		delay_us(10);
+	wr(TOP + 0x1838, (rd(TOP + 0x1838) & ~7u) | 1u);
+	for (unsigned i = 0; i < 1000 && (rd(TOP + 0x1838) & (1u << 16)); i++)
+		delay_us(10);
+	/* SWITCH_USER := switch path (MUX_SEL b4 = 1), wait BUSY b7 */
+	wr(cl + 0x100, rd(cl + 0x100) | (1u << 4));
+	for (unsigned i = 0; i < 1000 && (rd(cl + 0x100) & (1u << 7)); i++)
+		delay_us(10);
+	/* MUX_CLK_CPUCL0_PLL := 1 (switch), wait BUSY b16 */
+	wr(cl + 0x1000, (rd(cl + 0x1000) & ~1u) | 1u);
+	for (unsigned i = 0; i < 1000 && (rd(cl + 0x1000) & (1u << 16)); i++)
+		delay_us(10);
+	kv(" swu", rd(cl + 0x100));
+	kv(" mux_pll", rd(cl + 0x1000));
+}
```
Wire-up in smp_probe.c mngs_probe: call `mngs_switch_clk(kv, out);` after the `cluster on:` print and before the first `smc3(PSCI_CPU_ON...)`.
Outcomes: `MONGOOSE STARTED=0x1` and `mux_pll=0x1` -> b1 confirmed (the core only needed any clock); the PLL can be fixed afterwards. Still 0: b1 rejected, go to experiment 3.
Caveat [G]: the switch source clock might need the TOP gate (+0x2040) open or the shared PLL (PLL_SHARED1) already locked: both are in the dump (`top gate`, shared PLLs are used by CMU_TOP anyway on a running AP). If the CMU hangs on a write, the log stops at "MNGS switch path": that line is itself the result.
If the TOP mux/div were already correct, the diff writes are idempotent.

### Experiment 3: make the core's liveness and the memory visibility unambiguous (b3, b2, partly b5)

What it tests: whether the core ran at all (independent of the DRAM flag), and whether stale caches hide it. Three changes, no new hardware risk.

(a) Stub writes proof of life to three places and its MIDR/EL, and is cache-clean before CPU_ON (start.S):

```diff
--- start.S  (smp_entry)
 smp_entry:
 	msr	daifset, #0xf
 	mrs	x10, mpidr_el1
 	ubfx	x11, x10, #8, #1
 	and	x12, x10, #3
 	orr	x12, x12, x11, lsl #2
 	adr	x13, smp_flags
+	mrs	x15, midr_el1
+	str	x15, [x13, #8 * 8]		/* smp_flags[8] = MIDR (needs .space 128) */
+	mrs	x15, currentel
+	str	x15, [x13, #9 * 8]
 	mov	x14, #1
 	str	x14, [x13, x12, lsl #3]
 	dsb	sy
+	dc	civac, x13			/* push flag line out even if the core runs cached */
+	dsb	sy
-	movz	x0, #0x8400, lsl #16	/* PSCI CPU_OFF 0x84000002 */
-	movk	x0, #0x0002
-	smc	#0
-6:	wfe
+6:	wfe					/* leave the core spinning: PMU status then shows it running/WFI */
 	b	6b
 ...
-	.space	64
+	.space	128
```
(Dropping CPU_OFF here is deliberate: a core that turned itself off again via EL3 hides its state in the PMU; staying in `wfe` keeps `cpu st` meaningful.)

(b) Loader side (smp_probe.c, mngs_probe): clean+invalidate the code and flag lines before CPU_ON and invalidate the flag line when polling.

```diff
--- smp_probe.c
+static inline void dc_civac_range(const void *p, unsigned len)
+{
+	uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + len;
+	for (; a < e; a += 64)
+		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
+	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb" ::: "memory");
+}
 ...
 	smp_flags[0] = 0;
+	dc_civac_range(smp_entry, 4096);	/* stub code (and following page) */
+	dc_civac_range((const void *)smp_flags, 128);
 	wr(DD(0), rd(DD(0)) & ~1u);
```
and in `wait_flag`, `__asm__ volatile("dc ivac, %0" :: "r"(&smp_flags[slot]) : "memory");` before each read (MMU is off, so this is only defence against stale lines).
After the wait print: `kv("MIDR", smp_flags[8]); kv("EL", smp_flags[9]);` and then the PMU status of core 0 and `noncpu/l2/seq` again (mngs_more).

(c) Control: run exactly the same stub change for the A53 target 0x101 in `smp_probe()` first (the cluster that is known to start). If `started=1` with MIDR 0x410fd034 the stub/cache change is sound; any Mongoose result is then attributable to the cluster.

Outcomes:
- `MIDR=0x...` for Mongoose (impl 0x53) appears in the log: the core ran; earlier failure was visibility (b3) or the CPU_OFF cleared evidence. Done with bring-up of PSCI.
- Flag/MIDR still 0 but `cpu0 st` changes to a WFI/standby-looking value vs 0x10070f0f: the core runs but its stores never reach DRAM (b3 stronger, or no ACE path: NONCPU/L2 down).
- Nothing changes anywhere: the core is not released (b1/b2/b5); combine with experiment 1/2 results.

### Optional, only if experiment 1 shows the sequencer never wakes (stock lpm_init policy words for cluster 0)
Apply, once, the cluster-0 subset of `pmucal_lpm_init` (pmucal_system:2-3, 5, 17-31) before `cluster0_on`:
```c
wr(PMU+0x2608, rd(PMU+0x2608) & ~(1u<<17));            /* CLUSTER0_L2_OPTION bit17 := 0 */
for (c=0;c<4;c++) { wr(PMU+0x2008+c*0x80, rd(..) & ~(1u<<12)); }   /* CPUn_OPTION bit12 := 0 */
wr(PMU+0x2408, (rd(PMU+0x2408) & ~(0x3fu<<8)) | (0x1fu<<8));      /* NONCPU_OPTION [13:8] := 0x1F */
for (c=0;c<4;c++) { wr(PMU+0x2008+c*0x80, (rd(..)&~0x3fu)|0x1fu); wr(.., rd(..)|(1u<<28)); }
```
Log the old values first (mngs_more already prints `cpu0 opt`, `noncpu opt`, `l2 opt`); only apply if they differ from these targets. [G] low-medium payoff.

## 3. What to expect, and the likeliest overall story [G]
S-Boot never brings up the big cluster, so its sequencer is parked (option bit0 = 1), BUCK2 is PWREN-gated and off, PLL_CON0 stays at the reset
value ENABLE=1 STABLE=0, `mux_pll=0` leaves the cluster on that dead PLL. Stock EL3 powers the cluster before the first Mongoose core, and
cpufreq/APM only touch a powered domain. Powering the cluster first (Experiment 1), then retrying the PLL (or temporarily using the switch path, Experiment 2),
then PSCI CPU_ON with clean caches and a core that is observable (Experiment 3) is the most direct way to confirm or kill every item above in at most three boots.

## 4. Quick answers to the specific questions
1. PLL registers: LOCKTIME 0x000, PLL_CON0 0x120 (ENABLE b31, STABLE b29, M[25:16], P[13:8], S[2:0]), PLL_CON1/3/4 +0x124/+0x12c/+0x130 unnamed, PLL_CON2 +0x128 bits [29:28] touched in sleep paths; mux select 0x1000 b0 (0=PLL, 1=switch); no separate BYPASS/LOCK_EN field; PLL type 1050X. The "bypass" is the mux; Samsung's BYPASS->ENABLE->lock->switch dance is cal-if code not in the tree and is run by the APM.
2. Power: the "cluster on" indicator Samsung reads is 0x2484 bit0 (`cluster0_status`); it is 0 before and after. NONCPU 0x2404 / L2 0x2604 status are what stock `cluster_state` uses; the loader never prints them in this boot. DVFS first then CPU_ON is the order we did; stock is the reverse (untested here).
3. PSCI: MPIDR 0x000 is correct for cpu4 (DT reg <0x0 0x0>); the entry is the loader image, same as A53; PMU CONFIGURATION for core 0 is 0x2000; no RVBAR write exists in pmucal or exynos-pmu (EL3 does that); stub is valid ARMv8.0.
4. cluster0_on: four writes, all matched; no later enable step; missing only the stock lpm_init policy words and the per-core reset register 0x200C inspection.
5. Cache/coherency: no CCI code in the tree; flag visibility defended by Experiment 3.
