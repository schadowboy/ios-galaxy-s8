/* SPDX-License-Identifier: GPL-2.0-only
 * Galaxy S8 secondary-CPU probe, run by the loader before XNU (MMU off).
 *
 * s8-probe v3-v6: PSCI CPU_ON returns 0 (twice, never ALREADY_ON) yet no
 * secondary ever ran our entry. Samsung's 4.4 kernel powers cores through
 * the PMU itself (drivers/soc/samsung/exynos-pmu.c): CPUn_CONFIGURATION at
 * PMU + 0x2000 + ((Aff1 << 2 | Aff0) * 0x80), LOCAL_PWR_CFG = 0xF, status
 * at +4; per-cluster CPUSEQ_OPTION / NONCPU / L2 status at 0x2488 / 0x2404 /
 * 0x2604 + cluster * 0x20. This probe tries CPU_ON alone, then PMU power-up
 * followed by CPU_ON, for one A53 (0x101) and one Mongoose (0x000) core.
 * A started core marks its slot and turns itself off again (CPU_OFF).
 */
#include <stdint.h>
#include "smp_probe.h"
#include "mngs_clk.h"

#define PMU		0x16480000UL
#define CPU_CFG(m)	(PMU + 0x2000 + ((((m) >> 8) & 0xf) << 2 | ((m) & 0xf)) * 0x80)
#define CPU_STAT(m)	(CPU_CFG(m) + 4)
#define CL_CPUSEQ(c)	(PMU + 0x2488 + (c) * 0x20)
#define CL_NONCPU(c)	(PMU + 0x2404 + (c) * 0x20)
#define CL_L2(c)	(PMU + 0x2604 + (c) * 0x20)

#define PSCI_CPU_ON	0xc4000003UL
#define TIMER_HZ	26000000UL

extern char smp_entry[];
extern char smp_entry3[];
extern volatile uint64_t smp_flags[16];	/* start.S: set by a started core; [8..11] mngsexp=3 data */

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static int64_t smc3(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
			 "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

static int wait_flag(unsigned slot, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if (smp_flags[slot])
			return 1;
	return 0;
}

/* mngsexp=3: clean+invalidate a range to the point of coherency, then drop the I-cache */
static void dc_civac_range(const void *p, unsigned len)
{
	uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + len;
	for (; a < e; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy; ic ialluis; dsb sy; isb" ::: "memory");
}

/* like wait_flag, but invalidates the flag line before every read */
static int wait_flag_inv(unsigned slot, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end) {
		__asm__ volatile("dc ivac, %0; dsb sy" :: "r"(&smp_flags[slot]) : "memory");
		if (smp_flags[slot])
			return 1;
	}
	return 0;
}

static void regs(void (*kv)(const char *, uint64_t), uint64_t m)
{
	unsigned c = (m >> 8) & 0xf;
	kv(" cfg", rd(CPU_CFG(m)));
	kv("stat", rd(CPU_STAT(m)));
	kv("cpuseq", rd(CL_CPUSEQ(c)));
	kv("noncpu", rd(CL_NONCPU(c)));
	kv("l2", rd(CL_L2(c)));
}

/* Poll a PMU bit with a timeout; 1 = reached. */
static int wait_bit(uintptr_t a, uint32_t bit, unsigned ms)
{
	uint64_t end = ticks() + TIMER_HZ / 1000 * ms;
	while (ticks() < end)
		if (rd(a) & bit)
			return 1;
	return 0;
}

#define DD(n)		(0x16840000UL + (n) * 0x400)	/* DDn_DD_DA_INPUT_0 */

/*
 * Mongoose (CLUSTER0, MPIDR 0x0-0x3) bring-up test on core 0x000, in the
 * order of Samsung's cal-if/exynos8895/pmucal_cpu_exynos8895.h:
 * cluster0_on = CPU_QCH 0x3C20 := 1, wait 0x3C24; CLKSTOP 0x45C0 := 1,
 * wait 0x45C4; DD4 := 1; CPUSEQUENCER_OPTION 0x2488 bit 0 := 0.
 * cpu00_on = CONFIGURATION 0x2000 := 0xF; DD0 := 1. Then PSCI CPU_ON.
 * Every wait has a timeout and stops the test, so a stuck step is logged.
 */
void mngs_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	mngs_probe_x(kv, out, 0, 0);
}

static void fl(void (*flush)(void))
{
	if (flush)
		flush();
}

/*
 * wifiprobe=1 (analysis/wifi-w0-sequence.md): READ-ONLY dump of what the Wi-Fi PCIe link (pcie0) looks like when S-Boot hands
 * over: the gpj1 pins (WLAN_EN gpj1-3, PERST gpj1-2, CLKREQ gpj1-0), the PMU wake mask and PHY control, the FSYS1 sysreg, and
 * the PCIe controller blocks (ELBI, PCS, PHY, DBI). Nothing is written. Every block is announced and the log flushed BEFORE it
 * is read, so a block that is unclocked and hangs the bus is named by the last line of the loader log.
 */
/* every register read below announces its address and flushes the log first: the last line names a register that hangs */
static uint32_t wifi_traced_rd(void (*kv)(const char *, uint64_t), void (*flush)(void), uintptr_t a)
{
	uint32_t v;

	kv("WIFI: about to read", a);
	fl(flush);
	v = rd(a);
	kv("WIFI:   value", v);
	return v;
}

#define rd(a) wifi_traced_rd(kv, flush, (a))

void wifi_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
void wifi_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	static const uintptr_t GPJ = 0x11430000UL;      /* FSYS1 pin controller, gpj1 = first bank */
	static const uintptr_t SYSREG = 0x11421044UL;
	static const uintptr_t ELBI = 0x116A0000UL, PCS = 0x116C0000UL, PHY = 0x116D0000UL, DBI = 0x11700000UL;
	static const unsigned elbi_off[] = { 0x02c, 0x038, 0x074, 0x088, 0x0f4, 0x1b8, 0x1d0, 0x274, 0x288, 0x28c, 0x290, 0x2c8 };
	static const unsigned pcs_off[] = { 0x00c, 0x0ec, 0x0f8, 0x100, 0x104 };
	static const unsigned phy_off[] = { 0x000, 0x004, 0x008, 0x080, 0x15c, 0x170, 0x29c };

	out("WIFI probe: read-only dump of the pcie0 (Wi-Fi) state\n");
	fl(flush);

	out("WIFI: gpj1 (FSYS1 pin controller 0x11430000)\n");
	fl(flush);
	kv("WIFI: GPJ1CON", rd(GPJ + 0x00));
	kv("WIFI: GPJ1DAT", rd(GPJ + 0x04));
	kv("WIFI: GPJ1PUD", rd(GPJ + 0x08));
	kv("WIFI: GPJ1DRV", rd(GPJ + 0x0c));
	kv("WIFI: GPJ1CONPDN", rd(GPJ + 0x10));
	out("\n");

	out("WIFI: PMU\n");
	fl(flush);
	kv("WIFI: PMU WAKEUP_MASK 0x610", rd(PMU + 0x610));
	kv("WIFI: PMU PCIE_PHY_CONTROL 0x71c", rd(PMU + 0x71c));
	/* PMU 0x720 (PCIE_PHY_CONTROL of channel 1) HANGS the bus when read (v57): not read */
	out("\n");

	out("WIFI: FSYS1 sysreg 0x11421044\n");
	fl(flush);
	kv("WIFI: sysreg +0x00", rd(SYSREG + 0x00));
	kv("WIFI: sysreg +0x0c", rd(SYSREG + 0x0c));
	out("\n");

	out("WIFI: ELBI 0x116A0000\n");
	fl(flush);
	for (unsigned i = 0; i < sizeof(elbi_off) / sizeof(elbi_off[0]); i++) {
		kv("WIFI: ELBI off", elbi_off[i]);
		kv("WIFI: ELBI val", rd(ELBI + elbi_off[i]));
	}
	out("\n");

	out("WIFI: PCS 0x116C0000\n");
	fl(flush);
	for (unsigned i = 0; i < sizeof(pcs_off) / sizeof(pcs_off[0]); i++) {
		kv("WIFI: PCS off", pcs_off[i]);
		kv("WIFI: PCS val", rd(PCS + pcs_off[i]));
	}
	out("\n");

	out("WIFI: PHY 0x116D0000\n");
	fl(flush);
	for (unsigned i = 0; i < sizeof(phy_off) / sizeof(phy_off[0]); i++) {
		kv("WIFI: PHY off", phy_off[i]);
		kv("WIFI: PHY val", rd(PHY + phy_off[i]));
	}
	out("\n");

	out("WIFI: DBI 0x11700000 (root complex own config)\n");
	fl(flush);
	kv("WIFI: DBI vendor/device", rd(DBI + 0x00));
	kv("WIFI: DBI command/status", rd(DBI + 0x04));
	kv("WIFI: DBI class", rd(DBI + 0x08));
	out("\n");
	out("WIFI probe done\n");
	fl(flush);
}

#undef rd

/* a traced write: address and value are logged and flushed first, so a write that hangs the bus names itself */
static void wifi_wr_traced(void (*kv)(const char *, uint64_t), void (*flush)(void), uintptr_t a, uint32_t v)
{
	kv("WIFI: write addr", a);
	kv("WIFI:   value", v);
	fl(flush);
	wr(a, v);
}

static uint32_t wifi_rd_traced(void (*kv)(const char *, uint64_t), void (*flush)(void), const char *name, uintptr_t a)
{
	uint32_t v;

	kv("WIFI: about to read", a);
	fl(flush);
	v = rd(a);
	kv(name, v);
	return v;
}

static void wifi_us(unsigned us)
{
	uint64_t end = ticks() + (TIMER_HZ / 1000000) * us;

	while (ticks() < end)
		;
}

/*
 * wifiprobe=2: Samsung's pcie0 power-on up to and including the PHY configuration (analysis/wifi-w0-sequence.md section 3,
 * steps 1-5; pci-exynos.c exynos_pcie_poweron, pci-exynos8895_cal.c). The order matters: the PHY and PCS blocks hang the bus
 * when read or written before the PMU powers the PHY (v58: PHY read at 0x116D0000 hung, PCS read as 0xffffffff). Nothing is
 * done after the PHY is configured: PERST stays low, LTSSM stays off, so no link is attempted at this stage.
 */
void wifi_bringup(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), unsigned stage);
void wifi_bringup(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), unsigned stage)
{
	static const uintptr_t GPJ = 0x11430000UL, SYSREG = 0x11421044UL;
	static const uintptr_t ELBI = 0x116A0000UL, PCS = 0x116C0000UL, PHY = 0x116D0000UL;
	/* 26 MHz gen1 tables of pci-exynos8895_cal.c:147-164, verbatim */
	static const uint8_t cmn[48] = {
		0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x04, 0x31, 0x45, 0x65, 0x24, 0x33, 0x08, 0xA4, 0xFC,
		0xC6, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0x90, 0x10, 0x04, 0x4E, 0x8E,
		0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x73, 0x47, 0x8C, 0x40, 0x00, 0x22, 0xFF, 0xFF, 0xFF, 0x00, 0x80 };
	static const uint8_t trsv[156] = {
		0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
		0x06, 0x90, 0x68, 0x66, 0x09, 0x31, 0x42, 0x44, 0xC6, 0x18, 0x03, 0x33, 0x58, 0xE7, 0x20, 0x22,
		0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
		0x00, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x81, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
		0x06, 0x90, 0x68, 0x66, 0x09, 0x31, 0x42, 0x44, 0xC6, 0x18, 0x03, 0x33, 0x58, 0xE7, 0x20, 0x22,
		0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
		0x00, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00 };
	uint32_t v;

	kv("WIFI bringup stage", stage);
	fl(flush);

	/* 1. pins: gpj1-0 CLKREQ = function 2, gpj1-2 PERST and gpj1-3 WLAN_EN = outputs; PERST low, then WLAN_EN high */
	out("WIFI: step 1 pins (gpj1)\n");
	v = rd(GPJ + 0x00);
	wifi_wr_traced(kv, flush, GPJ + 0x04, rd(GPJ + 0x04) & ~(1u << 2)); /* PERST low first */
	wifi_wr_traced(kv, flush, GPJ + 0x00, (v & ~0xFF0Fu) | 0x1102u);
	wifi_wr_traced(kv, flush, GPJ + 0x04, rd(GPJ + 0x04) | (1u << 3)); /* WLAN_EN (the chip's REG_ON) high */
	kv("WIFI: GPJ1CON now", rd(GPJ + 0x00));
	kv("WIFI: GPJ1DAT now", rd(GPJ + 0x04));
	wifi_us(100000); /* the chip's turn-on delay (bcmdhd WIFI_TURNON_DELAY) */

	/* 2. PMU: wake mask stays as it is (0), PCIE_PHY_CONTROL bit 0 := 1 powers the PHY */
	out("WIFI: step 2 PMU PCIE_PHY_CONTROL\n");
	wifi_wr_traced(kv, flush, PMU + 0x71c, (rd(PMU + 0x71c) & ~1u) | 1u);
	kv("WIFI: PCIE_PHY_CONTROL now", rd(PMU + 0x71c));
	wifi_us(1000);

	/* 3. phy_all_pwrdn_clear (channel 0) */
	out("WIFI: step 3 PHY power-down clear\n");
	wifi_wr_traced(kv, flush, PHY + 0x20 * 4, 0xC0);
	wr(PHY + 0xA7 * 4, 0x7E);
	(void)wifi_rd_traced(kv, flush, "WIFI: PHY[0x20] now", PHY + 0x20 * 4);

	/* 4. phy_config */
	out("WIFI: step 4 PHY config\n");
	wifi_wr_traced(kv, flush, SYSREG + 0x0, rd(SYSREG + 0x0) & ~(1u << 1));
	v = rd(SYSREG + 0xC);
	wifi_wr_traced(kv, flush, SYSREG + 0xC, (((v & ~(0xfu << 4)) & ~(0xfu << 2)) | (0x3u << 2)) & ~(1u << 1));
	wifi_wr_traced(kv, flush, ELBI + 0x290, 0x0);
	wifi_us(10);
	wr(ELBI + 0x28C, 0x0);
	wifi_us(10);
	wr(ELBI + 0x288, 0x1);
	wifi_us(10);
	wr(ELBI + 0x288, 0x0);
	wifi_us(10);
	wr(ELBI + 0x288, 0x1);
	wifi_us(10);
	wifi_wr_traced(kv, flush, PHY + 0 * 4, cmn[0]);
	for (unsigned i = 1; i < 48; i++)
		wr(PHY + i * 4, cmn[i]);
	for (unsigned i = 0; i < 156; i++)
		wr(PHY + (0x30 + i) * 4, trsv[i]);
	wr(PHY + 0x5C * 4, 0x14);
	wifi_wr_traced(kv, flush, PCS + 0xF8, 0x70);
	wr(PCS + 0x100, 0x87);
	wr(PCS + 0x104, 0x50);
	v = rd(PCS + 0xC);
	wr(PCS + 0xC, (v & ~(1u << 1)) | (1u << 4));
	wr(ELBI + 0x290, 0x1);
	wifi_us(10);
	wr(ELBI + 0x28C, 0x1);
	wifi_us(10);
	for (unsigned i = 0; i < 48; i++)
		wr(PHY + i * 4, cmn[i]);
	for (unsigned i = 0; i < 156; i++)
		wr(PHY + (0x30 + i) * 4, trsv[i]);
	out("WIFI: PHY configured; no link attempted (PERST low, LTSSM off)\n");

	/* 5. state after the configuration */
	kv("WIFI: PHY[0x20] after", wifi_rd_traced(kv, flush, "WIFI: PHY[0x20]", PHY + 0x20 * 4));
	kv("WIFI: PCS[0xc] after", wifi_rd_traced(kv, flush, "WIFI: PCS[0xc]", PCS + 0xC));
	kv("WIFI: PCS[0xec] after", wifi_rd_traced(kv, flush, "WIFI: PCS[0xec]", PCS + 0xEC));
	kv("WIFI: PCS[0x100] after", wifi_rd_traced(kv, flush, "WIFI: PCS[0x100]", PCS + 0x100));
	kv("WIFI: ELBI link 0x74", wifi_rd_traced(kv, flush, "WIFI: ELBI[0x74]", ELBI + 0x74));
	kv("WIFI: ELBI 0x274", wifi_rd_traced(kv, flush, "WIFI: ELBI[0x274]", ELBI + 0x274));
	out("WIFI bringup done\n");
	fl(flush);
}

/* the PHY tables of wifi_bringup, once more at file scope for the link stage (pci-exynos8895_cal.c:147-164 verbatim) */
static const uint8_t wifi_cmn[48] = {
	0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x04, 0x31, 0x45, 0x65, 0x24, 0x33, 0x08, 0xA4, 0xFC,
	0xC6, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00, 0x60, 0x11, 0x00, 0x90, 0x10, 0x04, 0x4E, 0x8E,
	0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x73, 0x47, 0x8C, 0x40, 0x00, 0x22, 0xFF, 0xFF, 0xFF, 0x00, 0x80 };
static const uint8_t wifi_trsv[156] = {
	0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x68, 0x66, 0x09, 0x31, 0x42, 0x44, 0xC6, 0x18, 0x03, 0x33, 0x58, 0xE7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
	0x00, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x81, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF, 0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
	0x06, 0x90, 0x68, 0x66, 0x09, 0x31, 0x42, 0x44, 0xC6, 0x18, 0x03, 0x33, 0x58, 0xE7, 0x20, 0x22,
	0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
	0x00, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x85, 0xFF, 0x00 };

/* exynos_pcie_phy_config (pci-exynos8895_cal.c:145-229): resets, the tables, the PCS settings, the tables again */
static void wifi_phy_config(void)
{
	static const uintptr_t SYSREG = 0x11421044UL, ELBI = 0x116A0000UL, PCS = 0x116C0000UL, PHY = 0x116D0000UL;
	uint32_t v;

	wr(SYSREG + 0x0, rd(SYSREG + 0x0) & ~(1u << 1));
	v = rd(SYSREG + 0xC);
	wr(SYSREG + 0xC, (((v & ~(0xfu << 4)) & ~(0xfu << 2)) | (0x3u << 2)) & ~(1u << 1));
	wr(ELBI + 0x290, 0x0);
	wifi_us(10);
	wr(ELBI + 0x28C, 0x0);
	wifi_us(10);
	wr(ELBI + 0x288, 0x1);
	wifi_us(10);
	wr(ELBI + 0x288, 0x0);
	wifi_us(10);
	wr(ELBI + 0x288, 0x1);
	wifi_us(10);
	for (unsigned i = 0; i < 48; i++)
		wr(PHY + i * 4, wifi_cmn[i]);
	for (unsigned i = 0; i < 156; i++)
		wr(PHY + (0x30 + i) * 4, wifi_trsv[i]);
	wr(PHY + 0x5C * 4, 0x14);
	wr(PCS + 0xF8, 0x70);
	wr(PCS + 0x100, 0x87);
	wr(PCS + 0x104, 0x50);
	v = rd(PCS + 0xC);
	wr(PCS + 0xC, (v & ~(1u << 1)) | (1u << 4));
	wr(ELBI + 0x290, 0x1);
	wifi_us(10);
	wr(ELBI + 0x28C, 0x1);
	wifi_us(10);
	for (unsigned i = 0; i < 48; i++)
		wr(PHY + i * 4, wifi_cmn[i]);
	for (unsigned i = 0; i < 156; i++)
		wr(PHY + (0x30 + i) * 4, wifi_trsv[i]);
}

/*
 * wifiprobe=3: the whole of exynos_pcie_poweron + exynos_pcie_establish_link (pci-exynos.c:1549, 587; analysis/
 * wifi-w0-sequence.md), up to the link training: pins, PHY power, PHY configuration after PERST goes high (as Samsung's
 * code orders it), root-complex setup through the DBI, then LTSSM enable. The link-state register (ELBI 0x74) is logged as it
 * changes, for up to half a second, with the time of each change, and the state history is dumped at the end: a failed
 * training still says how far the link got.
 */
