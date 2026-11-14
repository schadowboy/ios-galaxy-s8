/* SPDX-License-Identifier: GPL-2.0-only
 * Exynos 8895 ACPM (APM co-processor) IPC client for the S8 loader (MMU off, polling).
 *
 * Protocol read from Samsung's kernel sources (drivers/soc/samsung/acpm/acpm_ipc.c,
 * fw_header/framework.h, include/soc/samsung/acpm_ipc_ctrl.h, cal-if/acpm_dvfs.c):
 *   AP2APM mailbox regs 0x16440000 (INTGR0 +0x08 doorbell, INTCR1 +0x20, INTSR1 +0x28)
 *   APM SRAM 0x16500000 (0xB400 bytes); struct acpm_framework at SRAM + 0x2850
 *   framework u32[]: [2]=ipc_channels (SRAM offset) [6]=ipc_ap_max
 *   struct ipc_channel (18 u32): id, field, owner, type, ch{rx_rear, rx_front, rx_base,
 *     rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size, tx_rear, tx_front, tx_base, q_len,
 *     q_elem_size, credit, mbox_addr}, ap_poll.   (APM's rx = AP's tx and vice versa)
 *   AP->APM message: write elem at tx_base + size*front, front = (front+1) % len,
 *     doorbell INTGR0 = (1 << id) << 16.  cmd[0] |= seq(1..63) << 16.
 *   Response: entry in the APM->AP queue (tx_* fields) whose cmd[0] >> 16 & 0x3f == seq.
 *   DVFS request (channel 5): cmd = { domain_idx, rate_kHz, req, 0 };
 *     req FREQ_REQ=0 FREQ_GET=1; domain idx CPUCL0 (Mongoose) = 2, CPUCL1 (A53) = 3,
 *     MIF = 0, G3D = 4 (low 16 bits of ACPM_DVFS_* in dt-bindings/clock/exynos8895.h).
 * Every SRAM pointer taken from the tables is range-checked; a bad layout aborts.
 */
#include <stdint.h>
#include "acpm.h"
#include "pmic.h"
#include "mngs_clk.h"

#define MBOX		0x16440000UL
#define SRAM		0x16500000UL
#define SRAM_SZ		0xB400UL
#define INITDATA	0x2850UL
#define INTGR0		0x08
#define INTCR1		0x20
#define INTSR1		0x28
#define SEQ_SHIFT	16
#define TIMER_HZ	26000000UL

struct chan {
	uint32_t id, type, len, size, ap_poll;
	uint32_t tx_rear, tx_front, tx_base;	/* AP -> APM (APM's rx_*) */
	uint32_t rx_rear, rx_front, rx_base;	/* APM -> AP (APM's tx_*) */
};

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t sram_rd(uint32_t off) { return rd32(SRAM + off); }
static inline void sram_wr(uint32_t off, uint32_t v) { wr32(SRAM + off, v); }

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static int ok_off(uint32_t off, uint32_t len)
{
	return !(off & 3) && off < SRAM_SZ && len <= SRAM_SZ - off;
}

/* Parse channel table entry i into c. Returns 0 or a negative code. */
static int get_chan(uint32_t i, struct chan *c)
{
	uint32_t tab = sram_rd(INITDATA + 8);		/* ipc_channels */
	uint32_t base;

	if (!ok_off(tab, 72 * (i + 1)))
		return -1;
	base = tab + 72 * i;
	c->id = sram_rd(base + 0);
	c->type = sram_rd(base + 12);
	c->rx_rear = sram_rd(base + 16 + 24);	/* ch.tx_rear  (APM->AP) */
	c->rx_front = sram_rd(base + 16 + 28);	/* ch.tx_front */
	c->rx_base = sram_rd(base + 16 + 32);	/* ch.tx_base */
	c->len = sram_rd(base + 16 + 36);
	c->size = sram_rd(base + 16 + 40);
	c->tx_rear = sram_rd(base + 16 + 0);	/* ch.rx_rear  (AP->APM) */
	c->tx_front = sram_rd(base + 16 + 4);	/* ch.rx_front */
	c->tx_base = sram_rd(base + 16 + 8);	/* ch.rx_base */
	c->ap_poll = sram_rd(base + 68);
	if (!c->len || c->len > 64 || c->size < 16 || c->size > 64 || (c->size & 3) ||
	    !ok_off(c->tx_rear, 4) || !ok_off(c->tx_front, 4) ||
	    !ok_off(c->rx_rear, 4) || !ok_off(c->rx_front, 4) ||
	    !ok_off(c->tx_base, c->len * c->size) || !ok_off(c->rx_base, c->len * c->size))
		return -2;
	return 0;
}

