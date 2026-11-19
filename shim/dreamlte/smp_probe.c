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
/* ==== BEGIN wifi_w2.c (copy of tools/wifi-w2/wifi_w2.c; edit THERE and re-run tools/wifi-w2/_append.sh) ==== */
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
#define L_TOTAL		0x18000u
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

/* one ioctl round trip: iovar 'name' with cmd (GET_VAR), reply into response buffer *respoff; waits up to timeout_ms */
static int w2_iovar(const char *name, uint32_t cmd, uint32_t outlen, uint32_t timeout_ms, struct w2_resp *rs)
{
	struct w2_ring *rq = &w2_rg[0], *cp = &w2_rg[2];
	uintptr_t it, req = W2_DMA_BASE + L_IOCT_REQ;
	uint64_t t0;
	uint32_t i, len, trans, rdold;
	int fetched = 0;

	rs->ack = rs->cmplt = 0;
	rs->ack_status = rs->status = 0;
	rs->resp_len = rs->pktid = rs->trans = 0;

	/* payload: the iovar name, NUL terminated, rest zero */
	for (i = 0; i < IOCT_REQ_MAX; i += 4)
		w2_wr(req + i, 0);
	for (len = 0; name[len]; len++)
		*(volatile uint8_t *)(req + len) = (uint8_t)name[len];
	len++;
	if (outlen > IOCT_REQ_MAX)
		outlen = IOCT_REQ_MAX;
	trans = ++w2_trans;
	w2_k("W2: ioctl request, cmd", cmd);
	w2_out("W2:   iovar ");
	w2_out(name);
	w2_out("\n");
	w2_k("W2:   trans_id", trans);
	w2_k("W2:   input/output buffer length", outlen);

	it = w2_h2d_begin(rq, MT_IOCTLPTR_REQ);
	if (!it)
		return -1;
	rdold = w2_t16(rq->ridx);
	w2_wr(it + 4, IOCTL_REQ_PKTID);			/* request_id */
	w2_wr(it + 8, cmd);				/* cmd */
	w2_wr(it + 12, trans | (outlen << 16));		/* trans_id, input_buf_len (DHD: both = len) */
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
				w2_k("W2:   ioctl COMPLETE, status", (uint64_t)(int64_t)rs->status);
				w2_k("W2:     resp_len", rs->resp_len);
				w2_k("W2:     trans_id", rs->trans);
				w2_k("W2:     response buffer pktid", rs->pktid);
				w2_k("W2:     cmd", w2_rd(d + 16));
			} else if (type == MT_GEN_STATUS || type == MT_RING_STATUS) {
				w2_k("W2:   firmware status message, status", w2_rd(d + 8) & 0xffff);
			} else if (type == MT_WL_EVENT) {
				w2_out("W2:   (a WLC event arrived: no event buffers were posted by us, so this is unexpected)\n");
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
/* ==== END wifi_w2.c ==== */