void wifi_w3_otp(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));	/* appended at the end of this file */
void wifi_link(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), unsigned stage);
void wifi_link(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), unsigned stage)
{
	static const uintptr_t GPJ = 0x11430000UL, SYSREG = 0x11421044UL;
	static const uintptr_t ELBI = 0x116A0000UL, PCS = 0x116C0000UL, PHY = 0x116D0000UL, DBI = 0x11700000UL;
	uint32_t v, last = 0xffffffffu;
	uint64_t t0, end;
	unsigned changes = 0;

	out("WIFI link stage: Samsung's poweron + establish_link\n");
	fl(flush);

	/* poweron steps 1-3 as in wifi_bringup */
	wifi_wr_traced(kv, flush, GPJ + 0x04, rd(GPJ + 0x04) & ~(1u << 2));
	wifi_wr_traced(kv, flush, GPJ + 0x00, (rd(GPJ + 0x00) & ~0xFF0Fu) | 0x1102u);
	wifi_wr_traced(kv, flush, GPJ + 0x04, rd(GPJ + 0x04) | (1u << 3));
	wifi_us(100000);
	wifi_wr_traced(kv, flush, PMU + 0x71c, (rd(PMU + 0x71c) & ~1u) | 1u);
	wifi_us(1000);
	wifi_wr_traced(kv, flush, PHY + 0x20 * 4, 0xC0);
	wr(PHY + 0xA7 * 4, 0x7E);
	out("WIFI: powered, PHY out of power-down\n");
	fl(flush);

	/* establish_link */
	wifi_wr_traced(kv, flush, PCS + 0xEC, rd(PCS + 0xEC) | (1u << 3)); /* no rx-elecidle check while the DBI is used */
	wr(ELBI + 0x1D0, 0x0);                                              /* soft core reset */
	wifi_us(20);
	wr(ELBI + 0x1D0, 0x1);
	out("WIFI: PERST high\n");
	fl(flush);
	wr(GPJ + 0x04, rd(GPJ + 0x04) | (1u << 2));
	wifi_us(20000);                                                     /* usleep_range(18000, 20000) */
	wr(ELBI + 0xF4, rd(ELBI + 0xF4) | 0x1 | (0x1 << 4));               /* APP_REQ_EXIT_L1_MODE | L1_REQ_NAK_CONTROL_MASTER */
	wr(ELBI + 0x1B8, 1u << 1);                                          /* PCIE_LINKDOWN_RST_MANUAL */
	wr(ELBI + 0x2C8, rd(ELBI + 0x2C8) & ~0xFFFu);                       /* QCH: no clock gating (ip-ver >= 0x889500) */
	out("WIFI: phy_config after PERST\n");
	fl(flush);
	wifi_phy_config();
	wr(ELBI + 0x018, rd(ELBI + 0x018) & ~(1u << 1));                    /* PCIE_SW_WAKE: bus number enable */
	wr(ELBI + 0x038, 0x1);                                              /* PCIE_L1_BUG_FIX_ENABLE */

	/* dw_pcie_setup_rc + exynos_pcie_setup_rc through the DBI (root complex's own config space) */
	out("WIFI: root complex setup (DBI)\n");
	fl(flush);
	wifi_wr_traced(kv, flush, DBI + 0x8BC, 0x1);                        /* PCIE_MISC_CONTROL: DBI_RO_WR_EN */
	v = rd(DBI + 0x710);
	wr(DBI + 0x710, (v & ~(0x3fu << 16)) | (0x1u << 16));              /* PORT_LINK_CONTROL: 1 lane */
	v = rd(DBI + 0x80C);
	wr(DBI + 0x80C, (v & ~(0x1fu << 8)) | (0x1u << 8));                 /* LINK_WIDTH_SPEED_CONTROL: 1 lane */
	wr(DBI + 0x10, 0x00000004);                                         /* RC BAR0, BAR1 */
	wr(DBI + 0x14, 0x00000000);
	v = rd(DBI + 0x3C);
	wr(DBI + 0x3C, (v & 0xffff00ffu) | 0x00000100u);                    /* interrupt pin */
	v = rd(DBI + 0x18);
	wr(DBI + 0x18, (v & 0xff000000u) | 0x00010100u);                    /* bus numbers 0, 1, 1 */
	wr(DBI + 0x20, 0x11C01180u);                                        /* memory base 0x11800000, limit 0x11C00000 */
	v = rd(DBI + 0x04);
	wr(DBI + 0x04, (v & 0xffff0000u) | 0x107u);                         /* IO | MEM | MASTER | SERR */
	v = rd(DBI + 0x7C);
	wr(DBI + 0x7C, (v & ~(0x7u << 15 | 0x3fu << 4 | 0xfu)) | (0x6u << 15) | (0x1u << 4) | 0x2u); /* LNKCAP: L1 exit latency 64 us, x1, 5 GT/s */
	wr(DBI + 0xB40, 0x1A);                                              /* aux clock 26 MHz */
	wr(DBI + 0xB44, 0xEA);                                              /* L1 substates durations */
	wr(DBI + 0x44, 0x0);                                                /* PM control/status */
	v = rd(DBI + 0x80);
	wr(DBI + 0x80, v | (1u << 5));                                      /* LNKCTL: retrain link */
	v = rd(DBI + 0xA0);
	wr(DBI + 0xA0, (v & ~0xfu) | 0x1u);                                 /* LNKCTL2: target speed 2.5 GT/s */
	v = rd(SYSREG + 0xC);
	wr(SYSREG + 0xC, (v & ~(1u << 16)) | (1u << 14) | (1u << 12));     /* PHY bifurcation mode, link 0 and PCS lane 0 enable */

	wifi_wr_traced(kv, flush, PCS + 0xEC, rd(PCS + 0xEC) & ~(1u << 3)); /* rx-elecidle check back on */

	/* LTSSM enable, then watch the link-state register */
	kv("WIFI: ELBI 0x74 before LTSSM", rd(ELBI + 0x74));
	out("WIFI: LTSSM enable\n");
	fl(flush);
	wr(ELBI + 0x02C, 0x1);
	t0 = ticks();
	end = t0 + TIMER_HZ / 2;
	while (ticks() < end) {
		v = rd(ELBI + 0x74);
		if (v != last && changes < 24) {
			kv("WIFI: link-state change at us", (ticks() - t0) / (TIMER_HZ / 1000000));
			kv("WIFI:   ELBI 0x74", v);
			changes++;
		}
		last = v;
		if ((v & 0x1f) >= 0x0d && (v & 0x1f) <= 0x14)
			break;
	}
	v = rd(ELBI + 0x74);
	kv("WIFI: final ELBI 0x74", v);
	kv("WIFI: LINK UP (state 0x0d..0x14)", ((v & 0x1f) >= 0x0d && (v & 0x1f) <= 0x14) ? 1 : 0);
	for (unsigned i = 0; i < 8; i++)
		kv("WIFI: LTSSM history", rd(ELBI + 0x138 + i * 4));
	kv("WIFI: DBI link status (LNKSTA 0x82)", rd(DBI + 0x80) >> 16);

	if (stage >= 4 && (kv("WIFI: link check", 0), ((rd(ELBI + 0x74) & 0x1f) >= 0x0d && (rd(ELBI + 0x74) & 0x1f) <= 0x14))) {
		/*
		 * wifiprobe=4: the device's configuration space. Outbound ATU region 0 = type CFG0 over the configuration window
		 * 0x11BFF000..0x11BFFFFF (DT reg "config") targeting bus 1, device 0, function 0 (pci-exynos.c:509
		 * exynos_pcie_prog_viewport_cfg0; DesignWare ATU registers at DBI + 0x900..0x91C).
		 */
		static const uintptr_t CFG = 0x11BFF000UL;

		out("WIFI: stage 4, outbound CFG0 window for bus 1 dev 0 fn 0\n");
		fl(flush);
		wifi_wr_traced(kv, flush, DBI + 0x900, 0x00000000u);            /* outbound, region 0 */
		wr(DBI + 0x90C, (uint32_t)CFG);                                 /* lower base */
		wr(DBI + 0x910, 0x0);                                           /* upper base */
		wr(DBI + 0x914, (uint32_t)CFG + 0x1000 - 1);                    /* limit */
		wr(DBI + 0x918, 0x01000000u);                                   /* lower target: bus 1, dev 0, fn 0 */
		wr(DBI + 0x91C, 0x0);                                           /* upper target */
		wr(DBI + 0x904, 0x4);                                           /* type CFG0 */
		wr(DBI + 0x908, 0x80000000u);                                   /* enable */
		kv("WIFI: ATU CR2 readback", rd(DBI + 0x908));
		v = wifi_rd_traced(kv, flush, "WIFI: EP vendor/device", CFG + 0x00);
		kv("WIFI: EP VENDOR ID", v & 0xffff);
		kv("WIFI: EP DEVICE ID", v >> 16);
		kv("WIFI: EP command/status", wifi_rd_traced(kv, flush, "WIFI: EP +0x04", CFG + 0x04));
		kv("WIFI: EP class/revision", wifi_rd_traced(kv, flush, "WIFI: EP +0x08", CFG + 0x08));
		kv("WIFI: EP BAR0", wifi_rd_traced(kv, flush, "WIFI: EP +0x10", CFG + 0x10));
		kv("WIFI: EP BAR1", wifi_rd_traced(kv, flush, "WIFI: EP +0x14", CFG + 0x14));
		kv("WIFI: EP BAR2", wifi_rd_traced(kv, flush, "WIFI: EP +0x18", CFG + 0x18));
		kv("WIFI: EP BAR3", wifi_rd_traced(kv, flush, "WIFI: EP +0x1c", CFG + 0x1c));
		kv("WIFI: EP subsystem", wifi_rd_traced(kv, flush, "WIFI: EP +0x2c", CFG + 0x2c));
		kv("WIFI: EP capabilities pointer", wifi_rd_traced(kv, flush, "WIFI: EP +0x34", CFG + 0x34));

		if (stage >= 5) {
			/*
			 * wifiprobe=5 (W1): outbound MEM window (ATU region 1, 0x11800000 up to the config window, PCI address = CPU
			 * address), size the chip's two 64-bit BARs, assign them inside that window, enable memory space, then point
			 * the chip's BAR0 backplane window (Broadcom config register 0x80) at ChipCommon (0x18000000) and read its
			 * chip-id register through BAR0. Expected chip id 0x4361.
			 */
			static const uintptr_t MEMWIN = 0x11800000UL;
			uint32_t orig[4], got[4], size0, size2, base0, base2, id;

			out("WIFI: stage 5, outbound MEM window + BAR sizing\n");
			fl(flush);
			wifi_wr_traced(kv, flush, DBI + 0x900, 0x00000001u); /* outbound, region 1 */
			wr(DBI + 0x90C, (uint32_t)MEMWIN);
			wr(DBI + 0x910, 0x0);
			wr(DBI + 0x914, (uint32_t)MEMWIN + 0x3FF000 - 1);    /* up to the configuration window */
			wr(DBI + 0x918, (uint32_t)MEMWIN);                  /* PCI address = CPU address */
			wr(DBI + 0x91C, 0x0);
			wr(DBI + 0x904, 0x0);                               /* type MEM */
			wr(DBI + 0x908, 0x80000000u);                       /* enable */
			kv("WIFI: ATU region 1 CR2", rd(DBI + 0x908));

			for (unsigned i = 0; i < 4; i++)
				orig[i] = rd(CFG + 0x10 + 4 * i);
			for (unsigned i = 0; i < 4; i++)
				wr(CFG + 0x10 + 4 * i, 0xffffffffu);
			for (unsigned i = 0; i < 4; i++)
				got[i] = rd(CFG + 0x10 + 4 * i);
			for (unsigned i = 0; i < 4; i++)
				wr(CFG + 0x10 + 4 * i, orig[i]);
			for (unsigned i = 0; i < 4; i++) {
				kv("WIFI: BAR original", orig[i]);
				kv("WIFI: BAR size-probe readback", got[i]);
			}
			size0 = ~(got[0] & ~0xfu) + 1;
			size2 = ~(got[2] & ~0xfu) + 1;
			kv("WIFI: BAR0 size", size0);
			kv("WIFI: BAR2 size", size2);
			base0 = (uint32_t)MEMWIN;
			base2 = size2 ? ((base0 + size0 + size2 - 1) & ~(size2 - 1)) : 0;
			kv("WIFI: BAR0 base", base0);
			kv("WIFI: BAR2 base", base2);
			if (size0 != 0 && size0 <= 0x100000) {
				wifi_wr_traced(kv, flush, CFG + 0x10, base0 | (got[0] & 0xfu));
				wr(CFG + 0x14, 0x0);
				if (size2 != 0 && size2 <= 0x800000 && base2 + size2 <= (uint32_t)MEMWIN + 0x3FF000) {
					wr(CFG + 0x18, base2 | (got[2] & 0xfu));
					wr(CFG + 0x1c, 0x0);
				} else {
					out("WIFI: BAR2 (4 MiB) does not fit beside BAR0 in the 4 MiB outbound window: left unassigned\n");
				}
				kv("WIFI: BAR0 now", rd(CFG + 0x10));
				kv("WIFI: BAR2 now", rd(CFG + 0x18));
				wr(CFG + 0x04, (rd(CFG + 0x04) & 0xffffu) | 0x6u); /* memory space + bus master */
				kv("WIFI: EP command now", rd(CFG + 0x04) & 0xffffu);
				wifi_wr_traced(kv, flush, CFG + 0x80, 0x18000000u); /* BAR0 window -> ChipCommon */
				kv("WIFI: BAR0 window readback", rd(CFG + 0x80));
				id = wifi_rd_traced(kv, flush, "WIFI: ChipCommon[0] chip id register", base0 + 0x0);
				kv("WIFI: CHIP ID", id & 0xffff);
				kv("WIFI: chip revision", (id >> 16) & 0xf);
				kv("WIFI: packaging option", (id >> 20) & 0xf);
				kv("WIFI: core count", (id >> 24) & 0xf);
				kv("WIFI: ChipCommon[4] capabilities", wifi_rd_traced(kv, flush, "WIFI: ChipCommon[4]", base0 + 0x4));

				if (stage == 6) {
					/*
					 * wifiprobe=6: dump the chip's EROM (the AI core table, ChipCommon EROMPTR at +0xFC) raw, through the
					 * BAR0 backplane window, until the end marker 0xF (at most 1024 words). Parsed offline on the PC:
					 * core ids and base/wrapper addresses (ARM CR4, SOCRAM, PCIe2).
					 */
					uint32_t erom = rd(base0 + 0xFC), win = 0xffffffffu, w;

					kv("WIFI: EROMPTR", erom);
					fl(flush);
					for (unsigned i = 0; i < 1024; i++) {
						uint32_t a = erom + 4 * i, want = a & ~0xFFFu;

						if (want != win) {
							wr(CFG + 0x80, want);
							(void)rd(CFG + 0x80);
							win = want;
						}
						w = rd(base0 + (a & 0xFFFu));
						kv("E", w);
						if (w == 0xF)
							break;
					}
					out("\nWIFI: EROM dump done\n");
					fl(flush);
				}

				if (stage >= 7) {
					/*
					 * wifiprobe=7 (W1): the final window layout, then halt the chip's ARM CR4 and prove the RAM is reachable.
					 * Core addresses are from the chip's own EROM (parse_erom.py on the v64 dump): ARM CR4 registers
					 * 0x18002000, its wrapper 0x18102000, RAM slave 0x170000 + 0x150000 (RAM base 0x170000, size 0x150000);
					 * the TCM is backplane address 0 through the second BAR once PCI_BAR1_WIN (config 0x84) is 0.
					 * ATU region 1 = BAR0 only (32 KiB), region 2 = TCM window (CPU 0x11808000.. -> PCI 0x12000000..), so both
					 * BARs fit although the SoC's outbound range is only 4 MiB. The halt is Samsung's ai_core_reset
					 * (aiutils.c / dhdpcie_bus_download_state): wrapper IOCTRL 0x408, RESETCTRL 0x800, RESETSTATUS 0x804.
					 */
					static const uintptr_t TCM = 0x11808000UL, WRAP_CR4 = 0x18102000UL;
					uint32_t io, rc, tries;

					out("WIFI: stage 7, window layout + ARM CR4 halt + TCM test\n");
					fl(flush);
					wifi_wr_traced(kv, flush, DBI + 0x900, 0x00000001u); /* region 1: BAR0 only */
					wr(DBI + 0x90C, (uint32_t)MEMWIN);
					wr(DBI + 0x910, 0x0);
					wr(DBI + 0x914, (uint32_t)MEMWIN + 0x8000 - 1);
					wr(DBI + 0x918, (uint32_t)MEMWIN);
					wr(DBI + 0x91C, 0x0);
					wr(DBI + 0x904, 0x0);
					wr(DBI + 0x908, 0x80000000u);
					wifi_wr_traced(kv, flush, DBI + 0x900, 0x00000002u); /* region 2: TCM window */
					wr(DBI + 0x90C, (uint32_t)TCM);
					wr(DBI + 0x910, 0x0);
					wr(DBI + 0x914, (uint32_t)MEMWIN + 0x3FF000 - 1);
					wr(DBI + 0x918, 0x12000000u);
					wr(DBI + 0x91C, 0x0);
					wr(DBI + 0x904, 0x0);
					wr(DBI + 0x908, 0x80000000u);
					kv("WIFI: ATU region 2 CR2 readback", rd(DBI + 0x908));
					kv("WIFI: ATU region 2 limit readback", rd(DBI + 0x914));
					wifi_wr_traced(kv, flush, CFG + 0x18, 0x12000000u | (got[2] & 0xfu));
					wr(CFG + 0x1c, 0x0);
					kv("WIFI: BAR2 now", rd(CFG + 0x18));
					wifi_wr_traced(kv, flush, CFG + 0x84, 0x0); /* PCI_BAR1_WIN: second BAR maps backplane address 0 (TCM) */
					kv("WIFI: PCI_BAR1_WIN readback", rd(CFG + 0x84));

					wifi_wr_traced(kv, flush, CFG + 0x80, (uint32_t)WRAP_CR4); /* BAR0 window -> the CR4 wrapper */
					kv("WIFI: BAR0 window readback", rd(CFG + 0x80));
					io = wifi_rd_traced(kv, flush, "WIFI: CR4 IOCTRL before", base0 + 0x408);
					rc = wifi_rd_traced(kv, flush, "WIFI: CR4 RESETCTRL before", base0 + 0x800);
					kv("WIFI: CR4 RESETSTATUS before", wifi_rd_traced(kv, flush, "WIFI: CR4 RESETSTATUS", base0 + 0x804));
					(void)io;
					(void)rc;
					/* ai_core_disable(SICF_CPUHALT): reset the core, then ioctrl = bits | FGC | CLOCK_EN */
					if (!(rd(base0 + 0x800) & 1)) {
						for (tries = 0; tries < 300 && rd(base0 + 0x804) != 0; tries++)
							wifi_us(1);
						wifi_wr_traced(kv, flush, base0 + 0x800, 0x1);
						wifi_us(10);
						for (tries = 0; tries < 300 && rd(base0 + 0x804) != 0; tries++)
							wifi_us(1);
						wr(base0 + 0x408, 0x20 | 0x2 | 0x1);
						(void)rd(base0 + 0x408);
					}
					/* ai_core_reset(SICF_CPUHALT, 0) */
					wr(base0 + 0x408, 0x20 | 0x2 | 0x1); /* CPUHALT | FGC | CLOCK_EN */
					(void)rd(base0 + 0x408);
					wr(base0 + 0x800, 0x0);              /* out of reset */
					(void)rd(base0 + 0x800);
					wifi_us(1);
					wr(base0 + 0x408, 0x20 | 0x1);       /* CPUHALT | CLOCK_EN (FGC off) */
					(void)rd(base0 + 0x408);
					wifi_us(1);
					kv("WIFI: CR4 IOCTRL after halt (expect 0x21)", rd(base0 + 0x408));
					kv("WIFI: CR4 RESETCTRL after (expect 0)", rd(base0 + 0x800));

					/* the TCM: the RAM at backplane 0x170000 (offset = 0x170000 in the TCM window) */
					out("WIFI: TCM test\n");
					fl(flush);
					kv("WIFI: TCM[0] (reset vector area)", wifi_rd_traced(kv, flush, "WIFI: TCM[0]", TCM + 0x0));
					kv("WIFI: TCM[0x170000] RAM first word", wifi_rd_traced(kv, flush, "WIFI: TCM[0x170000]", TCM + 0x170000));
					wifi_wr_traced(kv, flush, TCM + 0x170100, 0xa5a55a5au);
					wr(TCM + 0x170104, 0x12345678u);
					kv("WIFI: RAM test word 1 (expect 0xa5a55a5a)", rd(TCM + 0x170100));
					kv("WIFI: RAM test word 2 (expect 0x12345678)", rd(TCM + 0x170104));
					out("WIFI: stage 7 done\n");
					fl(flush);
				}

				if (stage >= 8) {
					/*
					 * wifiprobe=8 (W1b): download Samsung's firmware to the RAM base (0x170000), the NVRAM at the RAM end with
					 * its length token in the last word, the reset instruction (first firmware word) to address 0, release
					 * the CR4, then wait for the firmware to replace the token with its pcie_shared pointer and dump its console.
					 * Same order as bcmdhd dhdpcie_download_code_file / write_vars / download_state(FALSE).
					 */
					extern const uint32_t wifi_fw[], wifi_fw_end[], wifi_nvram[], wifi_nvram_end[];
					static const uintptr_t TCMW = 0x11808000UL, RAMB = 0x170000UL, RAMS = 0x150000UL;
					uint32_t fwn = (uint32_t)(wifi_fw_end - wifi_fw), nvn = (uint32_t)(wifi_nvram_end - wifi_nvram), i, bad = 0, v;
					uint32_t token, shared = 0;
					uintptr_t ramend = RAMB + RAMS;

					out("WIFI: stage 8, firmware download\n");
					kv("WIFI: firmware words", fwn);
					kv("WIFI: nvram words", nvn);
					fl(flush);
					if (stage >= 9)	/* W3d: identify the module from the OTP CIS first, as DHD does before the download (restores the BAR0 window) */
						wifi_w3_otp(kv, out, flush);
					for (i = 0; i < fwn; i++)
						wr(TCMW + RAMB + 4 * i, wifi_fw[i]);
					for (i = 0; i < fwn; i++)
						if (rd(TCMW + RAMB + 4 * i) != wifi_fw[i] && bad++ < 4)
							kv("WIFI: firmware mismatch at word", i);
					kv("WIFI: firmware mismatches", bad);
					fl(flush);
					for (i = 0; i < nvn; i++)
						wr(TCMW + ramend - 4 - 4 * nvn + 4 * i, wifi_nvram[i]);
					token = ((~nvn & 0xffffu) << 16) | (nvn & 0xffffu);
					wr(TCMW + ramend - 4, token);
					kv("WIFI: nvram token", rd(TCMW + ramend - 4));
					wr(TCMW + 0, wifi_fw[0]); /* resetinstr */
					kv("WIFI: reset instruction readback", rd(TCMW + 0));
					fl(flush);
					/* release the core: ai_core_reset(0, 0) = ioctrl FGC|CLK, reset 0, ioctrl CLK */
					wr(base0 + 0x408, 0x2 | 0x1);
					(void)rd(base0 + 0x408);
					wr(base0 + 0x800, 0x0);
					(void)rd(base0 + 0x800);
					wifi_us(1);
					wr(base0 + 0x408, 0x1);
					kv("WIFI: CR4 IOCTRL after release", rd(base0 + 0x408));
					fl(flush);
					for (i = 0; i < 5000; i++) {
						v = rd(TCMW + ramend - 4);
						if (v != token) {
							shared = v;
							break;
						}
						wifi_us(1000);
					}
					kv("WIFI: ms waited for the firmware", i);
					kv("WIFI: last RAM word (pcie_shared pointer)", shared);
					kv("WIFI: CR4 IOCTRL now", rd(base0 + 0x408));
					fl(flush);
					if (shared >= RAMB && shared < ramend && !(shared & 3)) {
						uint32_t sh[8], cons, buf, bsz, idx, n, k, j;
						char line[65];

						for (i = 0; i < 8; i++) {
							sh[i] = rd(TCMW + shared + 4 * i);
							kv("WIFI: pcie_shared word", sh[i]);
						}
						cons = sh[5];
						if (cons >= RAMB && cons < ramend) {
							buf = rd(TCMW + cons + 8);
							bsz = rd(TCMW + cons + 12);
							idx = rd(TCMW + cons + 16);
							kv("WIFI: console buf", buf);
							kv("WIFI: console size", bsz);
							kv("WIFI: console idx", idx);
							n = idx < bsz ? idx : bsz;
							if (n > 1500 || buf < RAMB || buf >= ramend)
								n = buf >= RAMB && buf < ramend ? 1500 : 0;
							out("WIFI: console: ");
							for (k = 0, j = 0; k < n; k += 4) {
								uint32_t w = rd(TCMW + buf + k);
								uint32_t b;

								for (b = 0; b < 4; b++) {
									char c = (char)(w >> (8 * b));
									line[j++] = (c >= 32 && c < 127) ? c : '|';
									if (j == 64) {
										line[j] = 0;
										out(line);
										j = 0;
									}
								}
							}
							line[j] = 0;
							out(line);
							out("\n");
						}
					}
					out("WIFI: stage 8 done\n");
					fl(flush);
				}
			} else {
				out("WIFI: BAR sizes not plausible, BARs left alone\n");
			}
		}
	}
	out("WIFI link stage done\n");
	fl(flush);
}