void acpm_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint32_t i, n;
	struct chan c;

	out("ACPM dump\n");
	kv("fw plugins", sram_rd(INITDATA + 0));
	kv("num_plugins", sram_rd(INITDATA + 4));
	kv("ipc_channels", sram_rd(INITDATA + 8));
	kv("num_ipc_ch", sram_rd(INITDATA + 12));
	n = sram_rd(INITDATA + 24);
	kv("ipc_ap_max", n);
	kv("ipc_max", sram_rd(INITDATA + 40));
	kv("INTSR1", rd32(MBOX + INTSR1));
	out("\n");
	if (n > 16)
		n = 16;
	for (i = 0; i < n; i++) {
		int rc = get_chan(i, &c);
		kv("ch idx", i);
		kv("rc", (uint64_t)(int64_t)rc);
		if (rc)
			continue;
		kv("id", c.id);
		kv("type", c.type);
		kv("len", c.len);
		kv("size", c.size);
		kv("poll", c.ap_poll);
		kv("field", sram_rd(sram_rd(INITDATA + 8) + 72 * i + 4));
		kv("owner", sram_rd(sram_rd(INITDATA + 8) + 72 * i + 8));
		kv("txF", sram_rd(c.tx_front));
		kv("txR", sram_rd(c.tx_rear));
		kv("rxF", sram_rd(c.rx_front));
		kv("rxR", sram_rd(c.rx_rear));
		if (c.type == 2) {
			uint32_t w;
			out("\n buf tx:");
			for (w = 0; w < 4; w++)
				kv("w", sram_rd(c.tx_base + 4 * w));
			out("\n buf rx:");
			for (w = 0; w < 4; w++)
				kv("w", sram_rd(c.rx_base + 4 * w));
		}
		out("\n");
	}
}

/* Dump the APM's plugin table (struct plugin, 44 bytes, at framework.plugins). */
void acpm_plugins_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint32_t tab = sram_rd(INITDATA + 0), n = sram_rd(INITDATA + 4), i, j;

	out("ACPM plugins\n");
	kv("tab", tab);
	kv("n", n);
	out("\n");
	if (n > 16 || !ok_off(tab, 44 * n))
		return;
	for (i = 0; i < n; i++) {
		uint32_t p = tab + 44 * i, fw = sram_rd(p + 40);
		char name[33];

		kv("plugin", i);
		kv("id", sram_rd(p + 0));
		kv("base", sram_rd(p + 4));
		kv("size", sram_rd(p + 32));
		kv("attached", sram_rd(p + 28) & 0xff);
		kv("stay", sram_rd(p + 36) & 0xff);
		kv("fw_off", fw);
		for (j = 0; j < 32; j++) {
			uint32_t a = fw + j;
			name[j] = 0;
			if (!fw || !ok_off(a & ~3u, 4))
				break;
			name[j] = (char)((sram_rd(a & ~3u) >> (8 * (a & 3))) & 0xff);
			if (!name[j])
				break;
			if (name[j] < 32 || name[j] > 126)
				name[j] = '?';
		}
		name[32] = 0;
		out(" fw=");
		out(name);
		out("\n");
	}
}

/*
 * Read-only dump of the APM firmware's log ring (what Linux prints as [ACPM_FW] lines):
 * framework fields log_buf_rear(52) log_buf_front(56) log_data(60) log_entry_size(64)
 * log_entry_len(68); each entry = header word, 8 chars, value word. Does not move rear.
 */
