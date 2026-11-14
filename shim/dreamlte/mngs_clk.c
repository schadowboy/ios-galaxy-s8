/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only dump of the Exynos 8895 Mongoose (CPUCL0) clock + PMU state.
 * Register map from Samsung's cal-if (exynos8895 cmucal-sfr.c / pmucal_cpu_exynos8895.h):
 *   CMU_CPUCL0 base 0x16800000: LOCKTIME +0x000, CON0_MUX_CLKCMU_CPUCL0_SWITCH_USER +0x100,
 *     PLL_CON0_PLL_CPUCL0 +0x120 (ENABLE b31, STABLE b29, M[25:16], P[13:8], S[2:0]),
 *     MUX_CLK_CPUCL0_PLL +0x1000 (SELECT b0, BUSY b16), DIV_CPU +0x180c, DIV_CMUREF +0x1808,
 *     DIV_PCLK +0x1810, DIV_PCLKDBG +0x1814.
 *   CMU_CPUCL1 base 0x16900000 (A53) is dumped too for comparison.
 *   PMU 0x16480000: CPUSEQ status/option cl0 0x2484/0x2488, cl1 0x24A4/0x24A8,
 *     QCH 0x3C20/0x3C24, CLKSTOP 0x45C0/0x45C4, CPU cfg/stat 0x2000+n*0x80 (+4).
 * mngs_clk_dump() is read only. mux_pll b0: 0 = PLL_CPUCL0 (the PLL itself), 1 = SWITCH_USER
 * path (cmucal-node.c:525); sw_user b4: 0 = 26 MHz OSC, 1 = CMU_TOP CLKCMU_CPUCL0_SWITCH.
 * mngs_exp1 / mngs_switch_clk / mngs_exp_restore (boot-arg mngsexp=1|2) do write registers.
 */
#include <stdint.h>
#include "pmic.h"
#include "mngs_clk.h"

#define CL0	0x16800000UL
#define CL1	0x16900000UL
#define PMU	0x16480000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }

static void dump_cmu(void (*kv)(const char *, uint64_t), uintptr_t b)
{
	kv(" locktime", rd(b + 0x000));
	kv(" sw_user", rd(b + 0x100));
	kv(" pll_con0", rd(b + 0x120));
	kv(" mux_pll", rd(b + 0x1000));
	kv(" div_cmuref", rd(b + 0x1808));
	kv(" div_cpu", rd(b + 0x180c));
	kv(" div_pclk", rd(b + 0x1810));
}

static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void delay_us(unsigned us)
{
	uint64_t end = ticks() + 26ULL * us;	/* 26 MHz */
	while (ticks() < end)
		;
}

/*
 * Experiment: Mongoose PLL_CON0 reads 0x81ec0312 at handoff (ENABLE=1, STABLE=0,
 * M=492 P=3 S=2 -> ~1066 MHz; mux_pll +0x1000 b0 = 0 selects the PLL ITSELF, per
 * cmucal-node.c parents {PLL_CPUCL0, SWITCH_USER}; it is NOT the oscillator, so the
 * cluster is clocked from this unlocked PLL): enabled but never locked. Cycle ENABLE
 * (b31) off then on and poll STABLE (b29) with a timeout; log every step.
 * Writes only PLL_CON0 of CMU_CPUCL0. Does nothing if the PLL is already stable.
 */
void mngs_pll_try(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	const uintptr_t con0 = CL0 + 0x120;
	out("MNGS PLL try\n");
	kv("con0 start", rd(con0));
	if (rd(con0) & (1u << 29)) {
		out("already stable\n");
		return;
	}
	wr(con0, rd(con0) & ~(1u << 31));
	delay_us(100);
	kv("con0 disabled", rd(con0));
	wr(con0, rd(con0) | (1u << 31));
	uint64_t end = ticks() + 26ULL * 5000;	/* 5 ms */
	while (ticks() < end && !(rd(con0) & (1u << 29)))
		;
	kv("con0 enabled", rd(con0));
	kv("PLL STABLE", (rd(con0) >> 29) & 1);
	out("\n");
}

/* ---- boot-arg mngsexp=1|2 experiments (log prefix "MEXP:") ---- */
#define TOP		0x15a80000UL
#define MX_B2CTRL	0x1b
#define MX_B2OUT	0x1c
#define MX_DD(n)	(0x16840000UL + (n) * 0x400)	/* DDn_DD_DA_INPUT_0 */

static int b2_saved = -1;	/* original B2CTRL if exp1 changed it, else -1 */

static void fl(void (*flush)(void))
{
	if (flush)
		flush();
}

/* Poll until (reg & bit) == want (0 or bit); every wait is bounded. 1 = reached. */
static int wait_bit(uintptr_t a, uint32_t bit, uint32_t want, unsigned ms)
{
	for (unsigned i = 0; i < ms * 100; i++) {
		if ((rd(a) & bit) == want)
			return 1;
		delay_us(10);
	}
	return 0;
}