/*
 * mngsexp=7: what stock Linux does before its first big-core CPU_ON (analysis: scratch/bigcore/notes.md; ref s8kernel-tgp-p
 * arch/arm64/kernel/smp.c:530-547, drivers/soc/samsung/exynos-cpumap.c:19-81): the CPUMAP SMC (x0 = -10, x1 = n) for every
 * cpu, whose result is how EL3 hands out the MPIDRs. Then one plain CPU_ON for Mongoose core 0. Reads only plus those SMCs.
 */
#define SMC_CMD_CPUMAP	((uint64_t)(int64_t)-10)
#define CHIPID_LOT	0x10000004UL
#define GICD_BASE	0x10201000UL

static void mexp7_lot_id(char out[6])
{
	uint32_t v = rd(CHIPID_LOT), r = 0;

	for (unsigned i = 0; i < 32; i++)
		r |= ((v >> i) & 1u) << (31 - i);
	r = (r >> 11) & 0x1fffff;
	for (int i = 4; i >= 1; i--) {
		uint32_t m = r % 36;

		r /= 36;
		out[i] = (char)(m < 10 ? m + '0' : m - 10 + 'A');
	}
	out[0] = 'N';
	out[5] = 0;
}

static void mexp7_snap(uint32_t *buf)	/* PMU INFORM0..15 */
{
	/* the INFORM registers at PMU+0x800 are not readable here (the read ended the boot in the first mngsexp=7 runs) */
	for (unsigned i = 0; i < 16; i++)
		buf[i] = 0;
}

static void mngs_exp7(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), unsigned mode)
{
	char lot[6];
	uint32_t s0[16], s1[16];

	out("MEXP7: begin\n");
	kv("MEXP7: CHIPID+4 raw", rd(CHIPID_LOT));
	mexp7_lot_id(lot);
	out("MEXP7: lot id ");
	out(lot);
	out("\n");
	kv("MEXP7: GICD_CTLR", rd(GICD_BASE + 0x000));
	kv("MEXP7: GICD_TYPER", rd(GICD_BASE + 0x004));
	kv("MEXP7: 0x1438 ECMU_CL0 SYS_PWR", rd(PMU + 0x1438));
	kv("MEXP7: 0x1384 QCH_ECMU_CL0", rd(PMU + 0x1384));
	kv("MEXP7: 0x13f8 CLKSTOP_OPEN_CL0", rd(PMU + 0x13f8));
	kv("MEXP7: 0x14b8 CLKSTOP_CL0", rd(PMU + 0x14b8));
	kv("MEXP7: 0x143c ECMU_CL1 SYS_PWR", rd(PMU + 0x143c));
	kv("MEXP7: noncpu cfg 0x2400", rd(PMU + 0x2400));
	kv("MEXP7: l2 cfg 0x2600", rd(PMU + 0x2600));
	kv("MEXP7: cl1 noncpu st 0x2424", rd(PMU + 0x2424));
	kv("MEXP7: cl1 l2 st 0x2624", rd(PMU + 0x2624));
	fl(flush);

	/* 1. the SMCs Linux issues before the first secondary CPU_ON (mode 8: only n < 4, mode 9: n <= 4 and stop) */
	for (uint64_t n = 0; n < 8; n++) {
		if (mode == 8 && n >= 4)
			break;
		if (mode == 9 && n > 4)
			break;
		out("MEXP7: CPUMAP call\n");
		fl(flush);
		int64_t h = smc3(SMC_CMD_CPUMAP, n, 0, 0);

		kv("MEXP7: CPUMAP (n<<32|ret)", (n << 32) | (uint32_t)h);
		fl(flush);
	}
	if (mode == 9) {
		out("MEXP7: mode 9 stops after CPUMAP(4)\n");
		fl(flush);
		return;
	}

	/* 2. Mongoose core 0, plain CPU_ON as Linux / S-Boot do, then watch 300 ms */
	for (unsigned i = 0; i < 16; i++)
		smp_flags[i] = 0;
	dc_civac_range(smp_entry3, 256);
	dc_civac_range((const void *)smp_flags, 128);
	mexp7_snap(s0);
	out("MEXP7: calling CPU_ON(0x000)\n");
	fl(flush);
	int64_t rcm = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry3, 0);

	mexp7_snap(s1);
	kv("MEXP7: MNGS 0x000 CPU_ON rc", (uint64_t)rcm);
	for (unsigned i = 0; i < 16; i++)
		if (s0[i] != s1[i])
			kv("MEXP7: INFORM changed (idx<<32|new)", ((uint64_t)i << 32) | s1[i]);
	fl(flush);
	kv("MEXP7: core0 st right after", rd(CPU_STAT(0)));
	kv("MEXP7: seq st right after", rd(PMU + 0x2484));
	kv("MEXP7: noncpu st right after", rd(PMU + 0x2404));
	kv("MEXP7: l2 st right after", rd(PMU + 0x2604));
	fl(flush);
	int up = wait_flag_inv(0, 300);

	kv("MONGOOSE STARTED", up);
	fl(flush);
	for (unsigned s = 8; s < 11; s++)
		__asm__ volatile("dc ivac, %0; dsb sy" :: "r"(&smp_flags[s]) : "memory");
	kv("MEXP7: MIDR_EL1", smp_flags[8]);
	kv("MEXP7: CurrentEL", smp_flags[9]);
	kv("MEXP7: MPIDR_EL1", smp_flags[10]);
	kv("MEXP7: core0 st", rd(CPU_STAT(0)));
	kv("MEXP7: seq st", rd(PMU + 0x2484));
	kv("MEXP7: noncpu st", rd(PMU + 0x2404));
	kv("MEXP7: l2 st", rd(PMU + 0x2604));
	fl(flush);
	/* a second CPU_ON: -4 (ALREADY_ON) or -5 (ON_PENDING) would prove EL3 tracked the first one */
	kv("MEXP7: MNGS 2nd CPU_ON rc", (uint64_t)smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry3, 0));
	fl(flush);
	kv("MEXP7: AFFINITY_INFO 0x000", (uint64_t)smc3(0xc4000004UL, 0x000, 0, 0));
	kv("MEXP7: PSCI_VERSION", (uint64_t)smc3(0x84000000UL, 0, 0, 0));
	fl(flush);
	smp_flags[11] = 1;	/* release the stub if it ever ran */
	__asm__ volatile("dc civac, %0; dsb sy" :: "r"(&smp_flags[11]) : "memory");
	out("MEXP7: done\n");
	fl(flush);
}

/*
 * mngsexp=10: the GIC distributor reads 0 (disabled) in the loader but is enabled in stock Linux before its first CPU_ON. Enable it,
 * try a plain CPU_ON of an A53 (control: plain CPU_ON never started one so far) and of Mongoose core 0, restore GICD_CTLR.
 */
static void mngs_exp10(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	uint32_t old = rd(GICD_BASE + 0x000);
	int up;

	out("MEXP10: begin\n");
	kv("MEXP10: GICD_CTLR before", old);
	kv("MEXP10: GICD_TYPER", rd(GICD_BASE + 0x004));
	wr(GICD_BASE + 0x000, old | 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	kv("MEXP10: GICD_CTLR after write", rd(GICD_BASE + 0x000));
	fl(flush);

	/* control: A53 core 0x101 */
	for (unsigned i = 0; i < 16; i++)
		smp_flags[i] = 0;
	dc_civac_range(smp_entry3, 256);
	dc_civac_range((const void *)smp_flags, 128);
	out("MEXP10: CPU_ON(0x101) plain\n");
	fl(flush);
	int64_t rca = smc3(PSCI_CPU_ON, 0x101, (uintptr_t)smp_entry3, 0);

	kv("MEXP10: A53 0x101 CPU_ON rc", (uint64_t)rca);
	up = wait_flag_inv(0, 300);
	kv("MEXP10: A53 STARTED", up);
	kv("MEXP10: A53 0x101 st", rd(CPU_STAT(0x101)));
	fl(flush);
	smp_flags[11] = 1;			/* release a started stub: it powers itself off */
	__asm__ volatile("dc civac, %0; dsb sy" :: "r"(&smp_flags[11]) : "memory");
	{
		uint64_t e = ticks() + TIMER_HZ / 10;

		while (ticks() < e)
			;
	}
	kv("MEXP10: A53 0x101 st after release", rd(CPU_STAT(0x101)));
	fl(flush);

	/* Mongoose core 0 */
	for (unsigned i = 0; i < 16; i++)
		smp_flags[i] = 0;
	dc_civac_range(smp_entry3, 256);
	dc_civac_range((const void *)smp_flags, 128);
	out("MEXP10: CPU_ON(0x000) plain\n");
	fl(flush);
	int64_t rcm = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)smp_entry3, 0);

	kv("MEXP10: MNGS 0x000 CPU_ON rc", (uint64_t)rcm);
	up = wait_flag_inv(0, 300);
	kv("MONGOOSE STARTED", up);
	for (unsigned s = 8; s < 11; s++)
		__asm__ volatile("dc ivac, %0; dsb sy" :: "r"(&smp_flags[s]) : "memory");
	kv("MEXP10: MIDR_EL1", smp_flags[8]);
	kv("MEXP10: core0 st", rd(CPU_STAT(0)));
	kv("MEXP10: noncpu st", rd(PMU + 0x2404));
	kv("MEXP10: l2 st", rd(PMU + 0x2604));
	smp_flags[11] = 1;
	__asm__ volatile("dc civac, %0; dsb sy" :: "r"(&smp_flags[11]) : "memory");
	wr(GICD_BASE + 0x000, old);
	kv("MEXP10: GICD_CTLR restored", rd(GICD_BASE + 0x000));
	out("MEXP10: done\n");
	fl(flush);
}

/*
 * s8bigboot=1: start XNU on Mongoose core 0 instead of this Cortex-A53 (the kernel numbers the cores 0-3 Mongoose, 4-7 Cortex, so its
 * boot CPU is a Mongoose). The GIC distributor must be enabled for EL3 to act on CPU_ON (loader experiment mngsexp=10). The new core runs
 * mongoose_boot_entry (start.S): the loader's own jump to XNU, with x0 = boot_args. Returns 0 when CPU_ON was accepted; the caller then
 * returns 0 to start.S, which powers this core off.
 */
extern char mongoose_boot_entry[];
extern volatile uint64_t mongoose_boot_slot[8];

int mongoose_handoff(uint64_t entry, uint64_t bootargs, void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	uint32_t old = rd(GICD_BASE + 0x000);

	wr(GICD_BASE + 0x000, old | 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	kv("MONGOOSE BOOT: GICD_CTLR", rd(GICD_BASE + 0x000));
	mongoose_boot_slot[0] = entry;
	mongoose_boot_slot[1] = bootargs;
	dc_civac_range((const void *)mongoose_boot_slot, 64);
	dc_civac_range(mongoose_boot_entry, 512);
	kv("MONGOOSE BOOT: XNU entry", entry);
	kv("MONGOOSE BOOT: boot_args", bootargs);
	out("MONGOOSE BOOT: CPU_ON(0x000)\n");
	fl(flush);
	int64_t rc = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)mongoose_boot_entry, bootargs);

	kv("MONGOOSE BOOT: CPU_ON rc", (uint64_t)rc);
	fl(flush);
	return rc == 0 ? 0 : -1;
}

/* mode 0 = original behaviour; 2 = switch the CPUCL0 clock before CPU_ON; 3 = observable stub */
void mngs_probe_x(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		  void (*flush)(void), unsigned mode)
{
	const int obs = mode >= 3;	/* 3 and 4 both use the observable stub + cache clean */
	const void *entry = obs ? (const void *)smp_entry3 : (const void *)smp_entry;
	out("MNGS probe (Mongoose cluster 0)\n");
	if (mode == 10) {
		mngs_exp10(kv, out, flush);
		return;
	}
	if (mode >= 7) {
		mngs_exp7(kv, out, flush, mode);
		return;
	}
	kv("seq st", rd(PMU + 0x2484));
	kv("seq opt", rd(PMU + 0x2488));
	kv("qch", rd(PMU + 0x3c24));
	kv("clkstop", rd(PMU + 0x45c4));
	kv("dd4", rd(DD(4)));
	kv("cpu0 st", rd(CPU_STAT(0)));
	out("\n");
	if (mode == 5 || mode == 6) {
		uint32_t old500 = rd(PMU + 0x500);
		/*
		 * mngsexp=6 = 5 plus PMU 0x500 := 0xF01 first (Linux dfd_set_dump_gpr(1): DFD_EDPCSR_DUMP_EN | the four
		 * L2/DBGL1 reset-disable enables), the bit S-Boot's big-core path tests; the old value is restored afterwards.
		 *
		 * mngsexp=5: exactly what S-Boot does to start a big core (analysis/sboot-mngs-bringup.md, its crash-dump routine
		 * at file offset 0x75a08): read PMU 0x500 and 0x420, 0x2440 |= 0x30000, then one plain PSCI CPU_ON (x1 = MPIDR,
		 * x3 = 0). No QCH/CLKSTOP/DD/0x2488 writes and no CONFIGURATION (0x2000) toggling around it.
		 */
		out("MEXP: exp5/6 S-Boot's own MNGS sequence\n");
		if (mode == 6) {
			wr(PMU + 0x500, 0xf01);
			kv("MEXP: pmu 0x500 set to", rd(PMU + 0x500));
		}
		kv("MEXP: pmu 0x500", rd(PMU + 0x500));
		kv("MEXP: pmu 0x420", rd(PMU + 0x420));
		kv("MEXP: pmu 0x2440 before", rd(PMU + 0x2440));
		wr(PMU + 0x2440, rd(PMU + 0x2440) | 0x30000);
		kv("MEXP: pmu 0x2440 after", rd(PMU + 0x2440));
		for (unsigned i = 0; i < 16; i++)
			smp_flags[i] = 0;
		dc_civac_range(entry, 256);
		dc_civac_range((const void *)smp_flags, 128);
		fl(flush);
		int64_t rc5 = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)entry, 0);
		kv("MEXP: CPU_ON rc", (uint64_t)rc5);
		int up5 = wait_flag_inv(0, 300);
		kv("MONGOOSE STARTED", up5);
		for (unsigned s = 8; s < 11; s++)
			__asm__ volatile("dc ivac, %0; dsb sy" :: "r"(&smp_flags[s]) : "memory");
		kv("MEXP: MIDR_EL1", smp_flags[8]);
		kv("MEXP: CurrentEL", smp_flags[9]);
		kv("MEXP: core0 st", rd(CPU_STAT(0)));
		kv("MEXP: pmu 0x2604 (L2 st)", rd(PMU + 0x2604));
		kv("MEXP: pmu 0x2404 (noncpu st)", rd(PMU + 0x2404));
		kv("MEXP: pmu 0x2484 (seq st)", rd(PMU + 0x2484));
		if (mode == 6) {
			wr(PMU + 0x500, old500);
			kv("MEXP: pmu 0x500 restored", rd(PMU + 0x500));
		}
		out("MNGS probe done (exp5/6)\n");
		fl(flush);
		return;
	}
	if (mode == 4) {
		/*
		 * mngsexp=4: the cluster-0 subset of stock pmucal_lpm_init (analysis/mngs-bringup.md):
		 * the sequencer policy words S-Boot leaves at reset values. Old values are logged so
		 * the effect is visible; nothing outside cluster 0 is touched.
		 */
		out("MEXP: exp4 stock lpm_init policy words, cluster 0\n");
		kv("MEXP: l2 opt 0x2608 before", rd(PMU + 0x2608));
		kv("MEXP: noncpu opt 0x2408 before", rd(PMU + 0x2408));
		for (unsigned c = 0; c < 4; c++)
			kv("MEXP: cpuN opt before", rd(PMU + 0x2008 + c * 0x80));
		wr(PMU + 0x2608, rd(PMU + 0x2608) & ~(1u << 17));
		wr(PMU + 0x2408, (rd(PMU + 0x2408) & ~(0x3fu << 8)) | (0x1fu << 8));
		for (unsigned c = 0; c < 4; c++) {
			uintptr_t a = PMU + 0x2008 + c * 0x80;
			wr(a, rd(a) & ~(1u << 12));
			wr(a, (rd(a) & ~0x3fu) | 0x1fu);
			wr(a, rd(a) | (1u << 28));
		}
		kv("MEXP: l2 opt 0x2608 after", rd(PMU + 0x2608));
		kv("MEXP: noncpu opt 0x2408 after", rd(PMU + 0x2408));
		kv("MEXP: cpu0 opt after", rd(PMU + 0x2008));
		fl(flush);
	}
	wr(PMU + 0x3c20, rd(PMU + 0x3c20) | 1);
	if (!wait_bit(PMU + 0x3c24, 1, 50)) {
		out("MNGS: QCH status stuck\n");
		return;
	}
	wr(PMU + 0x45c0, rd(PMU + 0x45c0) | 1);
	if (!wait_bit(PMU + 0x45c4, 1, 50)) {
		out("MNGS: CLKSTOP status stuck\n");
		return;
	}
	wr(DD(4), rd(DD(4)) | 1);
	wr(PMU + 0x2488, rd(PMU + 0x2488) & ~1u);
	kv("cluster on: seq st", rd(PMU + 0x2484));
	out("\n");
	/*
	 * Boot 31 powered the core first, then CPU_ON: status 0xf, rc 0, the
	 * core never ran our entry. The order that starts the A53s (SweetCPU,
	 * patch 0124): power off, CPU_ON (firmware records the entry), power
	 * 0 -> 0xF edge, CPU_ON again. cpu00_on adds DD0 after the power-up.
	 */
	if (mode == 2 && mngs_switch_clk(kv, out, flush)) {
		out("MEXP: clock switch failed, CPU_ON skipped\n");
		fl(flush);
		return;
	}
	for (unsigned i = 0; i < 16; i++)
		smp_flags[i] = 0;
	if (obs) {
		out("MEXP: exp3 cache clean of stub + flags (A53 control skipped: XNU starts those cores)\n");
		dc_civac_range(entry, 256);
		dc_civac_range((const void *)smp_flags, 128);
		fl(flush);
	}
	wr(DD(0), rd(DD(0)) & ~1u);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) & ~0xfu);
	uint64_t end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf))
		;
	int64_t rc0 = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)entry, 0);
	wr(CPU_CFG(0), rd(CPU_CFG(0)) | 0xf);
	end = ticks() + TIMER_HZ / 10;
	while (ticks() < end && (rd(CPU_STAT(0)) & 0xf) != 0xf)
		;
	wr(DD(0), rd(DD(0)) | 1);
	kv("first CPU_ON rc", (uint64_t)rc0);
	kv("cpu0 st", rd(CPU_STAT(0)));
	kv("dd0", rd(DD(0)));
	out("\n");
	fl(flush);
	int up = obs ? wait_flag_inv(0, 100) : wait_flag(0, 100);
	int64_t rc = 0;
	if (!up) {
		rc = smc3(PSCI_CPU_ON, 0x000, (uintptr_t)entry, 0);
		up = obs ? wait_flag_inv(0, 300) : wait_flag(0, 300);
	}
	kv("CPU_ON rc", (uint64_t)rc);
	kv("MONGOOSE STARTED", up);
	if (obs) {
		for (unsigned s = 8; s < 11; s++)
			__asm__ volatile("dc ivac, %0; dsb sy" :: "r"(&smp_flags[s]) : "memory");
		kv("MEXP: MIDR_EL1", smp_flags[8]);
		kv("MEXP: CurrentEL", smp_flags[9]);
		kv("MEXP: MPIDR_EL1", smp_flags[10]);
		kv("MEXP: core0 st", rd(CPU_STAT(0)));
		kv("MEXP: noncpu st", rd(PMU + 0x2404));
		kv("MEXP: l2 st", rd(PMU + 0x2604));
		kv("MEXP: seq st", rd(PMU + 0x2484));
		/* release: the stub (if it ever runs) powers itself off, now or later */
		smp_flags[11] = 1;
		__asm__ volatile("dc civac, %0; dsb sy" :: "r"(&smp_flags[11]) : "memory");
		uint64_t e2 = ticks() + TIMER_HZ / 20;
		while (ticks() < e2)
			;
		kv("MEXP: core0 st after release", rd(CPU_STAT(0)));
	}
	out("\nMNGS probe done\n");
	fl(flush);
}