void acpm_log_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint32_t rear_o = sram_rd(INITDATA + 52), front_o = sram_rd(INITDATA + 56);
	uint32_t data = sram_rd(INITDATA + 60), esz = sram_rd(INITDATA + 64);
	uint32_t len = sram_rd(INITDATA + 68), r, f, i, j;

	out("ACPM fw log\n");
	if (!ok_off(rear_o, 4) || !ok_off(front_o, 4) || esz < 16 || esz > 64 || !len ||
	    len > 1024 || !ok_off(data, esz * len)) {
		out("log layout invalid\n");
		return;
	}
	r = sram_rd(rear_o);
	f = sram_rd(front_o);
	kv("rear", r);
	kv("front", f);
	kv("len", len);
	out("\n");
	if (r >= len || f >= len)
		return;
	/* show the newest 24 entries before 'front' (the ring may already have been drained) */
	i = (f + len - 24) % len;
	for (j = 0; j < 24; j++, i = (i + 1) % len) {
		uint32_t e = data + esz * i, hdr = sram_rd(e);
		char s[9];
		uint32_t k;

		if (!hdr && !sram_rd(e + 4) && !sram_rd(e + 8))
			continue;
		for (k = 0; k < 8; k++) {
			s[k] = (char)((sram_rd(e + 4 + (k & ~3u)) >> (8 * (k & 3))) & 0xff);
			if (s[k] < 32 || s[k] > 126)
				s[k] = '.';
		}
		s[8] = 0;
		kv(" e", i);
		kv("hdr", hdr);
		out(" ");
		out(s);
		kv(" val", sram_rd(e + esz - 4));
		out("\n");
	}
}

static uint32_t seq_num;

/* Send a 4-word command on channel id, wait for the response, copy it back into cmd[]. */
int acpm_send(uint32_t chan_id, uint32_t cmd[4], uint32_t timeout_ms)
{
	uint32_t i, n = sram_rd(INITDATA + 24), w;
	struct chan c;
	int found = 0;

	if (n > 16)
		n = 16;
	for (i = 0; i < n; i++) {
		if (get_chan(i, &c))
			continue;
		if (c.id == chan_id) {
			found = 1;
			break;
		}
	}
	if (!found)
		return -10;

	if (c.type == 2) {
		/* TYPE_BUFFER: one 16-byte slot per direction, no ring pointers.
		 * Write the command into the TX slot, ring the doorbell, then watch the RX
		 * slot (and INTSR1) for a reply whose seq matches. */
		if (++seq_num >= 64)
			seq_num = 1;
		cmd[0] |= seq_num << SEQ_SHIFT;
		for (w = 0; w < 4; w++)
			sram_wr(c.tx_base + 4 * w, cmd[w]);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(MBOX + INTGR0, (1u << c.id) << 16);

		uint64_t bend = ticks() + (TIMER_HZ / 1000) * timeout_ms;
		while (ticks() < bend) {
			if (((sram_rd(c.rx_base) >> SEQ_SHIFT) & 0x3f) == seq_num ||
			    (rd32(MBOX + INTSR1) & (1u << c.id))) {
				for (w = 0; w < 4; w++)
					cmd[w] = sram_rd(c.rx_base + 4 * w);
				wr32(MBOX + INTCR1, 1u << c.id);
				return 0;
			}
		}
		for (w = 0; w < 4; w++)
			cmd[w] = sram_rd(c.rx_base + 4 * w);	/* last RX contents, for the log */
		return -15;
	}

	uint32_t front = sram_rd(c.tx_front), rear = sram_rd(c.tx_rear);
	uint32_t next = (front + 1) % c.len;
	if (front >= c.len || rear >= c.len)
		return -11;
	if (next == rear)
		return -12;				/* queue full */

	if (++seq_num >= 64)
		seq_num = 1;
	cmd[0] |= seq_num << SEQ_SHIFT;
	uint32_t slot = c.tx_base + c.size * front;
	for (w = 0; w < c.size / 4; w++)
		sram_wr(slot + 4 * w, w < 4 ? cmd[w] : 0);
	__asm__ volatile("dsb sy" ::: "memory");
	sram_wr(c.tx_front, next);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(MBOX + INTGR0, (1u << c.id) << 16);

	uint64_t end = ticks() + (TIMER_HZ / 1000) * timeout_ms;
	while (ticks() < end) {
		uint32_t f = sram_rd(c.rx_front), r = sram_rd(c.rx_rear), k;
		if (f >= c.len || r >= c.len)
			return -13;
		for (k = r; k != f; k = (k + 1) % c.len) {
			uint32_t e = c.rx_base + c.size * k;
			if (((sram_rd(e) >> SEQ_SHIFT) & 0x3f) == seq_num) {
				for (w = 0; w < 4; w++)
					cmd[w] = sram_rd(e + 4 * w);
				/* dequeue this entry (move rear's entry into its slot, as Linux does) */
				if (k != r) {
					uint32_t er = c.rx_base + c.size * r;
					for (w = 0; w < c.size / 4; w++)
						sram_wr(e + 4 * w, sram_rd(er + 4 * w));
				}
				r = (r + 1) % c.len;
				sram_wr(c.rx_rear, r);
				if (r == sram_rd(c.rx_front))
					wr32(MBOX + INTCR1, 1u << c.id);
				return 0;
			}
		}
	}
	return -14;					/* timeout */
}

