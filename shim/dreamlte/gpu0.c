/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S8 (Exynos 8895) Mali-G71 Stage 0 probe, MMU off, physical addresses.
 * Written from register facts in samsung-src (pmucal_local_exynos8895.h:357,
 * exynos8895.dtsi:777) and gpu-plan.md. Every risky step is preceded by a log
 * line and a flush to USERDATA; a failed step skips the rest.
 */
#include <stdint.h>
#include "gpu0.h"
#include "pmic.h"

#define PMU		0x16480000UL
#define CMU_G3D		0x13800000UL
#define G3D		0x13900000UL
#define TIMER_HZ	26000000UL

static void (*kv)(const char *, uint64_t);
static void (*out)(const char *);
static void (*flush)(void);

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void say(const char *s)
{
	out("GPU0: ");
	out(s);
	out("\n");
}

static void val(const char *k, uint64_t v)
{
	out("GPU0: ");
	kv(k, v);
	out("\n");
}

/* poll (rd(a) & mask) == want for up to ms milliseconds; returns 1 on success */
static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	do {
		if ((rd(a) & mask) == want)
			return 1;
	} while (ticks() < end);
	return (rd(a) & mask) == want;
}

static void step(const char *s)
{
	say(s);
	flush();
}

void gpu0_probe(void (*kv_fn)(const char *, uint64_t), void (*out_fn)(const char *), void (*flush_fn)(void))
{
	kv = kv_fn;
	out = out_fn;
	flush = flush_fn;

	say("start (gpuprobe=1)");
	/* pre-arm: any later reset lands in TWRP (same recipe as s8_reset_recovery) */
	wr(PMU + 0x808, 0x12345678U);	/* INFORM2 */
	wr(PMU + 0x80c, 0x12345674U);	/* INFORM3 recovery */
	say("recovery INFORM2/3 pre-armed");
	say("watchdog not armed (no WTCON/PMU watchdog-mask evidence in samsung-src)");

	/* step 1: read-only dumps. PMU and PMIC first; CMU_G3D reads are the first
	 * risky ones (flushed before). */
	step("step 1 about to read PMU G3D regs");
	val("PMU+40E0 G3D_CONFIG", rd(PMU + 0x40E0));
	val("PMU+40E4 G3D_STATUS", rd(PMU + 0x40E4));
	val("PMU+40E8 G3D_OPTION", rd(PMU + 0x40E8));
	step("step 1 about to read PMIC BUCK6");
	val("B6CTRL 0x23", (uint64_t)(int64_t)pmic_read8(0x23));
	val("B6OUT1 0x24", (uint64_t)(int64_t)pmic_read8(0x24));
	val("B6OUT2 0x25", (uint64_t)(int64_t)pmic_read8(0x25));
	step("step 1 about to read CMU_G3D");
#define C(n, o) do { say("reading " n); flush(); val(n, rd(CMU_G3D + (o))); } while (0)
	C("CMU_G3D+100", 0x100); C("CMU_G3D+120", 0x120); C("CMU_G3D+140", 0x140);
	C("CMU_G3D+148", 0x148); C("CMU_G3D+1000", 0x1000); C("CMU_G3D+1804", 0x1804);
#undef C
	step("step 1 done");

	/* step 2: rail to 800 mV (300 mV + 6.25 mV * 0x50), never lower it */
	step("step 2 about to raise BUCK6 to 800 mV (0x50)");
	int cur = pmic_read8(0x24);
	if (cur < 0) {
		say("step 2 failed: B6OUT1 read timeout, skipping rest");
		goto done;
	}
	if (cur < 0x50) {
		int rc = pmic_write8(0x24, 0x50);
		val("step 2 write rc", (uint64_t)(int64_t)rc);
	} else {
		say("step 2 B6OUT1 already >= 800 mV, left alone");
	}
	int back = pmic_read8(0x24);
	val("step 2 B6OUT1 readback", (uint64_t)(int64_t)back);
	if (back < 0x50) {
		say("step 2 failed: rail not at 800 mV, skipping rest");
		goto done;
	}
	step("step 2 done");

	/* step 3: replay stock g3d_on (pmucal_local_exynos8895.h:357-363) */
	if ((rd(PMU + 0x40E4) & 0xf) == 0xf) {
		say("step 3 domain already ON, g3d_on replay skipped");
	} else {
		step("step 3 about to replay g3d_on");
		wr(CMU_G3D + 0x148, 0x30000003U);
		wr(CMU_G3D + 0x140, rd(CMU_G3D + 0x140) | (1u << 4));
		wr(CMU_G3D + 0x100, rd(CMU_G3D + 0x100) | (1u << 4));
		step("step 3 about to write PMU G3D config = 0xF");
		wr(PMU + 0x40E0, (rd(PMU + 0x40E0) & ~0xfu) | 0xfu);
		int ok = poll(PMU + 0x40E4, 0xf, 0xf, 100);
		val("step 3 STATUS after poll", rd(PMU + 0x40E4));
		val("step 3 CMU+148", rd(CMU_G3D + 0x148));
		val("step 3 CMU+140", rd(CMU_G3D + 0x140));
		val("step 3 CMU+100", rd(CMU_G3D + 0x100));
		if (!ok) {
			say("step 3 failed: domain did not report ON, GPU registers not touched");
			goto done;
		}
	}
	step("step 3 done");

	/* step 4: identity reads, only with the domain ON */
	if ((rd(PMU + 0x40E4) & 0xf) != 0xf) {
		say("step 4 failed: domain not ON");
		goto done;
	}
	step("step 4 about to read GPU_ID");
	uint32_t id = rd(G3D + 0x000);
	val("GPU_ID", id);
	flush();
#define R(n, o) val(n, rd(G3D + (o)))
	R("L2_FEATURES", 0x004); R("CORE_FEATURES", 0x008); R("TILER_FEATURES", 0x00c);
	R("MEM_FEATURES", 0x010); R("MMU_FEATURES", 0x014); R("AS_PRESENT", 0x018);
	R("JS_PRESENT", 0x01c); R("GPU_IRQ_RAWSTAT", 0x020); R("GPU_STATUS", 0x034);
	flush();
	R("THREAD_MAX_THREADS", 0x0a0); R("THREAD_MAX_WORKGROUP", 0x0a4);
	R("THREAD_MAX_BARRIER", 0x0a8); R("THREAD_FEATURES", 0x0ac);
	R("SHADER_PRESENT_LO", 0x100); R("SHADER_PRESENT_HI", 0x104);
	R("TILER_PRESENT", 0x110); R("L2_PRESENT", 0x120);
	R("SHADER_READY", 0x140); R("TILER_READY", 0x150); R("L2_READY", 0x160);
	R("COHERENCY_FEATURES", 0x300);
#undef R
	uint32_t shp = rd(G3D + 0x100), tilp = rd(G3D + 0x110), l2p = rd(G3D + 0x120);
	step("step 4 done");

	/* step 5: soft reset, then L2, shaders, tiler (S9+ hakios.mali.c:459-461 offsets) */
	step("step 5 about to soft reset");
	wr(G3D + 0x024, 0xffffffffU);	/* GPU_IRQ_CLEAR */
	wr(G3D + 0x030, 1);		/* GPU_COMMAND = SOFT_RESET */
	if (!poll(G3D + 0x020, 1u << 8, 1u << 8, 100)) {
		val("step 5 failed: no RESET_COMPLETED, RAWSTAT", rd(G3D + 0x020));
		goto done;
	}
	wr(G3D + 0x024, 0xffffffffU);
	say("step 5 reset completed");
	step("step 5 about to power L2");
	wr(G3D + 0x1a0, l2p);		/* L2_PWRON */
	if (!poll(G3D + 0x160, l2p, l2p, 100)) {
		val("step 5 failed: L2_READY", rd(G3D + 0x160));
		goto done;
	}
	step("step 5 about to power shaders + tiler");
	wr(G3D + 0x180, shp);		/* SHADER_PWRON */
	int s_ok = poll(G3D + 0x140, shp, shp, 100);
	wr(G3D + 0x190, tilp);		/* TILER_PWRON */
	int t_ok = poll(G3D + 0x150, tilp, tilp, 100);
	val("step 5 SHADER_READY", rd(G3D + 0x140));
	val("step 5 TILER_READY", rd(G3D + 0x150));
	val("step 5 L2_READY", rd(G3D + 0x160));
	val("step 5 GPU_STATUS", rd(G3D + 0x034));
	val("step 5 shader ok", (uint64_t)s_ok);
	val("step 5 tiler ok", (uint64_t)t_ok);
	step("step 5 done");

	/* step 6: power back down (PWROFF offsets are [G]: 0x1c0/0x1d0/0x1e0) */
	step("step 6 about to power cores off");
	wr(G3D + 0x1c0, shp);
	wr(G3D + 0x1d0, tilp);
	(void)poll(G3D + 0x140, shp, 0, 100);
	(void)poll(G3D + 0x150, tilp, 0, 100);
	wr(G3D + 0x1e0, l2p);
	(void)poll(G3D + 0x160, l2p, 0, 100);
	val("step 6 SHADER_READY", rd(G3D + 0x140));
	val("step 6 TILER_READY", rd(G3D + 0x150));
	val("step 6 L2_READY", rd(G3D + 0x160));
	say("step 6 done");
done:
	say("probe ended (any 'failed' line above = abort point)");
	flush();
}