void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	static const uint64_t targets[] = { 0x101, 0x000 };
	uint64_t self = 0;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(self));
	out("SMP probe\n");
	kv("boot mpidr", self & 0xffffff);
	regs(kv, self & 0xffffff);
	out("\n");
	for (unsigned t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
		uint64_t m = targets[t];
		unsigned slot = (((m >> 8) & 1) << 2) | (m & 3);
		smp_flags[slot] = 0;
		kv("target", m);
		regs(kv, m);
		out("\n");
		int64_t rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		int up = wait_flag(slot, 300);
		kv(" CPU_ON rc", (uint64_t)rc);
		kv("started", up);
		regs(kv, m);
		out("\n");
		if (up)
			continue;
		/* Power the core (and its cluster sequencer) through the PMU. */
		unsigned c = (m >> 8) & 0xf;
		wr(CL_CPUSEQ(c), rd(CL_CPUSEQ(c)) & ~1u);
		wr(CPU_CFG(m), rd(CPU_CFG(m)) | 0xf);
		uint64_t end = ticks() + TIMER_HZ / 10;
		while (ticks() < end && (rd(CPU_STAT(m)) & 0xf) != 0xf)
			;
		out(" after PMU power-up:");
		regs(kv, m);
		out("\n");
		rc = smc3(PSCI_CPU_ON, m, (uintptr_t)smp_entry, slot);
		up = wait_flag(slot, 300);
		kv(" CPU_ON rc", (uint64_t)rc);
		kv("started", up);
		regs(kv, m);
		out("\n");
	}
	out("SMP probe done\n");
}
/* ==== BEGIN haptic.c (copy of tools/haptic/haptic.c; edit THERE and re-run tools/haptic/_append.sh) ==== */
/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Item #13 haptics: make the Galaxy S8 (SM-G950F, dreamlte) vibration motor buzz from the loader (MMU off).
 *
 * Hardware path, every value traced to Samsung's own kernel (ref/s8kernel-tgp-p), see tools/haptic/HAPTIC-PLAN.md:
 *   motor driver IC = MAX77865 (its haptic block), I2C slave 0x66 on hsi2c_32 = 0x10970000 (same bus as the
 *   battery fuel gauge 0x36 / charger 0x69 that touch_probe.c battery_probe() already uses),
 *   motor input = Exynos8895 PWM timer channel 1 (PWM block 0x104c0000, TOUT1 = gpd0-5 function 2) at 20.48 kHz;
 *   the MAX77865 divides it by 128 (MCONFIG PDIV) = 160 Hz LRA drive, duty > 50 % = amplitude.
 *   Drivers: drivers/motor/max77865_haptic.c, drivers/pwm/pwm-samsung.c, dts exynos8895-dreamlte_motor.dtsi.
 *   NOT touched: the S2MPS17 PMIC (SPEEDY). The driver's "VDD_MOTOR_3P3" regulator is commented out in
 *   max77865_haptic.c (motor_vdd_en is dead code); the motor supply is the MAX77865 boost (BOOSTCONTROL1/2).
 *
 * Every register write is logged with its read-back so a no-buzz run shows which step failed. Every exit path
 * (success or error) switches the motor off (MEN=0), stops the timer, releases the pin and restores the
 * registers it changed.
 *
 * Built like the rest of the loader: MMU off, -mgeneral-regs-only -mstrict-align, no libc, no pointer tables.
 * All names carry the hp_/HP_ prefix so this file can be appended to smp_probe.c unchanged
 * (tools/haptic/_append.sh).
 */
#include <stdint.h>

#define HP_TIMER_HZ_DEFAULT	26000000UL

/* ---- hsi2c_32 (USI13, second HSI2C) = MAX77865; same bring-up as touch_probe.c battery_probe() ---- */
#define HP_CMU_PERIC1		0x10800000UL
#define HP_P1_USI13_USER_MUX	(HP_CMU_PERIC1 + 0x02c0)	/* bit 4 = SEL (0 = OSCCLK, 26 MHz) */
#define HP_P1_USI13_PCLK	(HP_CMU_PERIC1 + 0x20cc)
#define HP_P1_USI13_SCLK	(HP_CMU_PERIC1 + 0x20d0)
#define HP_GATE_ON		((1u << 21) | (1u << 20))	/* MANUAL | CG_VALUE */
#define HP_SYSREG_USI13		0x1082102cUL
#define HP_USI_V1_UART_I2C1	0xau
#define HP_GPE5			(0x10980000UL + 0x0c0)		/* PERIC1 gpe5: CON, DAT, PUD(+8), DRV */
#define HP_HSI2C		0x10970000UL
#define HP_CTL			0x00
#define HP_FIFO_CTL		0x04
#define HP_TRAILIG_CTL		0x08
#define HP_INT_ENABLE		0x20
#define HP_INT_STATUS		0x24
#define HP_FIFO_STATUS		0x30
#define HP_TX_DATA		0x34
#define HP_RX_DATA		0x38
#define HP_CONF			0x40
#define HP_AUTO_CONF		0x44
#define HP_TIMEOUT		0x48
#define HP_TRANS_STATUS		0x50
#define HP_TIMING_FS1		0x60
#define HP_TIMING_FS2		0x64
#define HP_TIMING_FS3		0x68
#define HP_TIMING_SLA		0x6c
#define HP_ADDR			0x70
#define HP_CTL_I2C_MASTER	((1u << 0) | (1u << 3))
#define HP_CTL_RXCHON		(1u << 6)
#define HP_CTL_TXCHON		(1u << 7)
#define HP_CTL_SW_RST		(1u << 31)
#define HP_CONF_AUTO_MODE	(1u << 31)
#define HP_AUTO_READ		(1u << 16)
#define HP_AUTO_STOP		(1u << 17)
#define HP_AUTO_RUN		(1u << 31)
#define HP_INT_DONE		(1u << 7)
#define HP_INT_ERRS		(0xfu << 8)
#define HP_RX_EMPTY		(1u << 24)
#define HP_TX_FULL		(1u << 7)
#define HP_CLKIN		26000000u
#define HP_SCL_HZ		400000u

/* ---- MAX77865 (slave 0x66, 8-bit 0xCC) registers, include/linux/mfd/max77865-private.h ---- */
#define HP_PM_ADDR		0x66
#define HP_PM_ID1		0x00
#define HP_PM_REV		0x01
#define HP_PM_MAINCTRL1		0x02	/* bit 7 BIASEN */
#define HP_PM_MCONFIG		0x10	/* bit 7 LRA, bit 6 MEN, bit 5 EXT_PWM(0), bits 1:0 PDIV (10 = /128) */
#define HP_PM_BOOST1		0x4c	/* bits 1:0 BSTOUT */
#define HP_PM_BOOST2		0x4f	/* bit 3 FORCE_EN */
#define HP_BIASEN		0x80
#define HP_MOTOR_LRA		0x80
#define HP_MOTOR_EN		0x40
#define HP_PDIV_MASK		0x03
#define HP_PDIV_128		0x02
#define HP_BSTOUT_MASK		0x03
#define HP_FORCE_EN		0x08

/* ---- Exynos8895 PWM timer block (dts sec_pwm 0x104c0000), pwm-samsung.c: channel 1 ---- */
#define HP_PWM			0x104c0000UL
#define HP_TCFG0		(HP_PWM + 0x00)		/* bits 7:0 prescaler 0 (timers 0,1) */
#define HP_TCFG1		(HP_PWM + 0x04)		/* bits 7:4 mux of timer 1 (0 = /1) */
#define HP_TCON			(HP_PWM + 0x08)
#define HP_TCNTB1		(HP_PWM + 0x18)		/* 0x0c + chan * 0xc */
#define HP_TCMPB1		(HP_PWM + 0x1c)
#define HP_TCNTO1		(HP_PWM + 0x20)
#define HP_T1_START		(1u << 8)		/* TCON channel index 2 for timer 1 (gap after timer 0) */
#define HP_T1_MANUAL		(1u << 9)
#define HP_T1_INVERT		(1u << 10)
#define HP_T1_RELOAD		(1u << 11)
#define HP_T1_MASK		(0xfu << 8)
#define HP_CMU_PERIC0_QCH_PWM	0x10403010UL		/* cal-if cmucal-sfr.c QCH_CON_PWM_QCH: bit0 ENABLE, bit1 CLOCK_REQ */
#define HP_GPD0			0x104d0000UL		/* PERIC0 pinctrl, bank gpd0 at +0x000: CON, DAT(+4), PUD(+8) */

/* Samsung's alert waveform (dreamlte_motor.dtsi): period 48828 ns = 20.48 kHz (/128 = 160 Hz), duty 31738 ns = 65 % */
#define HP_PERIOD_NS		48828u
#define HP_DUTY_MAX_NS		31738u

static void (*hp_kv)(const char *, uint64_t);
static void (*hp_out)(const char *);
static uint64_t hp_hz = HP_TIMER_HZ_DEFAULT;

static inline uint32_t hp_rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void hp_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t hp_hr(unsigned o) { return hp_rd(HP_HSI2C + o); }
static inline void hp_hw(unsigned o, uint32_t v) { hp_wr(HP_HSI2C + o, v); }

static uint64_t hp_ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void hp_delay_ms(unsigned ms)
{
	uint64_t end = hp_ticks() + hp_hz / 1000 * ms;
	while (hp_ticks() < end)
		;
}

/* write an MMIO register, log "name: old -> new -> readback" */
static void hp_mmio_w(const char *name, uintptr_t a, uint32_t v)
{
	uint32_t old = hp_rd(a);
	hp_wr(a, v);
	hp_out("HAPTIC mmio ");
	hp_kv(name, a);
	hp_kv("old", old);
	hp_kv("wr", v);
	hp_kv("rb", hp_rd(a));
	hp_out("\n");
}

/* ---- HSI2C (i2c-exynos5.c, Exynos8895 fast mode), copy of touch_probe.c with the bus fixed to 0x10970000 ---- */
static void hp_set_timing(void)
{
	unsigned ftl = (hp_hr(HP_CONF) >> 16) & 7;
	unsigned temp = HP_CLKIN / HP_SCL_HZ - (ftl + 3) * 2;
	unsigned div = temp / 512;
	unsigned cyc = (temp + ((ftl + 3) % (div + 1)) * 2) / (div + 1) - 2;
	unsigned l = cyc * 64 / 100, h = cyc - l;
	hp_hw(HP_TIMING_FS1, l << 24 | l << 16 | l << 8);
	hp_hw(HP_TIMING_FS2, (l / 2) << 24 | l << 8 | h);
	hp_hw(HP_TIMING_FS3, div << 16 | cyc);
	hp_hw(HP_TIMING_SLA, l / 2);
}

static void hp_i2c_reset(void)
{
	hp_hw(HP_CTL, hp_hr(HP_CTL) | HP_CTL_SW_RST);
	hp_hw(HP_CTL, hp_hr(HP_CTL) & ~HP_CTL_SW_RST);
	hp_set_timing();
	hp_hw(HP_TIMEOUT, hp_hr(HP_TIMEOUT) & ~(1u << 31));
	hp_hw(HP_CTL, HP_CTL_I2C_MASTER);
	hp_hw(HP_TRAILIG_CTL, 0xf);
	hp_hw(HP_CONF, hp_hr(HP_CONF) | HP_CONF_AUTO_MODE);
}

static uint32_t hp_wait_done(void)
{
	uint64_t end = hp_ticks() + hp_hz / 20;
	for (;;) {
		uint32_t s = hp_hr(HP_INT_STATUS);
		if (s & (HP_INT_DONE | HP_INT_ERRS)) {
			hp_hw(HP_INT_STATUS, s);
			return (s & HP_INT_ERRS) ? s : 0;
		}
		if (hp_ticks() > end)
			return 0x80000000u | hp_hr(HP_TRANS_STATUS);
	}
}

/* write wlen bytes, then (repeated start) read rlen bytes; 0 = ok */
static uint32_t hp_xfer(unsigned dev, const uint8_t *w, unsigned wlen, uint8_t *r, unsigned rlen)
{
	uint32_t st;
	hp_hw(HP_INT_STATUS, hp_hr(HP_INT_STATUS));
	hp_hw(HP_ADDR, (dev & 0x3ff) << 10);
	hp_hw(HP_INT_ENABLE, HP_INT_DONE | HP_INT_ERRS);
	if (wlen) {
		hp_hw(HP_FIFO_CTL, 3);
		hp_hw(HP_CTL, HP_CTL_I2C_MASTER | HP_CTL_TXCHON);
		hp_hw(HP_AUTO_CONF, (rlen ? 0 : HP_AUTO_STOP) | wlen | HP_AUTO_RUN);
		for (unsigned i = 0; i < wlen; i++) {
			uint64_t end = hp_ticks() + hp_hz / 100;
			while ((hp_hr(HP_FIFO_STATUS) & HP_TX_FULL) && hp_ticks() < end)
				;
			hp_hw(HP_TX_DATA, w[i]);
		}
		if ((st = hp_wait_done()))
			goto bad;
	}
	if (rlen) {
		hp_hw(HP_FIFO_CTL, 3);
		hp_hw(HP_CTL, HP_CTL_I2C_MASTER | HP_CTL_RXCHON);
		hp_hw(HP_AUTO_CONF, HP_AUTO_READ | HP_AUTO_STOP | rlen | HP_AUTO_RUN);
		for (unsigned i = 0; i < rlen; i++) {
			uint64_t end = hp_ticks() + hp_hz / 20;
			while ((hp_hr(HP_FIFO_STATUS) & HP_RX_EMPTY) && hp_ticks() < end)
				;
			if (hp_hr(HP_FIFO_STATUS) & HP_RX_EMPTY) {
				st = 0x40000000u | i;
				goto bad;
			}
			r[i] = (uint8_t)hp_hr(HP_RX_DATA);
		}
		if ((st = hp_wait_done()))
			goto bad;
	}
	hp_hw(HP_CTL, HP_CTL_I2C_MASTER);
	return 0;
bad:
	hp_i2c_reset();
	return st;
}

/* one PMIC register read, with one retry; 0 = ok */
static uint32_t hp_pm_rd(uint8_t reg, uint8_t *v)
{
	uint32_t st = hp_xfer(HP_PM_ADDR, &reg, 1, v, 1);
	if (st)
		st = hp_xfer(HP_PM_ADDR, &reg, 1, v, 1);
	return st;
}

/* write + read back + log. returns 0 if the transfer worked (a read-back mismatch is logged, not an error) */
static uint32_t hp_pm_wr(const char *name, uint8_t reg, uint8_t val)
{
	uint8_t d[2] = { reg, val }, rb = 0xee;
	uint32_t st = hp_xfer(HP_PM_ADDR, d, 2, 0, 0);
	if (st)
		st = hp_xfer(HP_PM_ADDR, d, 2, 0, 0);
	uint32_t st2 = st ? 0 : hp_pm_rd(reg, &rb);
	hp_out("HAPTIC pmic wr ");
	hp_kv(name, reg);
	hp_kv("val", val);
	hp_kv("st", st);
	hp_kv("rb", st2 ? 0xdead0000u | (st2 & 0xffff) : rb);
	hp_out("\n");
	return st | st2;
}