/*
 * Load and attach the DVFS plugin, as Samsung's plugins_init() does (drivers/soc/samsung/acpm/acpm.c):
 * the APM's table lists plugin 3 = exynos8895_acpm_dvfs.fw (base 0x3801, size 0x5c00, attached=0,
 * stay_attached=1) sharing its SRAM region with plugin 2 (attached=1, stay_attached=0).
 * Sequence: detach plugin 2 (DP_D | id<<26), copy the blob to SRAM + (base & ~1),
 * attach plugin 3 (DP_A | id<<26), all on the framework channel (4). Returns 0 on success.
 */
#define DP_A		(1u << 25)
#define DP_D		(1u << 24)
#define PROTO_ID	26
#define P_SIZE		44

extern const uint32_t acpm_dvfs_fw[];
extern const uint32_t acpm_dvfs_fw_end[];

static uint32_t plugin_attached(uint32_t tab, uint32_t i)
{
	return sram_rd(tab + P_SIZE * i + 28) & 0xff;
}

int acpm_attach_dvfs(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint32_t tab = sram_rd(INITDATA + 0), n = sram_rd(INITDATA + 4);
	uint32_t fw_words = (uint32_t)(acpm_dvfs_fw_end - acpm_dvfs_fw), w;
	uint32_t p2 = tab + P_SIZE * 2, p3 = tab + P_SIZE * 3, base;
	uint32_t cmd[4];
	int rc;

	out("ACPM attach DVFS plugin\n");
	if (n < 4 || !ok_off(tab, P_SIZE * n))
		return -1;
	kv("fw bytes", fw_words * 4);
	if (sram_rd(p2 + 0) != 2 || sram_rd(p3 + 0) != 3 || sram_rd(p3 + 4) != 0x3801 ||
	    sram_rd(p2 + 4) != 0x3801 || sram_rd(p3 + 32) != 0x5c00 || fw_words * 4 != 0x5c00 ||
	    (sram_rd(p3 + 36) & 0xff) != 1) {
		out("plugin table does not match expectations, not touching it\n");
		return -2;
	}
	if (plugin_attached(tab, 3)) {
		out("already attached\n");
		return 0;
	}
	if (plugin_attached(tab, 2)) {
		cmd[0] = DP_D | (2u << PROTO_ID);
		cmd[1] = cmd[2] = cmd[3] = 0;
		rc = acpm_send(4, cmd, 300);
		kv("detach p2 rc", (uint64_t)(int64_t)rc);
		kv("resp0", cmd[0]);
		kv("p2 attached", plugin_attached(tab, 2));
		out("\n");
		if (rc || plugin_attached(tab, 2))
			return -3;
	}
	base = sram_rd(p3 + 4) & ~1u;
	for (w = 0; w < fw_words; w++)
		sram_wr(base + 4 * w, acpm_dvfs_fw[w]);
	__asm__ volatile("dsb sy" ::: "memory");
	for (w = 0; w < fw_words; w++) {
		if (sram_rd(base + 4 * w) != acpm_dvfs_fw[w]) {
			kv("verify mismatch at word", w);
			return -4;
		}
	}
	out("fw copied + verified\n");
	cmd[0] = DP_A | (3u << PROTO_ID);
	cmd[1] = cmd[2] = cmd[3] = 0;
	rc = acpm_send(4, cmd, 500);
	kv("attach p3 rc", (uint64_t)(int64_t)rc);
	kv("resp0", cmd[0]);
	kv("p3 attached", plugin_attached(tab, 3));
	out("\n");
	return rc ? -5 : (plugin_attached(tab, 3) ? 0 : -6);
}

