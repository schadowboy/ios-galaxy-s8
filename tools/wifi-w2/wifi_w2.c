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
