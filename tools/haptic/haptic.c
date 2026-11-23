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