/*
 * The APM's frequency/voltage map sits right after the DVFS plugin in SRAM:
 * fvmap = (plugin3.base & ~1) + plugin3.size. struct fvmap_header is 24 bytes, one per DVFS
 * domain (idx = low 16 bits of ACPM_DVFS_*): word0 {type, num_of_lv, members, pll}, word2 {init_lv,...},
 * word5 {o_ratevolt, o_tables}; rate_volt table = fvmap + o_ratevolt, entries {rate kHz, volt uV}.
 */
static int fv_table(uint32_t dom, uint32_t *nlv, uint32_t *init_lv, uint32_t *tab)
{
	uint32_t t = sram_rd(INITDATA + 0), p3 = t + 44 * 3;
	uint32_t fv = (sram_rd(p3 + 4) & ~1u) + sram_rd(p3 + 32), hdr = fv + 24 * dom;

	if (!ok_off(fv, 24 * (dom + 1)))
		return -1;
	*nlv = (sram_rd(hdr + 0) >> 8) & 0xff;
	*init_lv = sram_rd(hdr + 8) & 0xff;
	*tab = fv + (sram_rd(hdr + 20) & 0xffff);
	if (!*nlv || *nlv > 32 || !ok_off(*tab, 8 * *nlv))
		return -2;
	return 0;
}

void acpm_fvmap_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *))
{
	uint32_t dom, j, nlv, init, tab;

	out("ACPM fvmap\n");
	for (dom = 2; dom <= 3; dom++) {
		int rc = fv_table(dom, &nlv, &init, &tab);

		kv("domain", dom);
		kv("rc", (uint64_t)(int64_t)rc);
		if (rc) {
			out("\n");
			continue;
		}
		kv("levels", nlv);
		kv("init_lv", init);
		out("\n");
		for (j = 0; j < nlv; j++) {
			kv(" kHz", sram_rd(tab + 8 * j));
			kv("uV", sram_rd(tab + 8 * j + 4));
		}
		out("\n");
	}
}

static void delay_ms(uint32_t ms)
{
	uint64_t end = ticks() + (TIMER_HZ / 1000) * ms;

	while (ticks() < end)
		;
}

/*
 * Request the level that is 'from_low' steps above the lowest one in the domain's table
 * (0 = lowest frequency). A request equal to the already configured rate is a no-op.
 */
int acpm_dvfs_level(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		    uint32_t domain, uint32_t from_low);

/*
 * Channel owned by the DVFS plugin (plugin id 3 -> owner mask 0x8). The firmware's channel table,
 * not Samsung's device tree (which says 5), is authoritative: on this phone's APM firmware the DVFS
 * plugin owns channel 3 (the APM log showed channel-5 doorbells reaching no plugin).
 */
static uint32_t dvfs_ch_override = 0xffffffffu;	/* boot-arg dvfsch=<n>; absent = firmware table lookup */

void acpm_set_dvfs_channel(uint32_t ch)
{
	dvfs_ch_override = ch;
}

static uint32_t dvfs_channel(void)
{
	uint32_t tab = sram_rd(INITDATA + 8), n = sram_rd(INITDATA + 24), i;

	if (dvfs_ch_override != 0xffffffffu)
		return dvfs_ch_override;
	if (n > 16 || !ok_off(tab, 72 * n))
		return 5;
	for (i = 0; i < n; i++)
		if (sram_rd(tab + 72 * i + 8) == (1u << 3) && sram_rd(tab + 72 * i + 12) == 2)
			return sram_rd(tab + 72 * i + 0);
	return 5;
}