static void exp_pmu(void (*kv)(const char *, uint64_t), const char *tag)
{
	kv(tag, 0);
	kv("MEXP: noncpu st 0x2404", rd(PMU + 0x2404));
	kv("MEXP: l2 st 0x2604", rd(PMU + 0x2604));
	kv("MEXP: seq st 0x2484", rd(PMU + 0x2484));
	kv("MEXP: seq opt 0x2488", rd(PMU + 0x2488));
	kv("MEXP: core0 reset 0x200c", rd(PMU + 0x200c));
	kv("MEXP: core0 cfg", rd(PMU + 0x2000));
	kv("MEXP: core0 st", rd(PMU + 0x2004));
}

/*
 * Experiment 1: cluster power sequence FIRST, then rail + PLL retry.
 * Writes: PMU qch/clkstop cfg (already 1), DD4/DD0 bit0, seq option bit0 := 0, core0 cfg := 0xf,
 * PMIC B2CTRL |= 0xc0 (only if B2OUT <= 0x3c = 675 mV; restored by mngs_exp_restore()),
 * CMU_CPUCL0 PLL_CON0 ENABLE cycle. No PSCI.
 */
void mngs_exp1(void (*kv)(const char *, uint64_t), void (*out)(const char *),
	       void (*flush)(void))
{
	const uintptr_t con0 = CL0 + 0x120;
	out("MEXP: exp1 begin\n");
	exp_pmu(kv, "MEXP: pre");
	int b2c = pmic_read8(MX_B2CTRL), b2o = pmic_read8(MX_B2OUT);
	kv("MEXP: B2CTRL", b2c < 0 ? 0xdead : (uint64_t)b2c);
	kv("MEXP: B2OUT", b2o < 0 ? 0xdead : (uint64_t)b2o);
	kv("MEXP: con0", rd(con0));
	fl(flush);
	/* stock cluster0_on */
	wr(PMU + 0x3c20, rd(PMU + 0x3c20) | 1);
	if (!wait_bit(PMU + 0x3c24, 1, 1, 50)) {
		out("MEXP: QCH status stuck, exp1 stops\n");
		fl(flush);
		return;
	}
	wr(PMU + 0x45c0, rd(PMU + 0x45c0) | 1);
	if (!wait_bit(PMU + 0x45c4, 1, 1, 50)) {
		out("MEXP: CLKSTOP status stuck, exp1 stops\n");
		fl(flush);
		return;
	}
	wr(MX_DD(4), rd(MX_DD(4)) | 1);
	wr(PMU + 0x2488, rd(PMU + 0x2488) & ~1u);
	exp_pmu(kv, "MEXP: after cluster0_on");
	out("MEXP: powering core0 (cpu00_on)\n");
	fl(flush);
	/* cpu00_on */
	wr(PMU + 0x2000, rd(PMU + 0x2000) | 0xf);
	(void)wait_bit(PMU + 0x2004, 0xf, 0xf, 100);
	wr(MX_DD(0), rd(MX_DD(0)) | 1);
	for (unsigned i = 1; i <= 8; i++) {
		delay_us(25000);
		kv("MEXP: t+ms", i * 25);
		kv("MEXP: seq st", rd(PMU + 0x2484));
		kv("MEXP: noncpu st", rd(PMU + 0x2404));
		kv("MEXP: l2 st", rd(PMU + 0x2604));
		kv("MEXP: core0 st", rd(PMU + 0x2004));
	}
	fl(flush);
	/* rail: always on, only at/below the 675 mV setting */
	if (b2c < 0 || b2o < 0 || b2o > 0x3c) {
		out("MEXP: B2 read failed or B2OUT > 0x3c, rail left alone\n");
	} else {
		b2_saved = b2c;
		kv("MEXP: B2CTRL before", (uint64_t)b2c);
		(void)pmic_write8(MX_B2CTRL, (uint8_t)(b2c | 0xc0));
		int n = pmic_read8(MX_B2CTRL);
		kv("MEXP: B2CTRL after", n < 0 ? 0xdead : (uint64_t)n);
		delay_us(20000);
		n = pmic_read8(MX_B2OUT);
		kv("MEXP: B2OUT after", n < 0 ? 0xdead : (uint64_t)n);
	}
	fl(flush);
	/* PLL ENABLE cycle, M/P/S kept */
	kv("MEXP: con0 start", rd(con0));
	wr(con0, rd(con0) & ~(1u << 31));
	delay_us(1000);
	kv("MEXP: con0 disabled", rd(con0));
	wr(con0, rd(con0) | (1u << 31));
	int stable = -1;
	for (unsigned i = 1; i <= 50; i++) {
		delay_us(1000);
		uint32_t c = rd(con0);
		kv("MEXP: con0 t+ms", ((uint64_t)i << 32) | c);
		if (c & (1u << 29)) {
			stable = (int)i;
			break;
		}
	}
	kv("MEXP: PLL STABLE at ms (-1 never)", (uint64_t)(int64_t)stable);
	exp_pmu(kv, "MEXP: end exp1");
	fl(flush);
	mngs_clk_dump(kv, out, "mexp1");
	fl(flush);
}

