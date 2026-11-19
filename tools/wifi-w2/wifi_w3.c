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