/* Log a channel's queue pointers: tx = AP->APM, rx = APM->AP. */
static void ch0_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		     uint32_t id, const char *tag)
{
	uint32_t i, n = sram_rd(INITDATA + 24);
	struct chan c;

	if (n > 16)
		n = 16;
	out(tag);
	for (i = 0; i < n; i++) {
		if (get_chan(i, &c) || c.id != id)
			continue;
		kv(" ch", id);
		kv("txF", sram_rd(c.tx_front));
		kv("txR", sram_rd(c.tx_rear));
		kv("rxF", sram_rd(c.rx_front));
		kv("rxR", sram_rd(c.rx_rear));
		kv("INTSR1", rd32(MBOX + INTSR1));
		out("\n");
		return;
	}
	out(" channel not found\n");
}

/* req: ACPM_FREQ_REQ / ACPM_FREQ_GET. Logs the response words. */
int acpm_dvfs(void (*kv)(const char *, uint64_t), void (*out)(const char *),
	      uint32_t domain, uint32_t req, uint32_t khz)
{
	uint32_t cmd[4] = { domain, khz, req, 0 };
	uint32_t ch = dvfs_channel();
	int rc;
	uint32_t c0 = cmd[0], c1 = cmd[1], c2 = cmd[2];

	if (dvfs_ch_override != 0xffffffffu)
		ch0_dump(kv, out, ch, "CH0: req before");
	rc = acpm_send(ch, cmd, dvfs_ch_override != 0xffffffffu ? 300 : 150);
	if (dvfs_ch_override != 0xffffffffu) {
		kv("CH0: req cmd0", c0);
		kv("cmd1", c1);
		kv("cmd2", c2);
		kv("rc", (uint64_t)(int64_t)rc);
		kv("resp0", cmd[0]);
		kv("resp1", cmd[1]);
		kv("resp2", cmd[2]);
		kv("resp3", cmd[3]);
		out("\n");
		ch0_dump(kv, out, ch, "CH0: req after");
	}

	kv("dvfs ch", ch);
	kv("dvfs domain", domain);
	kv("req", req);
	kv("khz", khz);
	kv("rc", (uint64_t)(int64_t)rc);
	kv("resp0", cmd[0]);
	kv("resp1", cmd[1]);
	kv("resp2", cmd[2]);
	kv("resp3", cmd[3]);
	kv("INTSR1", rd32(MBOX + INTSR1));
	out("\n");
	return rc;
}

/*
 * Read-only control experiment for dvfsch=<n>: FREQ_GET for CPUCL1/CPUCL0/G3D on the override channel
 * (changes no hardware state), then 512 bytes of APM SRAM. Flushes the log before and after.
 */
void acpm_ch0_experiment(void (*kv)(const char *, uint64_t), void (*out)(const char *),
			 void (*flush)(void))
{
	static const uint32_t doms[3] = { ACPM_DVFS_IDX_CPUCL1, ACPM_DVFS_IDX_CPUCL0, ACPM_DVFS_IDX_G3D };
	uint32_t ch = dvfs_channel(), i, w;

	out("CH0: experiment begin\n");
	flush();
	for (i = 0; i < 3; i++) {
		uint32_t cmd[4] = { doms[i], 0, ACPM_FREQ_GET, 0 };
		int rc;

		ch0_dump(kv, out, ch, "CH0: before");
		rc = acpm_send(ch, cmd, 300);
		out(i == 0 ? "CH0: GET cpucl1" : i == 1 ? "CH0: GET cpucl0" : "CH0: GET g3d");
		kv(" rc", (uint64_t)(int64_t)rc);
		kv("resp0", cmd[0]);
		kv("resp1", cmd[1]);
		kv("resp2", cmd[2]);
		kv("resp3", cmd[3]);
		kv("kHz", cmd[1]);
		out("\n");
		ch0_dump(kv, out, ch, "CH0: after");
	}
	for (i = 0; i < 0x200; i += 0x40) {
		out("CH0: SRAM");
		kv("[off", i);
		for (w = 0; w < 16; w++)
			kv("", sram_rd(i + 4 * w));
		out("\n");
	}
	out("CH0: experiment end\n");
	flush();
}