/* max77865_update_reg(): reg = (reg & ~mask) | (val & mask); *old gets the value before */
static uint32_t hp_pm_upd(const char *name, uint8_t reg, uint8_t val, uint8_t mask, uint8_t *old)
{
	uint8_t cur = 0;
	uint32_t st = hp_pm_rd(reg, &cur);
	if (st) {
		hp_out("HAPTIC pmic upd read failed ");
		hp_kv(name, reg);
		hp_kv("st", st);
		hp_out("\n");
		return st;
	}
	if (old)
		*old = cur;
	return hp_pm_wr(name, reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

/* Reproduce battery_probe()'s clock/pin/SYSREG set-up of USI13 (idempotent), then reset the controller. */
static void hp_i2c_setup(void)
{
	hp_out("HAPTIC step1 i2c bus hsi2c_32 0x10970000\n");
	hp_kv("sysreg", hp_rd(HP_SYSREG_USI13));
	hp_kv("gpe5", hp_rd(HP_GPE5));
	hp_out("\n");
	hp_wr(HP_P1_USI13_PCLK, hp_rd(HP_P1_USI13_PCLK) | HP_GATE_ON);
	hp_wr(HP_P1_USI13_SCLK, hp_rd(HP_P1_USI13_SCLK) | HP_GATE_ON);
	hp_wr(HP_P1_USI13_USER_MUX, hp_rd(HP_P1_USI13_USER_MUX) & ~(1u << 4));
	hp_wr(HP_SYSREG_USI13, (hp_rd(HP_SYSREG_USI13) & ~0xfu) | HP_USI_V1_UART_I2C1);
	hp_wr(HP_GPE5, (hp_rd(HP_GPE5) & ~0xff000000u) | 0x22000000u);
	hp_wr(HP_GPE5 + 8, hp_rd(HP_GPE5 + 8) & ~0xf000u);
	hp_i2c_reset();
}

/* read-only bus diagnosis when the MAX77865 top slave 0x66 does not answer */
static void hp_diag(void)
{
	uint8_t r = 0, b[2] = { 0, 0 };
	uint32_t st;
	r = 0x00;
	st = hp_xfer(0x36, &r, 1, b, 2);	/* fuel gauge STATUS */
	hp_out("HAPTIC diag fuelgauge 0x36 reg0 ");
	hp_kv("st", st);
	hp_kv("val", b[0] | b[1] << 8);
	hp_out("\n");
	r = 0xb2;
	st = hp_xfer(0x69, &r, 1, b, 1);	/* charger INT_OK */
	hp_out("HAPTIC diag charger 0x69 regb2 ");
	hp_kv("st", st);
	hp_kv("val", b[0]);
	hp_out("\n");
}

/* Measure the timer-1 input rate with cntpct so no clock assumption is hard-wired. 0 = timer does not count. */
static uint64_t hp_pwm_measure(void)
{
	uint32_t t, n1, n2;
	uint64_t t0, el;
	/* prescaler 0 = /1, timer 1 mux /1: same as the Linux clock driver leaves it (clk-exynos-pwm.c) */
	hp_wr(HP_TCFG0, hp_rd(HP_TCFG0) & ~0xffu);
	hp_wr(HP_TCFG1, hp_rd(HP_TCFG1) & ~0xf0u);
	hp_wr(HP_TCNTB1, 0x00ffffff);
	hp_wr(HP_TCMPB1, 0);
	t = hp_rd(HP_TCON) & ~HP_T1_MASK;
	hp_wr(HP_TCON, t | HP_T1_MANUAL);
	hp_wr(HP_TCON, t | HP_T1_START);	/* one-shot down counter, no reload, no invert (pin is not muxed yet) */
	t0 = hp_ticks();
	hp_delay_ms(2);
	n1 = hp_rd(HP_TCNTO1);
	el = hp_ticks() - t0;
	n2 = hp_rd(HP_TCNTO1);
	hp_wr(HP_TCON, t);			/* stop */
	hp_out("HAPTIC pwm measure ");
	hp_kv("tcnto@2ms", n1);
	hp_kv("tcnto+", n2);
	hp_kv("cnt_ticks", el);
	hp_out("\n");
	if (n1 >= 0x00ffffffu || n1 == 0)
		return 0;
	return (uint64_t)(0x00ffffffu - n1) * hp_hz / el;
}

void haptic_pulse(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void),
		  unsigned ms, unsigned strength)
{
	uint8_t id1 = 0, rev = 0, o_main = 0, o_mcfg = 0, o_b1 = 0, o_b2 = 0, t = 0;
	uint32_t o_qch = 0, o_tcon = 0, o_tcfg0 = 0, o_tcfg1 = 0, o_gcon = 0, o_gpud = 0;
	int pm_touched = 0, pwm_touched = 0, pin_touched = 0, qch_touched = 0, boost_touched = 0;
	uint64_t rate;
	uint32_t period_ns = HP_PERIOD_NS, duty_ns, tcnt, tduty, tcmp;

	hp_kv = kv;
	hp_out = out;
	{
		uint64_t f;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
		hp_hz = f ? f : HP_TIMER_HZ_DEFAULT;
	}
	if (ms > 300)
		ms = 300;
	if (!ms)
		ms = 150;
	if (strength > 10000)
		strength = 10000;
	out("\nHAPTIC pulse (MAX77865 0x66 @hsi2c_32 + PWM1 gpd0-5)\n");
	kv("ms", ms);
	kv("strength", strength);
	kv("cntfrq", hp_hz);
	out("\n");

	/* ---- step 1: bus + identify the MAX77865 (read only) ---- */
	hp_i2c_setup();
	if (hp_pm_rd(HP_PM_ID1, &id1) || hp_pm_rd(HP_PM_REV, &rev)) {
		out("HAPTIC FAIL step1: MAX77865 0x66 does not answer on hsi2c_32\n");
		hp_diag();
		goto done;
	}
	out("HAPTIC step1 ok ");
	kv("PMICID1", id1);
	kv("PMICREV", rev);
	out("\n");

	/* ---- step 2: read-only dump of the registers we are going to change ---- */
	if (hp_pm_rd(HP_PM_MAINCTRL1, &o_main) || hp_pm_rd(HP_PM_MCONFIG, &o_mcfg) ||
	    hp_pm_rd(HP_PM_BOOST1, &o_b1) || hp_pm_rd(HP_PM_BOOST2, &o_b2)) {
		out("HAPTIC FAIL step2: register read\n");
		goto done;
	}
	out("HAPTIC step2 original ");
	kv("MAINCTRL1", o_main);
	kv("MCONFIG", o_mcfg);
	kv("BOOST1", o_b1);
	kv("BOOST2", o_b2);
	out("\n");
	flush();	/* the PWM block access below is the first one that could hang on a gated clock */

	/* ---- step 3: PWM block clock (HWACG QCH) forced on, measure the timer rate ---- */
	out("HAPTIC step3 pwm clock\n");
	o_qch = hp_rd(HP_CMU_PERIC0_QCH_PWM);
	kv("QCH_PWM", o_qch);
	out("\n");
	hp_mmio_w("QCH_PWM", HP_CMU_PERIC0_QCH_PWM, (o_qch & ~0x1u) | 0x2u);	/* ENABLE=0 (manual), CLOCK_REQ=1 */
	qch_touched = 1;
	flush();
	o_tcon = hp_rd(HP_TCON);
	o_tcfg0 = hp_rd(HP_TCFG0);
	o_tcfg1 = hp_rd(HP_TCFG1);
	out("HAPTIC step3 pwm original ");
	kv("TCON", o_tcon);
	kv("TCFG0", o_tcfg0);
	kv("TCFG1", o_tcfg1);
	out("\n");
	pwm_touched = 1;
	rate = hp_pwm_measure();
	kv("tin_hz", rate);
	out("\n");
	if (rate < 1000000 || rate > 400000000) {
		out("HAPTIC FAIL step3: PWM timer 1 does not count (or absurd rate); clock gate / base address wrong\n");
		goto done;
	}

	/* ---- step 4: pin gpd0-5 = TOUT1 (function 2, pull-down) ---- */
	out("HAPTIC step4 pinmux gpd0-5\n");
	o_gcon = hp_rd(HP_GPD0);
	o_gpud = hp_rd(HP_GPD0 + 8);
	kv("gpd0 con", o_gcon);
	kv("gpd0 dat", hp_rd(HP_GPD0 + 4));
	kv("gpd0 pud", o_gpud);
	out("\n");
	pin_touched = 1;
	hp_mmio_w("gpd0.PUD", HP_GPD0 + 8, (o_gpud & ~(3u << 10)) | (1u << 10));
	hp_mmio_w("gpd0.CON", HP_GPD0, (o_gcon & ~(0xfu << 20)) | (2u << 20));

	/* ---- step 5: MAX77865 haptic block + boost (max77865_haptic_init_reg, max77865_motor_boost_control ON) ---- */
	out("HAPTIC step5 max77865 init\n");
	pm_touched = 1;
	if (hp_pm_upd("MAINCTRL1.BIASEN", HP_PM_MAINCTRL1, HP_BIASEN, HP_BIASEN, 0)) {
		out("HAPTIC FAIL step5: MAINCTRL1\n");
		goto done;
	}
	if (hp_pm_upd("MCONFIG.MEN=0", HP_PM_MCONFIG, 0, HP_MOTOR_EN, 0) ||
	    hp_pm_upd("MCONFIG.LRA", HP_PM_MCONFIG, HP_MOTOR_LRA, HP_MOTOR_LRA, 0) ||
	    hp_pm_upd("MCONFIG.PDIV=128", HP_PM_MCONFIG, HP_PDIV_128, HP_PDIV_MASK, 0)) {
		out("HAPTIC FAIL step5: MCONFIG\n");
		goto done;
	}
	boost_touched = 1;
	if (hp_pm_upd("BOOST1.BSTOUT=0", HP_PM_BOOST1, 0, HP_BSTOUT_MASK, 0) ||
	    hp_pm_upd("BOOST2.FORCE_EN", HP_PM_BOOST2, HP_FORCE_EN, HP_FORCE_EN, 0)) {
		out("HAPTIC FAIL step5: boost\n");
		goto done;
	}
	hp_delay_ms(6);		/* the driver sleeps 6 ms after enabling the supply */

	/* ---- step 6: PWM 20.48 kHz, duty = 50 % + (65 % - 50 %) * strength (pwm_samsung_config()+enable()) ---- */
	out("HAPTIC step6 pwm start\n");
	duty_ns = period_ns / 2 + (uint32_t)(((uint64_t)(HP_DUTY_MAX_NS - period_ns / 2) * strength) / 10000);
	tcnt = (uint32_t)((rate * period_ns + 500000000ull) / 1000000000ull);
	tduty = (uint32_t)(((uint64_t)tcnt * duty_ns + period_ns / 2) / period_ns);
	tcmp = tcnt - tduty;
	if (tcmp == tcnt)
		tcmp--;	/* driver: the compare is only checked after a decrement */
	kv("period_ns", period_ns);
	kv("duty_ns", duty_ns);
	kv("tcnt", tcnt);
	kv("tduty", tduty);
	out("\n");
	if (tcnt < 8) {
		out("HAPTIC FAIL step6: period too short for this timer rate\n");
		goto done;
	}
	hp_mmio_w("TCNTB1", HP_TCNTB1, tcnt - 1);	/* hardware counts one hidden tick (driver: --tcnt, --tcmp) */
	hp_mmio_w("TCMPB1", HP_TCMPB1, tcmp - 1);
	{
		uint32_t tcon = (hp_rd(HP_TCON) & ~HP_T1_MASK) | HP_T1_INVERT;	/* driver sets INVERT in pwm_samsung_init() */
		hp_mmio_w("TCON.manual", HP_TCON, tcon | HP_T1_MANUAL);
		hp_mmio_w("TCON.start", HP_TCON, tcon | HP_T1_RELOAD | HP_T1_START);
	}
	hp_delay_ms(1);
	kv("TCNTO1 a", hp_rd(HP_TCNTO1));
	kv("TCNTO1 b", hp_rd(HP_TCNTO1));
	kv("gpd0 dat", hp_rd(HP_GPD0 + 4));
	out("\n");

	/* ---- step 7: motor on, hold, motor off (haptic_enable / max77865_haptic_i2c) ---- */
	out("HAPTIC step7 MOTOR ON\n");
	flush();	/* the log must not depend on the pulse surviving */
	if (hp_pm_upd("MCONFIG.MEN=1", HP_PM_MCONFIG, HP_MOTOR_EN, HP_MOTOR_EN, 0)) {
		out("HAPTIC FAIL step7: MEN\n");
		goto done;
	}
	hp_delay_ms(ms / 2);
	kv("TCNTO1 mid", hp_rd(HP_TCNTO1));
	if (!hp_pm_rd(HP_PM_MCONFIG, &t))
		kv("MCONFIG mid", t);
	hp_delay_ms(ms - ms / 2);
	out("\nHAPTIC pulse finished\n");

done:
	/* ---- cleanup, runs on every path: motor off first, then timer, pin, boost, registers ---- */
	out("HAPTIC cleanup\n");
	if (pm_touched) {
		(void)hp_pm_upd("MCONFIG.MEN=0", HP_PM_MCONFIG, 0, HP_MOTOR_EN, 0);
		(void)hp_pm_wr("MCONFIG.restore", HP_PM_MCONFIG, (uint8_t)(o_mcfg & ~HP_MOTOR_EN));
		(void)hp_pm_wr("MAINCTRL1.restore", HP_PM_MAINCTRL1, o_main);
	}
	if (pwm_touched) {
		hp_mmio_w("TCNTB1.idle", HP_TCNTB1, 0);
		hp_mmio_w("TCMPB1.idle", HP_TCMPB1, 0);
		hp_mmio_w("TCON.restore", HP_TCON, (hp_rd(HP_TCON) & ~HP_T1_MASK) | (o_tcon & HP_T1_MASK & ~(HP_T1_START | HP_T1_RELOAD)));
		hp_wr(HP_TCFG0, o_tcfg0);
		hp_wr(HP_TCFG1, o_tcfg1);
	}
	if (pin_touched) {
		hp_mmio_w("gpd0.CON.restore", HP_GPD0, o_gcon);
		hp_mmio_w("gpd0.PUD.restore", HP_GPD0 + 8, o_gpud);
	}
	if (boost_touched) {
		(void)hp_pm_wr("BOOST2.restore", HP_PM_BOOST2, o_b2);
		(void)hp_pm_wr("BOOST1.restore", HP_PM_BOOST1, o_b1);
	}
	if (qch_touched)
		hp_mmio_w("QCH_PWM.restore", HP_CMU_PERIC0_QCH_PWM, o_qch);
	out("HAPTIC done\n");
	flush();
}
/* ==== END haptic.c ==== */
/* ==== BEGIN wifi_w2.c (copy of tools/wifi-w2/wifi_w2.c + wifi_w3.c; edit THERE and re-run tools/wifi-w2/_append.sh) ==== */
/* SPDX-License-Identifier: GPL-2.0-only
 *
 * W2a: talk to the running BCM4361 firmware through the PCIe MSGBUF (pcie_shared v7) protocol from the loader,
 * and make it answer two iovars: 'cur_etheraddr' (MAC) and 'ver' (firmware version string).
 *
 * Design and every field source: tools/wifi-w2/W2-PLAN.md. Reference sources (read, not copied): Samsung's own
 * ref/s8kernel-tgp-p/drivers/net/wireless/bcmdhd4361/{dhd_msgbuf.c,dhd_pcie.c,include/bcmpcie.h,include/bcmmsgbuf.h}
 * and torvalds/linux brcmfmac/{pcie.c,msgbuf.c}.
 *
 * Call it AFTER wifi_link(stage >= 8) in the same boot (wifiprobe=9): the PCIe link, BAR0 (0x11800000, 32 KiB),
 * BAR2/TCM window (0x11808000 -> PCI 0x12000000, backplane address 0 via PCI_BAR1_WIN = 0) and the running firmware
 * are expected to be exactly as wifi_link left them. Nothing here touches the PCIe controller (DBI/ATU/ELBI) except
 * one ELBI read of the link state; chip registers are reached only through BAR0 (PCIe2 core window) and TCM.
 *
 * Built like the rest of the loader: MMU off (all data accesses Device-nGnRnE: strictly ordered, no cache to maintain),
 * -mgeneral-regs-only -mstrict-align, no libc, no pointer tables in data (no ABS64 relocations).
 * All helpers carry the w2_/W2_ prefix so this file can be appended to smp_probe.c unchanged.
 */
#include <stdint.h>

/* ---- CPU-side addresses fixed by wifi_link (analysis/wifi-w0-sequence.md) ---- */
#define W2_ELBI		0x116A0000UL	/* ELBI: +0x74 link state (read only here) */
#define W2_CFG		0x11BFF000UL	/* outbound CFG0 window to the chip's config space (+0x80 = BAR0 window) */
#define W2_BAR0		0x11800000UL	/* chip BAR0: 4 KiB backplane window at config 0x80 */
#define W2_TCM		0x11808000UL	/* chip BAR2: TCM / RAM, offset = backplane address */
#define W2_RAMB		0x170000u	/* RAM slave of the ARM CR4 (EROM): 0x170000 + 0x150000 */
#define W2_RAME		0x2C0000u
#define W2_PCIE2_CORE	0x18003000u	/* PCIE2 core registers (EROM: core 0x83c rev 23, slave port 0) */
#define W2_TIMER_HZ	26000000UL

/* ---- PCIe2 core register offsets, corerev 23 (< 64): bcmdhd pcie_core.h PCIH2D_MailBox/PCIH2D_DB1/PCIMailBoxInt ---- */
#define W2_REG_MBINT	0x48u	/* PCIMailBoxInt: D2H doorbell/mailbox status (read only here) */
#define W2_REG_MBMASK	0x4Cu	/* PCIMailBoxMask (read only here: we poll, no interrupt is enabled) */
#define W2_REG_DB0	0x140u	/* H2D mailbox 0 = ring doorbell (value is ignored by the firmware; DHD writes 0x12345678) */
#define W2_REG_DB1	0x144u	/* H2D doorbell 1 = "host ready" (DHD: 0x12345678, Linux: 1) */
#define W2_DB_VAL	0x12345678u

/* ---- pcie_shared_t word offsets (bytes): bcmpcie.h pciedev_shared_t, cross-checked with W1b log and Linux pcie.c ---- */
#define SH_FLAGS	0
#define SH_TRAP_ADDR	4
#define SH_ASSERT_EXP	8
#define SH_ASSERT_FILE	12
#define SH_ASSERT_LINE	16
#define SH_CONSOLE	20
#define SH_FWID		28
#define SH_MAXRXBUFS	32	/* u16 total_lfrag_pkt_cnt, u16 max_host_rxbufs at 34 */
#define SH_DMA_RXOFF	36
#define SH_H2D_MB	40
#define SH_D2H_MB	44
#define SH_RINGINFO	48
#define SH_SCR_LEN	52
#define SH_SCR_ADDR	56	/* sh_addr_t: low, high */
#define SH_FLAGS2	80
#define SH_HOSTCAP	84
#define SH_WORDS	27	/* read 0..104 */

/* PCIE_SHARED_* feature flags the firmware advertises (bcmpcie.h) */
#define F_VERSION_MASK	0x000000FFu
#define F_ASSERT	0x00000200u
#define F_TRAP		0x00000400u
#define F_DMA_INDEX	0x00010000u
#define F_D2H_SEQNUM	0x00020000u
#define F_D2H_XORCSUM	0x00040000u
#define F_IDLE_FLOW	0x00080000u
#define F_2BYTE_IDX	0x00100000u
#define F_IDMA		0x00400000u
#define F_IFRM		0x01000000u
#define F_USE_MAILBOX	0x02000000u
#define F_HOSTRDY	0x10000000u
#define F_NO_OOB_DW	0x20000000u
#define F_INBAND_DS	0x40000000u
#define F_DAR		0x80000000u

/* host_cap word the host writes (bcmpcie.h HOSTCAP_*): API version 7 | H2D_ENABLE_HOSTRDY | DS_NO_OOB_DW | UR_FW_NO_TRAP.
 * No VALID_PHASE, no IDMA/IFRM/DAR, no extended trap data: the minimal set DHD uses on this chip without those features. */
#ifndef W2_HOSTCAP
#define W2_HOSTCAP	(0x07u | 0x00000400u | 0x00001000u | 0x00800000u)
#endif

/* ---- the host RAM the chip DMAs to/from: loader RAM, low bank, MMU off ----
 * ASSUMPTION A1: PCI address == CPU physical address (no inbound ATU, no IOMMU on pcie0; Samsung's driver only programs
 * outbound regions and its bcmdhd sets a 64-bit DMA mask on plain physical addresses). 0x8c000000 is above the S-Boot
 * initrd (0x89000000..0x8a000000) and DTB (0x8a000000) in the low bank and nothing else of the loader lives there (A2). */
#ifndef W2_DMA_BASE
#define W2_DMA_BASE	0x8c000000UL
#endif
#define L_CTRL_SUB	0x00000u	/* ring 0: H2D control submit, 64 x 40 */
#define L_RXPOST	0x01000u	/* ring 1: H2D rx post, 512 x 32 (0x4000) */
#define L_CTRL_CPL	0x05000u	/* ring 2: D2H control complete, 64 x 24 */
#define L_TX_CPL	0x06000u	/* ring 3: D2H tx complete, 1024 x 24 (0x6000) */
#define L_RX_CPL	0x0C000u	/* ring 4: D2H rx complete, 512 x 40 (0x5000) */
#define L_SCRATCH	0x11000u	/* D2H DMA scratch (8 bytes used) */
#define L_IOCT_REQ	0x12000u	/* ioctl request payload (the firmware DMA-reads it) */
#define L_RESP0		0x14000u	/* ioctl response buffers, 8192 bytes each */
#define L_RESP1		0x16000u
#define L_EVENT		0x18000u	/* W3c: 8 event buffers of 2048 bytes (DHD_FLOWRING_RX_BUFPOST_PKTSZ) */
#define L_TOTAL		0x1C000u
#define IOCT_REQ_MAX	2048u
#define RESP_SZ		8192u
#define PKTID_RESP0	0x101u
#define PKTID_RESP1	0x102u
#define IOCTL_REQ_PKTID	0xFFFEu		/* DHD_IOCTL_REQ_PKTID */

/* message types (bcmmsgbuf.h) */
#define MT_GEN_STATUS		0x01
#define MT_RING_STATUS		0x02
#define MT_IOCTLPTR_REQ		0x09
#define MT_IOCTLPTR_REQ_ACK	0x0A
#define MT_IOCTLRESP_BUF_POST	0x0B
#define MT_IOCTL_CMPLT		0x0C
#define MT_WL_EVENT		0x0E
#define WLC_GET_VAR		262u

#define EPOCH_MODULO		253u
#define EPOCH_INIT		(EPOCH_MODULO + 1)

struct w2_ring {
	uint32_t id;
	uint32_t depth;
	uint32_t ilen;
	uintptr_t host;		/* host (CPU == PCI) address of the ring memory */
	uint32_t widx;		/* TCM address of this ring's write index (16-bit value in a 32-bit slot) */
	uint32_t ridx;		/* TCM address of this ring's read index */
	uint32_t wr;		/* host copy: H2D = our write pointer, D2H = unused */
	uint32_t rd;		/* host copy: D2H = our read pointer */
	uint32_t seq;		/* H2D epoch counter / D2H expected epoch counter */
};

static void (*w2_kv)(const char *, uint64_t);
static void (*w2_out)(const char *);
static void (*w2_flush)(void);
static struct w2_ring w2_rg[5];
static uint32_t w2_sh;		/* TCM address of pcie_shared */
static uint32_t w2_trans;	/* ioctl transaction id */
static uint32_t w2_nitems;	/* D2H items logged so far */

/* ---- primitive access ---- */
static inline uint32_t w2_rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void w2_wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint16_t w2_rd16(uintptr_t a) { return *(volatile uint16_t *)a; }
static inline void w2_wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline void w2_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint32_t w2_t32(uint32_t o) { return w2_rd(W2_TCM + o); }
static void w2_tw32(uint32_t o, uint32_t v) { w2_wr(W2_TCM + o, v); }
static uint16_t w2_t16(uint32_t o) { return w2_rd16(W2_TCM + o); }
static void w2_tw16(uint32_t o, uint16_t v) { w2_wr16(W2_TCM + o, v); }

static uint64_t w2_ticks(void)
{
	uint64_t v;

	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void w2_us(uint32_t us)
{
	uint64_t end = w2_ticks() + (W2_TIMER_HZ / 1000000) * (uint64_t)us;

	while (w2_ticks() < end)
		;
}

static uint32_t w2_ms_since(uint64_t t0)
{
	return (uint32_t)((w2_ticks() - t0) / (W2_TIMER_HZ / 1000));
}

static void w2_fl(void)
{
	if (w2_flush)
		w2_flush();
}

static void w2_k(const char *n, uint64_t v)
{
	w2_kv(n, v);
}

static int w2_ram_ptr(uint32_t p)
{
	return p >= W2_RAMB && p < W2_RAME && !(p & 3);
}

/* print n bytes of text from a host RAM buffer, 64 characters per out() call, non-printables as '|' */
static void w2_text(uintptr_t a, uint32_t n)
{
	char line[65];
	uint32_t j = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = *(volatile uint8_t *)(a + i);

		line[j++] = (c >= 32 && c < 127) ? (char)c : '|';
		if (j == 64) {
			line[j] = 0;
			w2_out(line);
			j = 0;
		}
	}
	line[j] = 0;
	w2_out(line);
	w2_out("\n");
}

/* print a NUL-terminated string that lives in TCM (assert text), at most 80 bytes */
static void w2_tcm_str(uint32_t o)
{
	char line[81];
	uint32_t j = 0;

	if (o < W2_RAMB || o >= W2_RAME - 84)
		return;
	for (; j < 80; j++) {
		uint32_t w = w2_t32((o + j) & ~3u);
		uint8_t c = (uint8_t)(w >> (8 * ((o + j) & 3)));

		if (!c)
			break;
		line[j] = (c >= 32 && c < 127) ? (char)c : '|';
	}
	line[j] = 0;
	w2_out(line);
	w2_out("\n");
}

/* firmware console (hnd_cons_t: +8 buf, +12 size, +16 write index) from index 'from' to now, at most 1500 bytes */
static void w2_console(uint32_t cons, uint32_t from, const char *title)
{
	uint32_t buf, sz, idx, n, start;
	char line[65];
	uint32_t j = 0, word = 0, wordat = 0xffffffffu;

	if (!w2_ram_ptr(cons))
		return;
	buf = w2_t32(cons + 8);
	sz = w2_t32(cons + 12);
	idx = w2_t32(cons + 16);
	if (!w2_ram_ptr(buf) || sz == 0 || sz > 0x10000 || buf + sz > W2_RAME)
		return;
	w2_k("W2: console write index now", idx);
	n = (idx >= from) ? idx - from : sz;
	if (n > sz)
		n = sz;
	if (n > 1500)
		n = 1500;
	start = (idx + sz - n) % sz;
	w2_out(title);
	for (uint32_t k = 0; k < n; k++) {
		uint32_t p = (start + k) % sz;
		uint32_t wa = (buf + p) & ~3u;
		uint8_t c;

		if (wa != wordat) {
			word = w2_t32(wa);
			wordat = wa;
		}
		c = (uint8_t)(word >> (8 * ((buf + p) & 3)));
		line[j++] = (c >= 32 && c < 127) ? (char)c : '|';
		if (j == 64) {
			line[j] = 0;
			w2_out(line);
			j = 0;
		}
	}
	line[j] = 0;
	w2_out(line);
	w2_out("\n");
}

/* ---- ring plumbing (dhd_msgbuf.c dhd_prot_ring_init / write_complete; Linux pcie.c init_ringbuffers) ---- */
static void w2_ring_setup(struct w2_ring *r, uint32_t id, uint32_t depth, uint32_t ilen, uint32_t off)
{
	r->id = id;
	r->depth = depth;
	r->ilen = ilen;
	r->host = W2_DMA_BASE + off;
	r->wr = 0;
	r->rd = 0;
	r->seq = EPOCH_INIT;
}

/* H2D: build the next item (header word + zeroed body) and return its host address, or 0 when the ring is full */
static uintptr_t w2_h2d_begin(struct w2_ring *r, uint32_t msgtype)
{
	uint32_t rd = w2_t16(r->ridx), nxt = (r->wr + 1) % r->depth;
	uintptr_t it;

	if (rd >= r->depth || nxt == rd) {
		w2_k("W2: H2D ring full or bad read index, ring id", r->id);
		w2_k("W2:   read index", rd);
		return 0;
	}
	it = r->host + (uintptr_t)r->wr * r->ilen;
	for (uint32_t i = 0; i < r->ilen; i += 4)
		w2_wr(it + i, 0);
	/* cmn_msg_hdr: msg_type, if_id = 0, flags = 0 (no phase bit: HOSTCAP_H2D_VALID_PHASE not announced), epoch */
	w2_wr(it, msgtype | ((r->seq % EPOCH_MODULO) << 24));
	r->seq++;
	return it;
}

/* H2D: publish the item: write index into TCM (read back to flush the posted write), then ring doorbell 0 */
static void w2_h2d_commit(struct w2_ring *r)
{
	r->wr = (r->wr + 1) % r->depth;
	w2_dsb();
	w2_tw16(r->widx, (uint16_t)r->wr);
	(void)w2_t16(r->widx);
	w2_dsb();
	w2_wr(W2_BAR0 + W2_REG_DB0, W2_DB_VAL);
	(void)w2_rd(W2_BAR0 + W2_REG_MBINT);	/* a non-posted read from the chip behind the posted doorbell write */
}

/* post one ioctl response buffer (MSG_TYPE_IOCTLRESP_BUF_POST = ioctl_resp_evt_buf_post_msg_t) */
static int w2_post_resp(uint32_t pktid, uint32_t off)
{
	struct w2_ring *r = &w2_rg[0];
	uintptr_t it = w2_h2d_begin(r, MT_IOCTLRESP_BUF_POST);
	uintptr_t b = W2_DMA_BASE + off;

	if (!it)
		return -1;
	w2_wr(it + 4, pktid);		/* request_id */
	w2_wr(it + 8, RESP_SZ);		/* host_buf_len (u16) + reserved */
	w2_wr(it + 16, (uint32_t)b);	/* host_buf_addr low */
	w2_wr(it + 20, (uint32_t)(b >> 32));
	w2_h2d_commit(r);
	return 0;
}

/* D2H: is there an item? returns its host address (or 0). Verifies epoch + XOR checksum like dhd_prot_d2h_sync_xorcsum
 * (the firmware advertises PCIE_SHARED_D2H_SYNC_XORCSUM); a bad item is still returned after 20 ms, with a log line. */
static uintptr_t w2_d2h_peek(struct w2_ring *r)
{
	uint32_t wr = w2_t16(r->widx);
	uintptr_t it;
	uint64_t t0;
	uint32_t x = 1, ep = 0;

	if (wr >= r->depth) {
		w2_k("W2: D2H bad write index, ring id", r->id);
		w2_k("W2:   write index", wr);
		return 0;
	}
	if (wr == r->rd)
		return 0;
	it = r->host + (uintptr_t)r->rd * r->ilen;
	t0 = w2_ticks();
	do {
		x = 0;
		for (uint32_t i = 0; i < r->ilen; i += 4)
			x ^= w2_rd(it + i);
		ep = (w2_rd(it) >> 24) & 0xff;
		if (x == 0 && ep == r->seq % EPOCH_MODULO)
			break;
		w2_us(20);
	} while (w2_ms_since(t0) < 20);
	if (x != 0 || ep != r->seq % EPOCH_MODULO) {
		w2_k("W2: D2H item failed epoch/xor check: epoch", ep);
		w2_k("W2:   expected epoch", r->seq % EPOCH_MODULO);
		w2_k("W2:   xor of all words (0 = ok)", x);
	}
	return it;
}

static void w2_d2h_done(struct w2_ring *r)
{
	r->rd = (r->rd + 1) % r->depth;
	r->seq++;
	w2_dsb();
	w2_tw16(r->ridx, (uint16_t)r->rd);
	(void)w2_t16(r->ridx);
}

struct w2_resp {
	int ack, cmplt;
	int32_t ack_status, status;
	uint32_t resp_len, pktid, trans;
};

static void (*w2_evhook)(uintptr_t item);	/* W3c: called for every MSG_TYPE_WL_EVENT item seen while waiting for an ioctl */
static uint32_t w2_typecnt[64];			/* D2H control message types seen (index = msg_type & 63) */
static uint32_t w2_rep_pktid, w2_rep_off;	/* response buffer to re-post before the next request (DHD re-posts after each completion) */
static int w2_ready;				/* set by wifi_w2 once rings and host-ready are done: wifi_w3 needs it */
static uint32_t w2_cons;			/* firmware console address */

/* one ioctl round trip: command cmd, 'incopy' bytes of payload copied to the request buffer, input_buf_len = inlen,
 * output_buf_len = outlen (reply goes to a posted response buffer, <= 8192); waits up to timeout_ms */
static int w2_ioctl(uint32_t cmd, const char *label, const uint8_t *in, uint32_t incopy, uint32_t inlen,
		    uint32_t outlen, uint32_t timeout_ms, struct w2_resp *rs)
{
	struct w2_ring *rq = &w2_rg[0], *cp = &w2_rg[2];
	uintptr_t it, req = W2_DMA_BASE + L_IOCT_REQ;
	uint64_t t0;
	uint32_t i, trans, rdold;
	int fetched = 0;

	rs->ack = rs->cmplt = 0;
	rs->ack_status = rs->status = 0;
	rs->resp_len = rs->pktid = rs->trans = 0;

	if (w2_rep_pktid) {
		(void)w2_post_resp(w2_rep_pktid, w2_rep_off);
		w2_rep_pktid = 0;
	}
	/* payload, rest of the request buffer zero */
	for (i = 0; i < IOCT_REQ_MAX; i += 4)
		w2_wr(req + i, 0);
	if (incopy > IOCT_REQ_MAX)
		incopy = IOCT_REQ_MAX;
	for (i = 0; i < incopy; i++)
		*(volatile uint8_t *)(req + i) = in[i];
	if (inlen > 2008)	/* DHD MSGBUF_IOCTL_MAX_RQSTLEN: 2048 - 40 */
		inlen = 2008;
	if (outlen > RESP_SZ)
		outlen = RESP_SZ;
	trans = ++w2_trans;
	w2_k("W2: ioctl request, cmd", cmd);
	w2_out("W2:   ");
	w2_out(label);
	w2_out("\n");
	w2_k("W2:   trans_id", trans);
	w2_k("W2:   input_buf_len", inlen);
	w2_k("W2:   output_buf_len", outlen);
	w2_dsb();

	it = w2_h2d_begin(rq, MT_IOCTLPTR_REQ);
	if (!it)
		return -1;
	rdold = w2_t16(rq->ridx);
	w2_wr(it + 4, IOCTL_REQ_PKTID);			/* request_id */
	w2_wr(it + 8, cmd);				/* cmd */
	w2_wr(it + 12, trans | (inlen << 16));		/* trans_id, input_buf_len */
	w2_wr(it + 16, outlen);				/* output_buf_len, rsvd */
	w2_wr(it + 24, (uint32_t)req);			/* host_input_buf_addr low (8-byte aligned) */
	w2_wr(it + 28, (uint32_t)(req >> 32));
	w2_fl();
	w2_h2d_commit(rq);
	w2_k("W2:   H2D ctrl write index now", rq->wr);

	t0 = w2_ticks();
	while (w2_ms_since(t0) < timeout_ms) {
		uintptr_t d;

		if (!fetched && w2_t16(rq->ridx) != rdold) {
			fetched = 1;
			w2_k("W2:   chip consumed the request (H2D read index moved) after us", w2_ms_since(t0) * 1000);
		}
		while ((d = w2_d2h_peek(cp)) != 0) {
			uint32_t w0 = w2_rd(d), type = w0 & 0xff;

			w2_typecnt[type & 63]++;

			if (w2_nitems < 24) {
				w2_nitems++;
				w2_k("W2: D2H ctrl item, type", type);
				w2_k("W2:   words 0,1", ((uint64_t)w2_rd(d + 4) << 32) | w0);
				w2_k("W2:   words 2,3", ((uint64_t)w2_rd(d + 12) << 32) | w2_rd(d + 8));
				w2_k("W2:   words 4,5", ((uint64_t)w2_rd(d + 20) << 32) | w2_rd(d + 16));
			}
			if (type == MT_IOCTLPTR_REQ_ACK) {
				rs->ack = 1;
				rs->ack_status = (int16_t)(w2_rd(d + 8) & 0xffff);
				w2_k("W2:   ioctl ACK, status", (uint64_t)(int64_t)rs->ack_status);
			} else if (type == MT_IOCTL_CMPLT) {
				uint32_t w3 = w2_rd(d + 12);

				rs->cmplt = 1;
				rs->status = (int16_t)(w2_rd(d + 8) & 0xffff);
				rs->resp_len = w3 & 0xffff;
				rs->trans = w3 >> 16;
				rs->pktid = w2_rd(d + 4);
				w2_rep_pktid = rs->pktid == PKTID_RESP1 ? PKTID_RESP1 : PKTID_RESP0;
				w2_rep_off = rs->pktid == PKTID_RESP1 ? L_RESP1 : L_RESP0;
				w2_k("W2:   ioctl COMPLETE, status", (uint64_t)(int64_t)rs->status);
				w2_k("W2:     resp_len", rs->resp_len);
				w2_k("W2:     trans_id", rs->trans);
				w2_k("W2:     response buffer pktid", rs->pktid);
				w2_k("W2:     cmd", w2_rd(d + 16));
			} else if (type == MT_GEN_STATUS || type == MT_RING_STATUS) {
				w2_k("W2:   firmware status message, status", w2_rd(d + 8) & 0xffff);
			} else if (type == MT_WL_EVENT) {
				if (w2_evhook)
					w2_evhook(d);
				else
					w2_out("W2:   (a WLC event arrived while no event handler is installed)\n");
			}
			w2_d2h_done(cp);
			w2_fl();
		}
		if (rs->cmplt)
			return 0;
		w2_us(50);
	}
	w2_k("W2: ioctl TIMEOUT after ms", timeout_ms);
	w2_k("W2:   request consumed by chip", fetched);
	w2_k("W2:   ack seen", rs->ack);
	return -2;
}

/* WLC_GET_VAR style call: iovar 'name' (+ nothing else), input_buf_len = output_buf_len = outlen as in W2a */
static int w2_iovar(const char *name, uint32_t cmd, uint32_t outlen, uint32_t timeout_ms, struct w2_resp *rs)
{
	uint32_t n = 0;

	while (name[n])
		n++;
	return w2_ioctl(cmd, name, (const uint8_t *)name, n + 1, outlen, outlen, timeout_ms, rs);
}

static void w2_dump_state(const char *why)
{
	uint32_t i;

	w2_out("W2: state dump: ");
	w2_out(why);
	w2_out("\n");
	w2_k("W2:   ELBI link state", w2_rd(W2_ELBI + 0x74));
	w2_k("W2:   PCIe2 MAILBOXINT (D2H doorbell/mailbox status)", w2_rd(W2_BAR0 + W2_REG_MBINT));
	w2_k("W2:   pcie_shared flags", w2_t32(w2_sh + SH_FLAGS));
	for (i = 0; i < 5; i++) {
		w2_k("W2:   ring id", i);
		w2_k("W2:     write index (TCM)", w2_t16(w2_rg[i].widx));
		w2_k("W2:     read index (TCM)", w2_t16(w2_rg[i].ridx));
	}
	{
		uint32_t d2h_mb = w2_t32(w2_sh + SH_D2H_MB), h2d_mb = w2_t32(w2_sh + SH_H2D_MB);

		if (w2_ram_ptr(d2h_mb))
			w2_k("W2:   D2H mailbox data (FWHALT = 0x10000000)", w2_t32(d2h_mb));
		if (w2_ram_ptr(h2d_mb))
			w2_k("W2:   H2D mailbox data (pending = still unread by the chip)", w2_t32(h2d_mb));
	}
	{
		uint32_t fl = w2_t32(w2_sh + SH_FLAGS);

		if (fl & F_TRAP) {
			uint32_t ta = w2_t32(w2_sh + SH_TRAP_ADDR);

			w2_out("W2:   FIRMWARE TRAP flag set; trap_t at TCM address:\n");
			w2_k("W2:   trap_addr", ta);
			if (w2_ram_ptr(ta) && ta + 20 * 4 <= W2_RAME)
				for (i = 0; i < 20; i++)	/* trap_t: type, epc, cpsr, spsr, r0-r12, sp, lr, pc (ARM CR4 layout, assumed) */
					w2_k("W2:   trap_t word", w2_t32(ta + 4 * i));
		}
		if (fl & F_ASSERT) {
			w2_out("W2:   FIRMWARE ASSERT flag set: expression / file\n");
			w2_tcm_str(w2_t32(w2_sh + SH_ASSERT_EXP));
			w2_tcm_str(w2_t32(w2_sh + SH_ASSERT_FILE));
			w2_k("W2:   assert line", w2_t32(w2_sh + SH_ASSERT_LINE));
		}
	}
	w2_fl();
}

void wifi_w2_at(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), uint64_t dma_base);