/* Undo what mngs_exp1 changed in the PMIC; call right before the loader jumps to XNU. */
void mngs_exp_restore(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		      void (*flush)(void))
{
	if (b2_saved < 0)
		return;
	(void)pmic_write8(MX_B2CTRL, (uint8_t)b2_saved);
	int n = pmic_read8(MX_B2CTRL);
	kv("MEXP: B2CTRL restored to", (uint64_t)b2_saved);
	kv("MEXP: B2CTRL readback", n < 0 ? 0xdead : (uint64_t)n);
	b2_saved = -1;
	out("MEXP: restore done\n");
	fl(flush);
}

static void sw_regs(void (*kv)(const char *, uint64_t), const char *tag)
{
	kv(tag, 0);
	kv("MEXP: top sw mux 0x1038", rd(TOP + 0x1038));
	kv("MEXP: top sw div 0x1838", rd(TOP + 0x1838));
	kv("MEXP: top sw gate 0x2040", rd(TOP + 0x2040));
	kv("MEXP: cl0 sw_user 0x100", rd(CL0 + 0x100));
	kv("MEXP: cl0 mux_pll 0x1000", rd(CL0 + 0x1000));
	kv("MEXP: cl0 con0", rd(CL0 + 0x120));
}

/*
 * Experiment 2: move the CPUCL0 cluster clock off the (unlocked) PLL onto the CMU_TOP switch
 * path. CMU_TOP: MUX_CLKCMU_CPUCL0_SWITCH +0x1038 SELECT[1:0] BUSY b16, DIV +0x1838 [2:0]
 * BUSY b16, GATE +0x2040 (CG_VAL b21, MANUAL b20); CMU_CPUCL0: SWITCH_USER +0x100 MUX_SEL b4
 * BUSY b7, mux_pll +0x1000 SELECT b0 BUSY b16 (cmucal-sfr.c:3698-3706, 4314-4330).
 * Switch lut for CPUCL0 (cmucal-vclk.c:2486) is {1066000 kHz, mux 0, div 0}: mux 0. We use div 1
 * (half, ~533 MHz) as a conservative start at the low rail voltage. Returns 0 ok, -1 on timeout.
 */
int mngs_switch_clk(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		    void (*flush)(void))
{
	out("MEXP: switch clk begin\n");
	sw_regs(kv, "MEXP: switch before");
	fl(flush);
	uint32_t g = rd(TOP + 0x2040);
	if ((g & (1u << 20)) && !(g & (1u << 21))) {	/* manual gate closed */
		wr(TOP + 0x2040, g | (1u << 21));
		kv("MEXP: top gate opened", rd(TOP + 0x2040));
	}
	wr(TOP + 0x1838, (rd(TOP + 0x1838) & ~7u) | 1u);
	if (!wait_bit(TOP + 0x1838, 1u << 16, 0, 10)) {
		out("MEXP: TOP div busy timeout, stop\n");
		fl(flush);
		return -1;
	}
	wr(TOP + 0x1038, rd(TOP + 0x1038) & ~3u);
	if (!wait_bit(TOP + 0x1038, 1u << 16, 0, 10)) {
		out("MEXP: TOP mux busy timeout, stop\n");
		fl(flush);
		return -1;
	}
	sw_regs(kv, "MEXP: switch top set");
	fl(flush);
	wr(CL0 + 0x100, rd(CL0 + 0x100) | (1u << 4));
	if (!wait_bit(CL0 + 0x100, 1u << 7, 0, 10)) {
		out("MEXP: SWITCH_USER busy timeout, stop\n");
		fl(flush);
		return -1;
	}
	fl(flush);
	wr(CL0 + 0x1000, (rd(CL0 + 0x1000) & ~1u) | 1u);
	if (!wait_bit(CL0 + 0x1000, 1u << 16, 0, 10)) {
		out("MEXP: mux_pll busy timeout, stop\n");
		fl(flush);
		return -1;
	}
	sw_regs(kv, "MEXP: switch after");
	out("MEXP: switch clk done\n");
	fl(flush);
	return 0;
}

void mngs_clk_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		   const char *tag)
{
	out("MNGS clk dump ");
	out(tag);
	out("\n");
	out("CMU_CPUCL0 (Mongoose)\n");
	dump_cmu(kv, CL0);
	out("\nCMU_CPUCL1 (A53)\n");
	dump_cmu(kv, CL1);
	out("\nPMU\n");
	kv(" cl0 seq st", rd(PMU + 0x2484));
	kv(" cl0 seq opt", rd(PMU + 0x2488));
	kv(" cl1 seq st", rd(PMU + 0x24a4));
	kv(" cl1 seq opt", rd(PMU + 0x24a8));
	kv(" qch cfg", rd(PMU + 0x3c20));
	kv(" qch st", rd(PMU + 0x3c24));
	kv(" clkstop cfg", rd(PMU + 0x45c0));
	kv(" clkstop st", rd(PMU + 0x45c4));
	for (unsigned c = 0; c < 8; c++) {
		uintptr_t cfg = PMU + 0x2000 + c * 0x80;
		kv(" cpu cfg", rd(cfg));
		kv(" cpu st", rd(cfg + 4));
	}
	out("\n");
}