/*
 * Request an exact rate (kHz), but only if the APM's own table for the domain contains it.
 * Returns 0 if sent, -1 if the domain table is missing, -2 if the rate is not an operating point.
 */
int acpm_dvfs_rate(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		   uint32_t domain, uint32_t khz)
{
	uint32_t nlv, init, tab, j;

	if (fv_table(domain, &nlv, &init, &tab))
		return -1;
	for (j = 0; j < nlv; j++) {
		if (sram_rd(tab + 8 * j) == khz) {
			kv("rate req kHz", khz);
			kv("uV", sram_rd(tab + 8 * j + 4));
			acpm_dvfs(kv, out, domain, ACPM_FREQ_REQ, khz);
			delay_ms(50);
			return 0;
		}
	}
	kv("rate not in table", khz);
	out("\n");
	return -2;
}

/* CMU_CPUCL1 PLL_CON0 (0x16900120): ENABLE b31, STABLE b29, M[25:16], P[13:8], S[2:0]; fin = 26 MHz. */
#define CL1_PLL_CON0	0x16900120UL

static uint32_t a53_pll_khz(uint32_t *con0)
{
	uint32_t c = rd32(CL1_PLL_CON0), m = (c >> 16) & 0x3ff, p = (c >> 8) & 0x3f, s = c & 7;

	*con0 = c;
	if (!p)
		return 0;
	return (uint32_t)(((uint64_t)26000 * m) / ((uint64_t)p << s));
}

/* Wait up to 300 ms for the PLL to read back within 2% of 'khz' and be STABLE. */
static int a53_wait_pll(uint32_t khz, uint32_t *got)
{
	uint64_t end = ticks() + (TIMER_HZ / 1000) * 300;
	uint32_t con0;

	do {
		*got = a53_pll_khz(&con0);
		if ((con0 & (1u << 29)) && *got * 100 >= khz * 98 && *got * 100 <= khz * 102)
			return 0;
	} while (ticks() < end);
	return -1;
}

int a53_set_freq(void (*kv)(const char *, uint64_t), void (*out)(const char *), uint32_t mhz)
{
	uint32_t nlv, init, tab, con0, cur, target = mhz * 1000, j, got;

	out("A53FREQ: begin\n");
	kv("A53FREQ: target kHz", target);
	if (fv_table(3, &nlv, &init, &tab)) {
		out("A53FREQ: no fvmap for CPUCL1, unchanged\n");
		return -1;
	}
	cur = a53_pll_khz(&con0);
	kv("A53FREQ: old kHz", cur);
	kv("con0", con0);
	out("\n");
	for (j = 0; j < nlv && sram_rd(tab + 8 * j) != target; j++)
		;
	if (j == nlv) {
		out("A53FREQ: target is not an APM table level, unchanged\n");
		return -2;
	}
	if (target * 100 <= cur * 102) {
		out("A53FREQ: already at or above target, unchanged\n");
		return 0;
	}
	/* The table is sorted high -> low; climb from the first level above the current rate to the target. */
	for (j = nlv; j-- > 0;) {
		uint32_t step = sram_rd(tab + 8 * j);

		if (step * 100 <= cur * 102 || step > target)
			continue;
		pmic_rails(kv, out, "A53FREQ before step");
		kv("A53FREQ: step kHz", step);
		kv("uV", sram_rd(tab + 8 * j + 4));
		out("\n");
		/* The APM plugin raises the rail before it retunes the PLL (assumed, not verified). */
		acpm_dvfs(kv, out, ACPM_DVFS_IDX_CPUCL1, ACPM_FREQ_REQ, step);
		if (a53_wait_pll(step, &got)) {
			kv("A53FREQ: step REJECTED, pll kHz", got);
			kv("con0", rd32(CL1_PLL_CON0));
			out("\n");
			pmic_rails(kv, out, "A53FREQ after reject");
			return -3;
		}
		kv("A53FREQ: step ok, pll kHz", got);
		kv("con0", rd32(CL1_PLL_CON0));
		out("\n");
		cur = got;
	}
	pmic_rails(kv, out, "A53FREQ final");
	kv("A53FREQ: new kHz", cur);
	out("\n");
	return 0;
}

