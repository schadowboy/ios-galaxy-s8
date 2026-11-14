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
	}
	out("WIFI link stage done\n");
	fl(flush);
}

/* mode 0 = original behaviour; 2 = switch the CPUCL0 clock before CPU_ON; 3 = observable stub */
void mngs_probe_x(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		  void (*flush)(void), unsigned mode)
{
	const int obs = mode >= 3;	/* 3 and 4 both use the observable stub + cache clean */
	const void *entry = obs ? (const void *)smp_entry3 : (const void *)smp_entry;
	out("MNGS probe (Mongoose cluster 0)\n");
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