void wifi_w2(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
void wifi_w2(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	wifi_w2_at(kv, out, flush, W2_DMA_BASE);
}

/* dma_base is only checked against W2_DMA_BASE (the L_* layout uses the compile-time constant); override by -DW2_DMA_BASE */
void wifi_w2_at(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void), uint64_t dma_base)
{
	uint32_t sh[SH_WORDS], ri[15], flags, i, cons, cons_from;
	uint32_t ringmem, hw, hr, dw, dr;
	uint32_t maxsub = 0, maxcmp = 0, maxflow = 0, idxsz;
	struct w2_resp rs;
	int rc;
	int got_mac = 0, got_ver = 0;

	w2_kv = kv;
	w2_out = out;
	w2_flush = flush;
	w2_nitems = 0;
	w2_trans = 0x100;

	out("W2a: MSGBUF host side, MAC + version by iovar\n");
	kv("W2: DMA region base (loader RAM, low bank)", W2_DMA_BASE);
	kv("W2: DMA region size", L_TOTAL);
	if (dma_base != W2_DMA_BASE)
		out("W2: note: dma_base argument differs from the compiled W2_DMA_BASE, the compiled value is used\n");
	w2_fl();

	/* ---- 1. sanity: link, EP command, find pcie_shared ---- */
	kv("W2: ELBI link state (0x74)", w2_rd(W2_ELBI + 0x74));
	kv("W2: EP command/status", w2_rd(W2_CFG + 0x04));	/* bit1 memory space + bit2 bus master must be set (wifi_link: 0x6) */
	if ((w2_rd(W2_CFG + 0x04) & 6) != 6) {
		out("W2: bus master or memory space off, enabling\n");
		w2_wr(W2_CFG + 0x04, (w2_rd(W2_CFG + 0x04) & 0xffffu) | 6u);
		kv("W2: EP command now", w2_rd(W2_CFG + 0x04) & 0xffffu);
	}
	w2_fl();
	i = w2_t32(W2_RAME - 4);
	kv("W2: last RAM word (pcie_shared pointer from the firmware)", i);
	if (!w2_ram_ptr(i)) {
		out("W2: ABORT: no pcie_shared pointer: the firmware is not running (run with wifiprobe=9 so stage 8 ran first in this boot)\n");
		w2_fl();
		return;
	}
	w2_sh = i;
	for (i = 0; i < SH_WORDS; i++)
		sh[i] = w2_t32(w2_sh + 4 * i);
	flags = sh[SH_FLAGS / 4];
	kv("W2: pcie_shared flags (word 0)", flags);
	kv("W2:   version", flags & F_VERSION_MASK);
	kv("W2:   trap_addr", sh[SH_TRAP_ADDR / 4]);
	kv("W2:   console_addr", sh[SH_CONSOLE / 4]);
	kv("W2:   fwid", sh[SH_FWID / 4]);
	kv("W2:   max_host_rxbufs (u16) | total_lfrag (u16)", sh[SH_MAXRXBUFS / 4]);
	kv("W2:   dma_rxoffset", sh[SH_DMA_RXOFF / 4]);
	kv("W2:   h2d_mb_data_ptr", sh[SH_H2D_MB / 4]);
	kv("W2:   d2h_mb_data_ptr", sh[SH_D2H_MB / 4]);
	kv("W2:   rings_info_ptr", sh[SH_RINGINFO / 4]);
	kv("W2:   host_dma_scratch_buffer_len (host writes)", sh[SH_SCR_LEN / 4]);
	kv("W2:   flags2", sh[SH_FLAGS2 / 4]);
	kv("W2:   host_cap (host writes)", sh[SH_HOSTCAP / 4]);
	kv("W2:   feature DMA_INDEX (needs host index arrays; unsupported here)", !!(flags & F_DMA_INDEX));
	kv("W2:   feature 2BYTE_INDICES", !!(flags & F_2BYTE_IDX));
	kv("W2:   feature D2H_SYNC_SEQNUM", !!(flags & F_D2H_SEQNUM));
	kv("W2:   feature D2H_SYNC_XORCSUM", !!(flags & F_D2H_XORCSUM));
	kv("W2:   feature HOSTRDY_SUPPORT", !!(flags & F_HOSTRDY));
	kv("W2:   feature NO_OOB_DW", !!(flags & F_NO_OOB_DW));
	kv("W2:   feature IDMA", !!(flags & F_IDMA));
	kv("W2:   feature IFRM", !!(flags & F_IFRM));
	kv("W2:   feature DAR (doorbell registers elsewhere; unsupported here)", !!(flags & F_DAR));
	kv("W2:   feature USE_MAILBOX", !!(flags & F_USE_MAILBOX));
	cons = sh[SH_CONSOLE / 4];
	cons_from = w2_ram_ptr(cons) ? w2_t32(cons + 16) : 0;
	kv("W2: firmware console write index at start", cons_from);
	w2_fl();
	if ((flags & F_VERSION_MASK) != 7 || (flags & (F_DMA_INDEX | F_DAR | F_IDMA | F_IFRM))) {
		out("W2: ABORT: this code handles pcie_shared version 7 with TCM indices and DB0/DB1 doorbells only\n");
		w2_fl();
		return;
	}
	if (flags & (F_TRAP | F_ASSERT)) {
		out("W2: ABORT: firmware already trapped/asserted before any host request\n");
		w2_rg[0].widx = w2_rg[0].ridx = w2_sh;	/* dump_state reads ring indices: point them somewhere harmless */
		for (i = 1; i < 5; i++)
			w2_rg[i] = w2_rg[0];
		w2_dump_state("trap before start");
		w2_console(cons, 0, "W2: console: ");
		return;
	}

	/* ---- 2. ring_info ---- */
	if (!w2_ram_ptr(sh[SH_RINGINFO / 4]) || sh[SH_RINGINFO / 4] + sizeof(ri) > W2_RAME) {
		out("W2: ABORT: rings_info_ptr invalid\n");
		w2_fl();
		return;
	}
	for (i = 0; i < 15; i++)
		ri[i] = w2_t32(sh[SH_RINGINFO / 4] + 4 * i);
	ringmem = ri[0];
	hw = ri[1];
	hr = ri[2];
	dw = ri[3];
	dr = ri[4];
	maxflow = ri[13] & 0xffff;
	maxsub = ri[13] >> 16;
	maxcmp = ri[14] & 0xffff;
	kv("W2: ring_info.ringmem_ptr", ringmem);
	kv("W2:   h2d_w_idx_ptr", hw);
	kv("W2:   h2d_r_idx_ptr", hr);
	kv("W2:   d2h_w_idx_ptr", dw);
	kv("W2:   d2h_r_idx_ptr", dr);
	kv("W2:   h2d_w_idx_hostaddr (unused, no DMA index)", ((uint64_t)ri[6] << 32) | ri[5]);
	kv("W2:   max_tx_flowrings", maxflow);
	kv("W2:   max_submission_queues", maxsub);
	kv("W2:   max_completion_rings", maxcmp);
	w2_fl();
	if (!w2_ram_ptr(ringmem) || !w2_ram_ptr(hw) || !w2_ram_ptr(hr) || !w2_ram_ptr(dw) || !w2_ram_ptr(dr)) {
		out("W2: ABORT: ring_info pointers are not TCM RAM addresses\n");
		w2_fl();
		return;
	}
	idxsz = (flags & F_2BYTE_IDX) ? 2 : 4;	/* spacing of the per-ring index slots (DHD bus->rw_index_sz) */
	for (i = 0; i < 5; i++) {
		w2_k("W2: ring_mem[i] id/type word", w2_t32(ringmem + 16 * i));
		w2_k("W2:   max_item|len_items (firmware default, overwritten)", w2_t32(ringmem + 16 * i + 4));
	}

	/* ---- 3. host RAM: clean cache lines (no-op with the MMU off but harmless), zero, set up the 5 common rings ---- */
	for (uintptr_t a = W2_DMA_BASE & ~63UL; a < W2_DMA_BASE + L_TOTAL; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	for (uintptr_t a = W2_DMA_BASE; a < W2_DMA_BASE + L_TOTAL; a += 8)
		*(volatile uint64_t *)a = 0;
	w2_dsb();
	/* proof the RAM region is real memory and the CPU can use it (a stuck bus / bad address would show here) */
	w2_wr(W2_DMA_BASE + L_SCRATCH, 0xa5a5a5a5u);
	w2_k("W2: RAM test word (expect 0xa5a5a5a5)", w2_rd(W2_DMA_BASE + L_SCRATCH));
	w2_wr(W2_DMA_BASE + L_SCRATCH, 0);
	w2_ring_setup(&w2_rg[0], 0, 64, 40, L_CTRL_SUB);
	w2_ring_setup(&w2_rg[1], 1, 512, 32, L_RXPOST);
	w2_ring_setup(&w2_rg[2], 2, 64, 24, L_CTRL_CPL);
	w2_ring_setup(&w2_rg[3], 3, 1024, 24, L_TX_CPL);
	w2_ring_setup(&w2_rg[4], 4, 512, 40, L_RX_CPL);
	for (i = 0; i < 2; i++) {	/* H2D: index slot i of the h2d arrays */
		w2_rg[i].widx = hw + idxsz * i;
		w2_rg[i].ridx = hr + idxsz * i;
	}
	for (i = 2; i < 5; i++) {	/* D2H: slot (id - 2) of the d2h arrays */
		w2_rg[i].widx = dw + idxsz * (i - 2);
		w2_rg[i].ridx = dr + idxsz * (i - 2);
	}
	/* the index slots are 32-bit wide cells in the firmware layout; DHD/Linux access them as 16-bit at the cell start */
	if (idxsz != 4)
		out("W2: note: 2-byte index spacing, cells assumed packed\n");

	/* ---- 4. select the PCIe2 core in the BAR0 window (needed for the mailbox/doorbell registers) ---- */
	out("W2: selecting PCIe2 core (0x18003000) in the BAR0 window\n");
	w2_fl();
	w2_wr(W2_CFG + 0x80, W2_PCIE2_CORE);
	kv("W2: BAR0 window readback", w2_rd(W2_CFG + 0x80));
	kv("W2: PCIe2 MAILBOXINT before", w2_rd(W2_BAR0 + W2_REG_MBINT));
	kv("W2: PCIe2 MAILBOXMASK", w2_rd(W2_BAR0 + W2_REG_MBMASK));
	w2_fl();

	/* ---- 5. tell the firmware: host_cap, scratch buffer, ring memory (DHD dhd_prot_init order) ---- */
	w2_tw32(w2_sh + SH_HOSTCAP, W2_HOSTCAP);
	w2_tw32(w2_sh + SH_SCR_LEN, 8);
	w2_tw32(w2_sh + SH_SCR_ADDR, (uint32_t)(W2_DMA_BASE + L_SCRATCH));
	w2_tw32(w2_sh + SH_SCR_ADDR + 4, (uint32_t)((W2_DMA_BASE + L_SCRATCH) >> 32));
	kv("W2: host_cap readback", w2_t32(w2_sh + SH_HOSTCAP));
	kv("W2: scratch addr readback", w2_t32(w2_sh + SH_SCR_ADDR));
	for (i = 0; i < 5; i++) {
		uint32_t rm = ringmem + 16 * i;

		w2_tw32(rm + 8, (uint32_t)w2_rg[i].host);		/* base_addr low  */
		w2_tw32(rm + 12, (uint32_t)(w2_rg[i].host >> 32));	/* base_addr high */
		w2_tw16(rm + 4, (uint16_t)w2_rg[i].depth);		/* max_item  */
		w2_tw16(rm + 6, (uint16_t)w2_rg[i].ilen);		/* len_items */
		w2_tw16(w2_rg[i].widx, 0);
		w2_tw16(w2_rg[i].ridx, 0);
		w2_k("W2: ring written, ring_mem base low", w2_t32(rm + 8));
		w2_k("W2:   max_item|len_items readback", w2_t32(rm + 4));
	}
	w2_dsb();
	w2_fl();

	/* ---- 6. host ready (flags: HOSTRDY_SUPPORT): doorbell 1 ---- */
	out("W2: HOST READY (doorbell 1)\n");
	w2_fl();
	w2_wr(W2_BAR0 + W2_REG_DB1, W2_DB_VAL);
	w2_us(2000);

	/* ---- 7. two ioctl response buffers, then the requests ---- */
	rc = w2_post_resp(PKTID_RESP0, L_RESP0);
	rc |= w2_post_resp(PKTID_RESP1, L_RESP1);
	kv("W2: response buffers posted (0 = ok)", (uint64_t)(int64_t)rc);
	w2_rep_pktid = 0;
	w2_cons = cons;
	w2_ready = (rc == 0);
	{
		uint64_t t0 = w2_ticks();

		while (w2_ms_since(t0) < 200 && w2_t16(w2_rg[0].ridx) != w2_rg[0].wr)
			w2_us(100);
		kv("W2: chip read the posted buffers (H2D read index == write index)", w2_t16(w2_rg[0].ridx) == w2_rg[0].wr);
		kv("W2:   H2D ctrl read index", w2_t16(w2_rg[0].ridx));
		kv("W2:   after ms", w2_ms_since(t0));
	}
	kv("W2: PCIe2 MAILBOXINT after host-ready + posts", w2_rd(W2_BAR0 + W2_REG_MBINT));
	w2_fl();

	/* cur_etheraddr */
	rc = w2_iovar("cur_etheraddr", WLC_GET_VAR, 256, 2500, &rs);
	if (rc == 0 && rs.cmplt) {
		uintptr_t b = W2_DMA_BASE + (rs.pktid == PKTID_RESP1 ? L_RESP1 : L_RESP0);
		uint32_t m0 = w2_rd(b), m1 = w2_rd(b + 4);
		static const char hx[] = "0123456789abcdef";
		char s[19];

		for (i = 0; i < 6; i++) {
			uint8_t v = *(volatile uint8_t *)(b + i);

			s[i * 3] = hx[v >> 4];
			s[i * 3 + 1] = hx[v & 15];
			s[i * 3 + 2] = i == 5 ? '\n' : ':';
		}
		s[17] = '\n';
		s[18] = 0;
		kv("W2: response words 0 (MAC bytes 0-3)", m0);
		kv("W2: response words 1 (MAC bytes 4-5 in the low half)", m1);
		w2_out("W2: MAC ADDRESS ");
		w2_out(s);
		got_mac = rs.status == 0 && rs.resp_len >= 6;
		w2_k("W2: MAC read OK (status 0, resp_len >= 6)", got_mac);
	} else {
		w2_dump_state("cur_etheraddr failed");
	}
	w2_fl();

	/* ver */
	rc = w2_iovar("ver", WLC_GET_VAR, 512, 2500, &rs);
	if (rc == 0 && rs.cmplt) {
		uintptr_t b = W2_DMA_BASE + (rs.pktid == PKTID_RESP1 ? L_RESP1 : L_RESP0);
		uint32_t n = rs.resp_len;

		if (n > 400)
			n = 400;
		w2_out("W2: FIRMWARE VERSION: ");
		w2_text(b, n);
		got_ver = rs.status == 0 && n > 4;
		w2_k("W2: version read OK (status 0, resp_len > 4)", got_ver);
	} else {
		w2_dump_state("ver failed");
	}
	w2_fl();

	/* ---- 8. final state and the firmware console since we started ---- */
	if (!(got_mac && got_ver))
		w2_dump_state("end of run");
	w2_console(cons, cons_from, "W2: console since start: ");
	w2_k("W2a RESULT mac|ver (bit0 = MAC, bit1 = version)", (uint64_t)(got_mac | (got_ver << 1)));
	out("W2a done\n");
	w2_fl();
}
/* SPDX-License-Identifier: GPL-2.0-only
 *
 * W3a: bring the radio up and scan, over the MSGBUF ioctl channel that W2a (wifi_w2.c) set up.
 * MUST FOLLOW wifi_w2.c in the same translation unit (it uses its statics: rings, w2_ioctl, w2_k ...); the files are
 * concatenated by _append.sh into shim/dreamlte/smp_probe.c. Call wifi_w3() after wifi_w2() in the same boot (wifiprobe=10).
 * Plan, structures and sources: tools/wifi-w2/W3-PLAN.md.
 */

#define WLC_UP			2u
#define WLC_SCAN		50u
#define WLC_SCAN_RESULTS	51u
#define WLC_GET_UP		162u
#define WLC_SET_VAR		263u
#ifndef W3_COUNTRY
#define W3_COUNTRY		"DE"
#endif
#define W3_SCAN_WAIT_MS		4000u
#ifndef W3_LEGACY_POLLS
#define W3_LEGACY_POLLS		3u	/* x 2 s */
#endif
#ifndef W3_ESCAN_MS
#define W3_ESCAN_MS		5000u
#endif

/* prints the BCME name (statements, not a returned string: a string table would need ABS64 relocations) */
static void w3_bcme(int32_t e)
{
	if (e == 0) w2_out("OK");
	else if (e == -1) w2_out("BCME_ERROR");
	else if (e == -2) w2_out("BCME_BADARG");
	else if (e == -4) w2_out("BCME_NOTUP");
	else if (e == -5) w2_out("BCME_NOTDOWN");
	else if (e == -9) w2_out("BCME_RADIOOFF");
	else if (e == -14) w2_out("BCME_BUFTOOSHORT");
	else if (e == -16) w2_out("BCME_BUSY");
	else if (e == -23) w2_out("BCME_UNSUPPORTED");
	else if (e == -24) w2_out("BCME_BADLEN");
	else if (e == -25) w2_out("BCME_NOTREADY");
	else w2_out("(see bcmutils.h BCME_*)");
}

/* small line builder for the per-BSS lines */
struct w3_ln {
	char b[200];
	uint32_t n;
};

static void w3_s(struct w3_ln *l, const char *s)
{
	while (*s && l->n < sizeof(l->b) - 2)
		l->b[l->n++] = *s++;
	l->b[l->n] = 0;
}

static void w3_d(struct w3_ln *l, int32_t v)
{
	char t[12];
	uint32_t n = 0, u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;

	if (v < 0)
		w3_s(l, "-");
	do {
		t[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	while (n) {
		char c[2];

		c[0] = t[--n];
		c[1] = 0;
		w3_s(l, c);
	}
}

static void w3_x2(struct w3_ln *l, uint8_t v)
{
	static const char hx[] = "0123456789abcdef";
	char c[3];

	c[0] = hx[v >> 4];
	c[1] = hx[v & 15];
	c[2] = 0;
	w3_s(l, c);
}

static uint8_t w3_b(uintptr_t a) { return *(volatile uint8_t *)a; }
static uint32_t w3_u16(uintptr_t a) { return w3_b(a) | (uint32_t)w3_b(a + 1) << 8; }
static uint32_t w3_u32(uintptr_t a) { return w3_u16(a) | w3_u16(a + 2) << 16; }

#define SEC_WPA		1u
#define SEC_RSN		2u
#define SEC_PSK		4u
#define SEC_EAP		8u
#define SEC_SAE		16u
#define SEC_OWE		32u

/* walk the beacon IEs (host RAM, byte reads): RSN IE (48) AKM suites, vendor WPA IE (221, 00:50:f2 type 1) */
static uint32_t w3_sec_ies(uintptr_t ie, uint32_t len)
{
	uint32_t o = 0, f = 0;

	while (o + 2 <= len) {
		uint32_t id = w3_b(ie + o), l = w3_b(ie + o + 1);
		uintptr_t p = ie + o + 2;

		if (o + 2 + l > len)
			break;
		if (id == 48 && l >= 8) {	/* RSN: version u16, group suite, pairwise count + suites, AKM count + suites */
			uint32_t pw = w3_u16(p + 6), pos = 8 + 4 * pw;

			f |= SEC_RSN;
			if (pos + 2 <= l) {
				uint32_t ak = w3_u16(p + pos);

				pos += 2;
				for (uint32_t k = 0; k < ak && pos + 4 <= l; k++, pos += 4) {
					if (w3_b(p + pos) == 0x00 && w3_b(p + pos + 1) == 0x0f && w3_b(p + pos + 2) == 0xac) {
						uint32_t t = w3_b(p + pos + 3);

						if (t == 2 || t == 4 || t == 6)
							f |= SEC_PSK;
						else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12)
							f |= SEC_EAP;
						else if (t == 8 || t == 9)
							f |= SEC_SAE;
						else if (t == 18)
							f |= SEC_OWE;
					}
				}
			}
		} else if (id == 221 && l >= 4 && w3_b(p) == 0x00 && w3_b(p + 1) == 0x50 && w3_b(p + 2) == 0xf2 && w3_b(p + 3) == 1) {
			f |= SEC_WPA;
		}
		o += 2 + l;
	}
	return f;
}

static const char *w3_sec_name(uint32_t f, uint32_t cap)
{
	if (f & SEC_RSN) {
		if (f & SEC_SAE)
			return (f & SEC_PSK) ? "WPA2/WPA3-PSK+SAE" : "WPA3-SAE";
		if (f & SEC_OWE)
			return "OWE";
		if (f & SEC_EAP)
			return "WPA2-EAP";
		if (f & SEC_PSK)
			return (f & SEC_WPA) ? "WPA/WPA2-PSK" : "WPA2-PSK";
		return "WPA2";
	}
	if (f & SEC_WPA)
		return "WPA";
	return (cap & 0x10) ? "WEP" : "OPEN";
}

static void w3_status(const char *what, int rc, const struct w2_resp *rs)
{
	w2_out("W3: ");
	w2_out(what);
	if (rc != 0 || !rs->cmplt) {
		w2_out(": NO COMPLETION (transport rc, see W2: lines)\n");
		return;
	}
	w2_out(": ");
	w3_bcme(rs->status);
	w2_out("\n");
	w2_k("W3:   status (signed; ack status in W2 lines)", (uint64_t)(int64_t)rs->status);
}

/* iovar SET: "name\0" + data (len bytes), input_buf_len = output_buf_len = the whole buffer, as bcm_mkiovar + dhd_iovar do */
static int w3_setvar(const char *name, const uint8_t *data, uint32_t dlen, struct w2_resp *rs)
{
	uint8_t buf[96];
	uint32_t n = 0, i;

	while (name[n] && n < 40)
		buf[n] = (uint8_t)name[n], n++;
	buf[n++] = 0;
	for (i = 0; i < dlen && n < sizeof(buf); i++)
		buf[n++] = data[i];
	return w2_ioctl(WLC_SET_VAR, name, buf, n, n, n, 2500, rs);
}

/* ---- W3b: CLM blob download (bcmdhd dhd_common.c dhd_download_blob / dhd_download_2_dongle / dhd_apply_default_clm) ---- */
extern const uint8_t wifi_clm[], wifi_clm_end[];	/* shim/dreamlte/wifi_fw.S, fw/wifi_clm.bin (staged by tools/wifi-w2/wifi-clm-stage.sh) */

#define W3_CLM_CHUNK		1408u	/* dhd_common.c MAX_CHUNK_LEN */
#define W3_DLOAD_VER_SHIFT	12	/* wl_dload_data.flag = (DLOAD_HANDLER_VER = 1) << 12 | flags */
#define W3_DL_BEGIN		0x0002u
#define W3_DL_END		0x0004u
#define W3_DL_TYPE_CLM		2u
#define WLC_GET_VAR_		262u

static uintptr_t w3_resp(const struct w2_resp *rs)
{
	return W2_DMA_BASE + (rs->pktid == PKTID_RESP1 ? L_RESP1 : L_RESP0);
}

/* print a text iovar reply: "label: text" */
static void w3_get_text(const char *iovar, uint32_t outlen)
{
	struct w2_resp rs;
	int rc = w2_iovar(iovar, WLC_GET_VAR_, outlen, 2500, &rs);

	w3_status(iovar, rc, &rs);
	if (rc == 0 && rs.cmplt && rs.status == 0) {
		uint32_t n = rs.resp_len > outlen ? outlen : rs.resp_len;

		if (n > 300)
			n = 300;
		w2_out("W3:   ");
		w2_out(iovar);
		w2_out(" = ");
		w2_text(w3_resp(&rs), n);
	}
}

/* 'country' get: wl_country_t { country_abbrev[4], rev s32, ccode[4] }; "#n" / "#a" = no CLM country data */
static void w3_get_country(const char *when)
{
	struct w2_resp rs;
	int rc = w2_iovar("country", WLC_GET_VAR_, 64, 2500, &rs);

	w2_out("W3: country ");
	w2_out(when);
	w2_out("\n");
	if (rc == 0 && rs.cmplt && rs.status == 0) {
		uintptr_t b = w3_resp(&rs);
		struct w3_ln l;

		l.n = 0;
		l.b[0] = 0;
		w3_s(&l, "W3:   abbrev '");
		for (uint32_t i = 0; i < 4 && w3_b(b + i); i++) {
			char s[2] = { (char)w3_b(b + i), 0 };

			w3_s(&l, s);
		}
		w3_s(&l, "' rev ");
		w3_d(&l, (int32_t)w3_u32(b + 4));
		w3_s(&l, " ccode '");
		for (uint32_t i = 0; i < 4 && w3_b(b + 8 + i); i++) {
			char s[2] = { (char)w3_b(b + 8 + i), 0 };

			w3_s(&l, s);
		}
		w3_s(&l, "'\n");
		w2_out(l.b);
	} else {
		w3_status("country get", rc, &rs);
	}
}

/* returns 1 when every chunk was accepted */
static int w3_clmload(void)
{
	uint32_t total = (uint32_t)(wifi_clm_end - wifi_clm), off = 0, nch = 0;
	uint8_t buf[1480];
	struct w2_resp rs;
	int ok = 1;

	w2_k("W3: CLM blob size", total);
	if (total == 0) {
		w2_out("W3: no CLM blob embedded\n");
		return 0;
	}
	w2_k("W3: CLM blob first word", (uint32_t)wifi_clm[0] | (uint32_t)wifi_clm[1] << 8 | (uint32_t)wifi_clm[2] << 16 | (uint32_t)wifi_clm[3] << 24);
	while (off < total && ok) {
		uint32_t n = total - off, flag, plen, i, k = 0, rc;

		if (n > W3_CLM_CHUNK)
			n = W3_CLM_CHUNK;
		flag = (1u << W3_DLOAD_VER_SHIFT) | (off == 0 ? W3_DL_BEGIN : 0) | (off + n == total ? W3_DL_END : 0);
		for (i = 0; i < sizeof(buf); i++)
			buf[i] = 0;
		/* "clmload\0" + wl_dload_data { u16 flag, u16 dload_type = 2 (CLM), u32 len = data bytes, u32 crc = 0 } + data, data part padded to 8 */
		for (const char *s = "clmload"; *s; s++)
			buf[k++] = (uint8_t)*s;
		buf[k++] = 0;
		buf[k + 0] = (uint8_t)flag;
		buf[k + 1] = (uint8_t)(flag >> 8);
		buf[k + 2] = (uint8_t)W3_DL_TYPE_CLM;
		buf[k + 3] = 0;
		buf[k + 4] = (uint8_t)n;
		buf[k + 5] = (uint8_t)(n >> 8);
		buf[k + 6] = (uint8_t)(n >> 16);
		buf[k + 7] = (uint8_t)(n >> 24);
		for (i = 0; i < n; i++)
			buf[k + 12 + i] = wifi_clm[off + i];
		plen = (12 + n + 7) & ~7u;
		k += plen;
		rc = (uint32_t)w2_ioctl(WLC_SET_VAR, "clmload chunk", buf, k, k, k, 3000, &rs);
		nch++;
		if (rc != 0 || !rs.cmplt || rs.status != 0) {
			w2_k("W3: CLM chunk FAILED, offset", off);
			w3_status("clmload", (int)rc, &rs);
			ok = 0;
			break;
		}
		off += n;
	}
	w2_k("W3: CLM chunks sent", nch);
	w2_k("W3: CLM bytes accepted", off);
	if (!ok)
		w3_get_text("clmload_status", 16);	/* DHD prints clmload_status (an int) after a failure; the text print shows its raw bytes */
	w3_get_text("clm_ver", 256);
	return ok && off == total;
}

/* ---- W3c: BSS printer shared by the legacy results and the escan events ---- */
static uint32_t w3_bss_line(uintptr_t r, uint32_t avail, uint32_t n, int first)
{
	uint32_t ver, len, ieo, iel, cap, sl, ch, sec, i;
	int32_t rssi;
	struct w3_ln l;

	if (avail < 124)
		return 0;
	ver = w3_u32(r);
	len = w3_u32(r + 4);
	ieo = w3_u16(r + 116);
	iel = w3_u32(r + 120);
	cap = w3_u16(r + 16);
	sl = w3_b(r + 18);
	ch = w3_b(r + 88);
	rssi = (int16_t)w3_u16(r + 78);
	if (len < 124 || len > avail) {
		w2_k("W3: bss record length implausible, stop: length", len);
		return 0;
	}
	if (first) {
		w2_k("W3: first wl_bss_info version (109 expected)", ver);
		w2_k("W3:   length", len);
		w2_k("W3:   ie_offset", ieo);
		w2_k("W3:   ie_length", iel);
		w2_k("W3:   raw chanspec|atim|dtim|accessnet", w3_u32(r + 72));
	}
	if (ch == 0)
		ch = w3_u16(r + 72) & 0xff;	/* chanspec low byte = channel */
	if (ieo > len)
		ieo = len;
	if (iel > len - ieo)
		iel = len - ieo;
	sec = w3_sec_ies(r + ieo, iel);
	if (sl > 32)
		sl = 32;
	l.n = 0;
	l.b[0] = 0;
	w3_s(&l, "W3: BSS ");
	w3_d(&l, (int32_t)n + 1);
	w3_s(&l, " ch ");
	w3_d(&l, (int32_t)ch);
	w3_s(&l, " rssi ");
	w3_d(&l, rssi);
	w3_s(&l, " ");
	w3_s(&l, w3_sec_name(sec, cap));
	w3_s(&l, " bssid ");
	for (i = 0; i < 3; i++) {	/* first 3 octets only */
		w3_x2(&l, w3_b(r + 8 + i));
		w3_s(&l, ":");
	}
	w3_s(&l, "xx:xx:xx ssid \"");
	for (i = 0; i < sl; i++) {
		uint8_t c = w3_b(r + 19 + i);
		char s[2];

		s[0] = (c >= 32 && c < 127) ? (char)c : '?';
		s[1] = 0;
		w3_s(&l, s);
	}
	w3_s(&l, "\"\n");
	w2_out(l.b);
	return len;
}

/* ---- W3c: event path (dhd_msgbuf_rxbuf_post_event_bufs, dhd_prot_event_process) ---- */
#define MT_EVENT_BUF_POST	0x0D
#define PKTID_EVT0		0x200u
#define EVT_BUFS		8u
#define EVT_SZ			2048u
#define WLC_E_SCAN_COMPLETE	26u
#define WLC_E_ESCAN_RESULT	69u

static uint32_t w3_evt_type[16], w3_evt_cnt[16];
static uint32_t w3_ev_total, w3_ev_logged, w3_escan_bss, w3_escan_done, w3_ev_posted;

static uint32_t w3_be32(uintptr_t a)
{
	return (uint32_t)w3_b(a) << 24 | (uint32_t)w3_b(a + 1) << 16 | (uint32_t)w3_b(a + 2) << 8 | w3_b(a + 3);
}

/* ioctl_resp_evt_buf_post_msg_t with msg_type MSG_TYPE_EVENT_BUF_POST: same layout as the ioctl response buffer post */
static int w3_post_event(uint32_t idx)
{
	struct w2_ring *r = &w2_rg[0];
	uintptr_t b = W2_DMA_BASE + L_EVENT + (uintptr_t)idx * EVT_SZ, it;

	for (uint32_t i = 0; i < EVT_SZ; i += 4)
		w2_wr(b + i, 0);
	it = w2_h2d_begin(r, MT_EVENT_BUF_POST);
	if (!it)
		return -1;
	w2_wr(it + 4, PKTID_EVT0 + idx);
	w2_wr(it + 8, EVT_SZ);
	w2_wr(it + 16, (uint32_t)b);
	w2_wr(it + 20, (uint32_t)(b >> 32));
	w2_h2d_commit(r);
	return 0;
}

/* a MSG_TYPE_WL_EVENT item: request_id = our pktid, event_data_len u16 @12, seqnum u16 @14; the frame is in the posted buffer:
 * ether header (14) + bcmeth_hdr (10) + wl_event_msg_t (48, big endian) + data (host-defined layout, little endian for escan) */
static void w3_on_event(uintptr_t d)
{
	uint32_t pktid = w2_rd(d + 4), idx = pktid - PKTID_EVT0, edl = w2_rd(d + 12) & 0xffff;
	uint32_t et, st, rsn, dlen, i, slot = 16;
	uintptr_t b;

	w3_ev_total++;
	if (idx >= EVT_BUFS) {
		w2_k("W3: event item with unknown pktid", pktid);
		return;
	}
	b = W2_DMA_BASE + L_EVENT + (uintptr_t)idx * EVT_SZ;
	et = w3_be32(b + 28);
	st = w3_be32(b + 32);
	rsn = w3_be32(b + 36);
	dlen = w3_be32(b + 44);
	for (i = 0; i < 16; i++)
		if (w3_evt_cnt[i] && w3_evt_type[i] == et) {
			slot = i;
			break;
		}
	if (slot == 16)
		for (i = 0; i < 16; i++)
			if (!w3_evt_cnt[i]) {
				w3_evt_type[i] = et;
				slot = i;
				break;
			}
	if (slot < 16)
		w3_evt_cnt[slot]++;
	if (w3_ev_logged++ < 30) {
		w2_k("W3: EVENT type", et);
		w2_k("W3:   status|reason", ((uint64_t)st << 32) | rsn);
		w2_k("W3:   wl_event datalen|event_data_len|ethertype", ((uint64_t)dlen << 32) | (edl << 16) | (w3_b(b + 12) << 8 | w3_b(b + 13)));
		if (w3_ev_logged == 1) {
			w2_k("W3:   frame words 0..1 (check: ethertype 0x886c at byte 12)", ((uint64_t)w2_rd(b + 4) << 32) | w2_rd(b));
			w2_k("W3:   frame words 2..3", ((uint64_t)w2_rd(b + 12) << 32) | w2_rd(b + 8));
		}
	}
	if (et == WLC_E_ESCAN_RESULT) {
		uint32_t cnt = w3_u16(b + 82), off = 84, n;

		w2_k("W3:   escan result: sync_id|bss_count", ((uint64_t)w3_u16(b + 80) << 16) | cnt);
		for (n = 0; n < cnt && n < 8 && off + 124 <= EVT_SZ; n++) {
			uint32_t len = w3_bss_line(b + off, EVT_SZ - off, w3_escan_bss, w3_escan_bss == 0);

			if (!len)
				break;
			w3_escan_bss++;
			off += len;
		}
		if (st == 0)	/* WLC_E_STATUS_SUCCESS: scan finished (8 = PARTIAL: a result was delivered) */
			w3_escan_done = 1;
	} else if (et == WLC_E_SCAN_COMPLETE) {
		w2_k("W3:   SCAN_COMPLETE event, status", st);
		w3_escan_done = 1;
	}
	(void)w3_post_event(idx);	/* keep the pool full, as DHD does */
}

/* drain the D2H control ring for up to ms (stop early once the escan finished) */
static void w3_pump(uint32_t ms)
{
	struct w2_ring *cp = &w2_rg[2];
	uint64_t t0 = w2_ticks();
	uintptr_t d;
	uint32_t other = 0;

	while (w2_ms_since(t0) < ms && !w3_escan_done) {
		while ((d = w2_d2h_peek(cp)) != 0) {
			uint32_t type = w2_rd(d) & 0xff;

			w2_typecnt[type & 63]++;
			if (type == MT_WL_EVENT) {
				w3_on_event(d);
			} else if (other++ < 8) {
				w2_k("W3: other D2H ctrl item, type", type);
				w2_k("W3:   words 0,1", ((uint64_t)w2_rd(d + 4) << 32) | w2_rd(d));
				w2_k("W3:   words 2,3", ((uint64_t)w2_rd(d + 12) << 32) | w2_rd(d + 8));
			}
			w2_d2h_done(cp);
		}
		w2_us(20000);
	}
	w2_k("W3: pump finished after ms", w2_ms_since(t0));
}

/* ---- W3c: diagnostics, all best effort ---- */
static void w3_get(uint32_t cmd, const char *label, uint32_t len, uint32_t nwords)
{
	struct w2_resp rs;
	int rc = w2_ioctl(cmd, label, (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0", 0, len, len, 2500, &rs);

	w3_status(label, rc, &rs);
	if (rc == 0 && rs.cmplt && rs.status == 0)
		for (uint32_t i = 0; i < nwords; i++)
			w2_k("W3:   value word", w2_rd(w3_resp(&rs) + 4 * i));
}

static void w3_diag(const char *when)
{
	struct w2_resp rs;
	int rc;

	w2_out("W3: diagnostics ");
	w2_out(when);
	w2_out("\n");
	w3_get(37, "WLC_GET_RADIO (bit0 sw disable, bit1 hw disable)", 4, 1);
	w3_get(141, "WLC_GET_BAND (0 auto, 1 5G, 2 2G, 3 all)", 4, 1);
	w3_get(29, "WLC_GET_CHANNEL (hw, target, scan channel)", 12, 3);
	w3_get(85, "WLC_GET_PM", 4, 1);
	rc = w2_iovar("scansuppress", WLC_GET_VAR, 16, 2500, &rs);
	w3_status("scansuppress get", rc, &rs);
	if (rc == 0 && rs.cmplt && rs.status == 0)
		w2_k("W3:   scansuppress (1 = scans suppressed)", w2_rd(w3_resp(&rs)));
	/* WLC_GET_VALID_CHANNELS: wl_uint32_list { count, element[] }: how many channels the CLM allows for the country */
	{
		/* the input word 'count' is the capacity of the list (wl_uint32_list_t: "in - # of elements"); W3c sent 0 -> BCME_BUFTOOSHORT */
		static const uint8_t cap[4] = { 128, 0, 0, 0 };

		rc = w2_ioctl(217, "WLC_GET_VALID_CHANNELS (capacity 128)", cap, 4, 1024, 1024, 2500, &rs);
	}
	w3_status("valid channels", rc, &rs);
	if (rc == 0 && rs.cmplt && rs.status == 0) {
		uintptr_t b = w3_resp(&rs);
		uint32_t cnt = w3_u32(b), n2 = 0, n5 = 0;
		struct w3_ln l;

		l.n = 0;
		l.b[0] = 0;
		w3_s(&l, "W3:   channels:");
		if (cnt > 100)
			cnt = 100;
		for (uint32_t i = 0; i < cnt; i++) {
			uint32_t c = w3_u32(b + 4 + 4 * i);

			if (c <= 14)
				n2++;
			else
				n5++;
			if (i < 24) {
				w3_s(&l, " ");
				w3_d(&l, (int32_t)c);
			}
		}
		w3_s(&l, "\n");
		w2_out(l.b);
		w2_k("W3:   valid channels 2.4 GHz", n2);
		w2_k("W3:   valid channels 5 GHz", n5);
	}
	w3_get_country(when);
	w2_fl();
}

/* WLC_SCAN_RESULTS once: returns the number of BSS records printed; sets *ok when the call returned status 0 */
static uint32_t w3_fetch_results(int *ok, uint32_t *count_out)
{
	struct w2_resp rs;
	uint8_t rq[16];
	uint32_t i, shown = 0;
	int rc;

	for (i = 0; i < sizeof(rq); i++)
		rq[i] = 0;
	rq[1] = 0x20;	/* wl_scan_results_t.buflen = 8192, little endian (what the wl utility passes) */
	rc = w2_ioctl(WLC_SCAN_RESULTS, "WLC_SCAN_RESULTS (8192-byte buffer)", rq, sizeof(rq), 2008, 8192, 5000, &rs);
	w3_status("WLC_SCAN_RESULTS", rc, &rs);
	*count_out = 0;
	if (rc == 0 && rs.cmplt) {
		uintptr_t b = w3_resp(&rs);
		uint32_t dl = rs.resp_len > RESP_SZ ? RESP_SZ : rs.resp_len, off = 12, n, count = w3_u32(b + 8);

		w2_k("W3:   wl_scan_results.version", w3_u32(b + 4));
		w2_k("W3:   wl_scan_results.count", count);
		*count_out = count;
		if (rs.status == 0 && dl >= 12)
			*ok = 1;
		for (n = 0; n < count && n < 64 && off + 124 <= dl; n++) {
			uint32_t len = w3_bss_line(b + off, dl - off, n, n == 0);

			if (!len)
				break;
			shown++;
			off += len;
		}
		if (count && !shown)
			w2_out("W3: count > 0 but no record parsed (layout/version assumption)\n");
	}
	return shown;
}

/* build "wl_scan_params_t" (64 bytes) at p: broadcast SSID, all channels, defaults */
static void w3_scan_params(uint8_t *p, int passive)
{
	uint32_t i;

	for (i = 0; i < 64; i++)
		p[i] = 0;
	if (passive)
		p[43] = 1;	/* scan_type = DOT11_SCANTYPE_PASSIVE: receive only, no probe request is transmitted */
	for (i = 36; i < 42; i++)
		p[i] = 0xff;	/* bssid ff:ff:ff:ff:ff:ff */
	p[42] = 2;		/* DOT11_BSSTYPE_ANY */
	for (i = 44; i < 60; i++)
		p[i] = 0xff;	/* nprobes, active, passive, home time = -1 (default) */
}

/* second attempt: event buffers + event_msgs + escan; prints BSS from WLC_E_ESCAN_RESULT events */
static void w3_escan(int passive)
{
	struct w2_resp rs;
	uint8_t mask[16], ep[8 + 64];
	uint32_t i, posted = 0;
	int rc;

	w2_out(passive ? "W3: EVENT PATH, PASSIVE escan (no probe requests are sent)\n" :
		"W3: EVENT PATH: post event buffers, enable ESCAN_RESULT, run escan\n");
	w3_escan_done = 0;
	w2_evhook = w3_on_event;
	if (!w3_ev_posted) {	/* the setup is done once; the passive run reuses it */
		for (i = 0; i < EVT_BUFS; i++)
			if (w3_post_event(i) == 0)
				posted++;
		w3_ev_posted = posted;
		w2_k("W3: event buffers posted", posted);
		w2_us(50000);

		/* event_msgs: bit n of the 16-byte mask enables event n (DHD reads, modifies, writes: dhd_linux.c:11671-11775) */
		for (i = 0; i < 16; i++)
			mask[i] = 0;
		rc = w2_iovar("event_msgs", WLC_GET_VAR, 64, 2500, &rs);
		w3_status("event_msgs get", rc, &rs);
		if (rc == 0 && rs.cmplt && rs.status == 0)
			for (i = 0; i < 16; i++)
				mask[i] = w3_b(w3_resp(&rs) + i);
		mask[WLC_E_ESCAN_RESULT / 8] |= (uint8_t)(1u << (WLC_E_ESCAN_RESULT % 8));
		mask[WLC_E_SCAN_COMPLETE / 8] |= (uint8_t)(1u << (WLC_E_SCAN_COMPLETE % 8));
		rc = w3_setvar("event_msgs", mask, 16, &rs);
		w3_status("event_msgs set (26 SCAN_COMPLETE, 69 ESCAN_RESULT)", rc, &rs);
	}

	/* escan_params_t: u32 version = 1 (ESCAN_REQ_VERSION), u16 action = 1 (WL_SCAN_ACTION_START), u16 sync_id, wl_scan_params_t */
	for (i = 0; i < sizeof(ep); i++)
		ep[i] = 0;
	ep[0] = 1;
	ep[4] = 1;
	ep[6] = passive ? 0x35 : 0x34;	/* sync_id 0x1234 active, 0x1235 passive */
	ep[7] = 0x12;
	w3_scan_params(ep + 8, passive);
	rc = w3_setvar("escan", ep, sizeof(ep), &rs);
	w3_status("escan start", rc, &rs);
	w3_pump(passive ? 8000u : W3_ESCAN_MS);
}

/* receiver sanity: tune a few channels and read the PHY noise floor (WLC_GET_PHY_NOISE, dBm). A working receiver reports about -85..-100;
 * 0 or a constant/absurd value on every channel means no working RX path (nvram/FEM) */
static void w3_noise(void)
{
	static const uint8_t chans[8] = { 1, 6, 11, 36, 64, 100, 149, 165 };
	struct w2_resp rs;
	uint8_t arg[4];

	w2_out("W3: noise floor per channel (channel, noise dBm)\n");
	for (uint32_t i = 0; i < sizeof(chans); i++) {
		int rc;
		struct w3_ln l;

		arg[0] = chans[i];
		arg[1] = arg[2] = arg[3] = 0;
		rc = w2_ioctl(30, "WLC_SET_CHANNEL", arg, 4, 4, 4, 2500, &rs);
		if (rc != 0 || !rs.cmplt || rs.status != 0) {
			w2_k("W3:   set channel failed, channel", chans[i]);
			w3_status("WLC_SET_CHANNEL", rc, &rs);
			continue;
		}
		w2_us(200000);
		rc = w2_ioctl(135, "WLC_GET_PHY_NOISE", (const uint8_t *)"\0\0\0\0", 0, 4, 4, 2500, &rs);
		l.n = 0;
		l.b[0] = 0;
		w3_s(&l, "W3:   channel ");
		w3_d(&l, chans[i]);
		w3_s(&l, " noise ");
		if (rc == 0 && rs.cmplt && rs.status == 0) {
			w3_d(&l, (int32_t)w2_rd(w3_resp(&rs)));
			w3_s(&l, " dBm\n");
		} else {
			w3_s(&l, "(query failed)\n");
		}
		w2_out(l.b);
	}
}

/* OTP CIS: which module is this? bcmdhd dhd_pcie.c dhd_parse_board_information_bcm4361 reads the CIS tuples at backplane 0x18011110..0x18011167
 * after writing OTP control 1 (0x18010324) = 0x00fa0000 and the PMU min resource mask (0x18012618) = 0x0e4fffff; vendor-ID tuple 0x81 selects the
 * NVRAM/CLM variant via dhd_custom_cis.c vid_info[]. Call BEFORE the firmware download (CR4 halted), exactly where DHD does it; it saves and
 * restores the BAR0 window so the caller's wrapper window is untouched. First a read-only attempt, the two DHD writes only if the CIS is not there. */
void wifi_w3_otp(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
void wifi_w3_otp(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	uint32_t win = w2_rd(W2_CFG + 0x80), raw[22], i, pass;
	uint8_t *p = (uint8_t *)raw;
	int tot, vlen = 0, found = 0;
	uint8_t vid[8] = { 0 };
	uint32_t btype = 0;

	w2_kv = kv;
	w2_out = out;
	w2_flush = flush;
	out("W3: OTP CIS read (module identification)\n");
	kv("W3: BAR0 window to restore", win);
	for (pass = 0; pass < 2 && !found; pass++) {
		if (pass == 1) {
			out("W3: CIS not valid yet: DHD's OTP/PMU setup writes, then read again\n");
			w2_fl();
			w2_wr(W2_CFG + 0x80, 0x18010000u);
			(void)w2_rd(W2_CFG + 0x80);
			w2_wr(W2_BAR0 + 0x324, 0x00fa0000u);
			kv("W3:   OTP control 1 readback", w2_rd(W2_BAR0 + 0x324));
			w2_wr(W2_CFG + 0x80, 0x18012000u);
			(void)w2_rd(W2_CFG + 0x80);
			w2_wr(W2_BAR0 + 0x618, 0x0e4fffffu);
			kv("W3:   PMU min resource mask readback", w2_rd(W2_BAR0 + 0x618));
		}
		w2_fl();
		w2_wr(W2_CFG + 0x80, 0x18011000u);
		(void)w2_rd(W2_CFG + 0x80);
		for (i = 0; i < 22; i++)
			raw[i] = w2_rd(W2_BAR0 + 0x110 + 4 * i);
		kv("W3: CIS word 0 (first byte 0x80 = valid start tuple)", raw[0]);
		kv("W3: CIS word 1", raw[1]);
		kv("W3: CIS word 2", raw[2]);
		kv("W3: CIS word 3", raw[3]);
		kv("W3: CIS word 4", raw[4]);
		kv("W3: CIS word 5", raw[5]);
		if (p[0] != 0x80)
			continue;
		tot = (int)sizeof(raw);
		for (i = 0; i < sizeof(raw);) {	/* tuple = id, len (tag + data), tag, data[len-1] */
			uint32_t len = p[i + 1], tag = p[i + 2];

			if ((int)(len + 2) > tot || len == 0)
				break;
			if (tag == 0x81 && len > 1 && len - 1 <= sizeof(vid)) {
				vlen = (int)len - 1;
				for (int k = 0; k < vlen; k++)
					vid[k] = p[i + 3 + k];
			} else if (tag == 0x1b && len > 1) {
				btype = p[i + 3];
			}
			tot -= (int)(len + 2);
			i += len + 2;
		}
		found = vlen > 0;
	}
	w2_wr(W2_CFG + 0x80, win);	/* restore (the caller's wrapper window) */
	kv("W3: BAR0 window restored, readback", w2_rd(W2_CFG + 0x80));
	if (found) {
		struct w3_ln l;

		l.n = 0;
		l.b[0] = 0;
		w3_s(&l, "W3: OTP VID bytes:");
		for (int k = 0; k < vlen; k++) {
			w3_s(&l, " ");
			w3_x2(&l, vid[k]);
		}
		w3_s(&l, "  boardtype byte ");
		w3_x2(&l, (uint8_t)btype);
		w3_s(&l, "\n");
		w2_out(l.b);
		if (vlen == 2 && vid[1] == 0x22)
			w2_out("W3: => Murata module (vid_info: murata_mur_r0<byte0 hex>_1kl_b0 for byte0 0x13..0x15, 0x20..0x24, 0x30..0x34; nvram _r0XX_b0)\n");
		else if (vlen == 2 && vid[1] == 0x33)
			w2_out("W3: => SEMCO module (byte0: 51 r01d, 61 r01f, 62 r02g, 71 r01h, 81 r01i, 82 r02j, 91 r02a_b2, a1 r02b_b2; nvram _rXXX_b0)\n");
		else
			w2_out("W3: => unknown vendor id, see dhd_custom_cis.c vid_info[]\n");
	} else {
		w2_out("W3: no valid OTP CIS found (nvram variant stays a guess)\n");
	}
	w2_fl();
}

void wifi_w3(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
void wifi_w3(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void))
{
	struct w2_resp rs;
	uint8_t z4[4] = { 0, 0, 0, 0 };
	uint8_t cc[12] = { 'D', 'E', 0, 0, 0xff, 0xff, 0xff, 0xff, 'D', 'E', 0, 0 };
	uint8_t sp[64];
	int rc, up_ok = 0, scan_ok = 0, res_ok = 0, any = 0, clm_ok = 0;
	uint32_t i, cons_from = 0, count = 0, poll;

	w2_kv = kv;
	w2_out = out;
	w2_flush = flush;
	out("W3: radio up + scan over MSGBUF ioctls (CLM, legacy scan with polling, then the event/escan path)\n");
	if (!w2_ready) {
		out("W3: ABORT: W2a did not finish setting up the rings in this boot\n");
		w2_fl();
		return;
	}
	cc[0] = (uint8_t)W3_COUNTRY[0];
	cc[1] = (uint8_t)W3_COUNTRY[1];
	cc[8] = cc[0];
	cc[9] = cc[1];
	if (w2_ram_ptr(w2_cons))
		cons_from = w2_t32(w2_cons + 16);
	kv("W3: firmware console write index at start", cons_from);
	w2_fl();

	/* 1. CLM first (the firmware has no built-in regulatory data: country '#n' before), while the radio is down */
	w3_get_country("before CLM");
	clm_ok = w3_clmload();
	kv("W3: CLM LOADED OK", clm_ok);
	w3_get_country("after CLM");
	w2_fl();

	/* 2. country (as in W3b: rev -1 worked) */
	rc = w3_setvar("country", cc, sizeof(cc), &rs);
	w3_status("SET country " W3_COUNTRY " (rev -1)", rc, &rs);
	if (!(rc == 0 && rs.cmplt && rs.status == 0)) {
		cc[4] = cc[5] = cc[6] = cc[7] = 0;
		rc = w3_setvar("country", cc, sizeof(cc), &rs);
		w3_status("SET country " W3_COUNTRY " (rev 0)", rc, &rs);
	}
	rc = w3_setvar("mpc", z4, 4, &rs);
	w3_status("SET mpc 0", rc, &rs);

	/* 3. WLC_UP, GET_UP must read 1 */
	for (poll = 0; poll < 2 && !up_ok; poll++) {
		rc = w2_ioctl(WLC_UP, "WLC_UP", z4, 4, 4, 4, 5000, &rs);
		w3_status("WLC_UP", rc, &rs);
		w2_us(poll ? 1000000 : 300000);
		rc = w2_ioctl(WLC_GET_UP, "WLC_GET_UP", z4, 4, 4, 4, 2500, &rs);
		w3_status("GET_UP after", rc, &rs);
		if (rc == 0 && rs.cmplt) {
			uint32_t u = w2_rd(w3_resp(&rs));

			kv("W3:   up state (1 = up)", u);
			if (u == 1 && rs.status == 0)
				up_ok = 1;
		}
	}
	kv("W3: UP OK (GET_UP reads 1)", up_ok);
	w2_fl();

	/* 4. diagnostics before the scan */
	w3_diag("before scan");

	/* 5. legacy scan, polled every 2 s for up to 20 s */
	w3_scan_params(sp, 0);
	rc = w2_ioctl(WLC_SCAN, "WLC_SCAN (wl_scan_params, all channels, active)", sp, sizeof(sp), sizeof(sp), sizeof(sp), 5000, &rs);
	w3_status("WLC_SCAN", rc, &rs);
	scan_ok = (rc == 0 && rs.cmplt && rs.status == 0);
	kv("W3: SCAN STARTED OK", scan_ok);
	w2_fl();
	for (poll = 0; poll < W3_LEGACY_POLLS && !any; poll++) {
		w2_us(2000000);
		kv("W3: legacy poll, seconds since scan start", 2 * (poll + 1));
		(void)w3_fetch_results(&res_ok, &count);
		if (count)
			any = 1;
		w2_fl();
	}
	kv("W3: legacy SCAN RESULTS OK (call status 0)", res_ok);
	kv("W3: legacy at least one BSS", any);
	w3_diag("after legacy scan");

	/* 6. second attempt through events when the legacy path found nothing */
	if (!any) {
		/* BT coexistence: nvram has btc_mode=1; with no BT chip brought up the BT-active/GCI lines may float and the arbiter can deny the
		 * shared antenna to WLAN. Turn it off for the event scans (experiment E3, see W3-PLAN.md W3d). */
		w2_k("W3: btc_mode was (nvram 1); setting 0 for the event scans", 0);
		rc = w3_setvar("btc_mode", z4, 4, &rs);
		w3_status("SET btc_mode 0", rc, &rs);
		w3_escan(0);
	}
	if (!any && !w3_escan_bss)
		w3_escan(1);
	w3_noise();
	for (i = 0; i < 64; i++)
		if (w2_typecnt[i])
			kv("W3: D2H control message type count (type<<32|count)", ((uint64_t)i << 32) | w2_typecnt[i]);
	for (i = 0; i < 16; i++)
		if (w3_evt_cnt[i])
			kv("W3: WLC event type count (type<<32|count)", ((uint64_t)w3_evt_type[i] << 32) | w3_evt_cnt[i]);
	kv("W3: escan BSS seen via events", w3_escan_bss);

	if (!(up_ok && scan_ok && res_ok && (any || w3_escan_bss)))
		w2_dump_state("W3 end of run (something failed)");
	w2_console(w2_cons, cons_from, "W3: console since W3 start: ");
	kv("W3a RESULT bits (0 up, 1 scan started, 2 results call ok, 3 legacy BSS, 4 CLM, 5 events posted, 6 event seen, 7 escan BSS)",
	   (uint64_t)(up_ok | (scan_ok << 1) | (res_ok << 2) | (any << 3) | (clm_ok << 4) |
		      ((w3_ev_posted == EVT_BUFS) << 5) | ((w3_ev_total != 0) << 6) | ((w3_escan_bss != 0) << 7)));
	out("W3 done\n");
	w2_fl();
}
/* ==== END wifi_w2.c ==== */