/* CMU_CPUCL0 (Mongoose) PLL_CON0 0x16800120, same field layout as CPUCL1. */
#define CL0_PLL_CON0	0x16800120UL

static uint32_t mngs_pll_khz(uint32_t *con0)
{
	uint32_t c = rd32(CL0_PLL_CON0), m = (c >> 16) & 0x3ff, p = (c >> 8) & 0x3f, s = c & 7;

	*con0 = c;
	if (!p)
		return 0;
	return (uint32_t)(((uint64_t)26000 * m) / ((uint64_t)p << s));
}

/* Wait up to 300 ms for CPUCL0 PLL STABLE (b29) and a rate within 2% of 'khz'. */
static int mngs_wait_pll(uint32_t khz, uint32_t *got)
{
	uint64_t end = ticks() + (TIMER_HZ / 1000) * 300;
	uint32_t con0;

	do {
		*got = mngs_pll_khz(&con0);
		if ((con0 & (1u << 29)) && *got * 100 >= khz * 98 && *got * 100 <= khz * 102)
			return 0;
	} while (ticks() < end);
	return -1;
}

int mngs_set_freq(void (*kv)(const char *, uint64_t), void (*out)(const char *), uint32_t mhz,
		  void (*flush)(void))
{
	uint32_t nlv, init, tab, con0, cur, target = mhz * 1000, j, got;

	out("MNGS: begin\n");
	kv("MNGS: target kHz", target);
	out("\n");
	if (fv_table(ACPM_DVFS_IDX_CPUCL0, &nlv, &init, &tab)) {
		out("MNGS: no fvmap for CPUCL0, unchanged\n");
		flush();
		return -1;
	}
	for (j = 0; j < nlv; j++) {
		kv("MNGS: lvl MHz", sram_rd(tab + 8 * j) / 1000);
		kv("mV", sram_rd(tab + 8 * j + 4) / 1000);
		kv("idx", j);
		out("\n");
	}
	kv("MNGS: init_lv", init);
	cur = mngs_pll_khz(&con0);
	kv("pll kHz", cur);
	kv("pll_con0", con0);
	out("\n");
	for (j = 0; j < nlv && sram_rd(tab + 8 * j) != target; j++)
		;
	if (j == nlv) {
		out("MNGS: target is not a CPUCL0 table level, unchanged\n");
		flush();
		return -2;
	}
	pmic_rails(kv, out, "MNGS before");
	flush();
	/* Table is sorted high -> low: climb from the lowest level up to the target, never above. */
	for (j = nlv; j-- > 0;) {
		uint32_t step = sram_rd(tab + 8 * j);

		if (step > target)
			continue;
		kv("MNGS: step kHz", step);
		kv("uV", sram_rd(tab + 8 * j + 4));
		out("\n");
		flush();
		if (acpm_dvfs(kv, out, ACPM_DVFS_IDX_CPUCL0, ACPM_FREQ_REQ, step))
			out("MNGS: request not accepted (rc != 0)\n");
		else
			out("MNGS: request accepted\n");
		if (mngs_wait_pll(step, &got)) {
			kv("MNGS: PLL NOT STABLE, pll kHz", got);
			kv("con0", rd32(CL0_PLL_CON0));
			out("\n");
			pmic_rails(kv, out, "MNGS after fail");
			mngs_clk_dump(kv, out, "mngs fail");
			flush();
			return -3;
		}
		kv("MNGS: PLL LOCKED, pll kHz", got);
		kv("con0", rd32(CL0_PLL_CON0));
		out("\n");
		pmic_rails(kv, out, "MNGS after step");
		flush();
	}
	return 0;
}

int acpm_dvfs_level(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		    uint32_t domain, uint32_t from_low)
{
	uint32_t nlv, init, tab, rate;

	if (fv_table(domain, &nlv, &init, &tab) || from_low >= nlv)
		return -1;
	rate = sram_rd(tab + 8 * (nlv - 1 - from_low));	/* table is sorted high -> low */
	kv("level req kHz", rate);
	acpm_dvfs(kv, out, domain, ACPM_FREQ_REQ, rate);	/* fire-and-forget: no reply is expected */
	delay_ms(50);
	return 0;
}
