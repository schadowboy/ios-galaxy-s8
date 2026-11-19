# APM ignores DVFS requests: root cause analysis (static, 2026-11-11)

Legend: [E] = evidence (file:line or log line), [G] = guess / inference.
Sources: samsung-src/drivers/soc/samsung/{acpm,cal-if}, DTs in analysis/kernel-re (sda7_22f1000.dts = TWRP DT),
logs/loader-v23.txt, tmp-a53/acpm.c (= loader source), analysis/fw/dvfs_recovery.fw (disassembled with capstone, Thumb-2,
tools/dis_dvfs.py, output tmp-a53/dvfs.dis).

## 1. Root cause (rank 1): we send DVFS requests on the wrong IPC channel (3 instead of 0)

[E] Which channel does stock use for set_rate?
- `exynos_acpm_set_rate()` sends on `acpm_dvfs.ch_num` (cal-if/acpm_dvfs.c:33).
- `acpm_dvfs.ch_num` is filled in `exynos_acpm_dvfs_init()` (acpm_dvfs.c:296-299) by `acpm_ipc_request_channel(acpm_dvfs.dev, ...)`.
- `acpm_dvfs.dev` is set by `exynos_acpm_set_device(dev)` (acpm_dvfs.c:308-311), called from `cal_if_init(dev)` (cal-if.c:284,315).
- `dev` is the node of whoever calls cal_if_init. In the Samsung tree this is the clock controller
  (`clock-controller@15a80000`, compatible samsung,exynos8895-clock), the only node with `acpm-ipc-channel = <0>`
  (exynos8895.dtsi:611-615; TWRP DT sda7_22f1000.dts:4639-4643). [G, small]: the call to cal_if_init(np) lives in the clock
  driver (not in the tree we have); it is the only caller with such a node, and `acpm_ipc_request_channel` needs the property.
- The DT node `acpm_dvfs` (acpm-ipc-channel = <5>, exynos8895.dtsi:2458-2460) is used ONLY by `acpm_dvfs_probe()`
  (acpm_dvfs.c:249-270) to register `acpm_noti_mif_callback`: it is the APM -> AP notification channel (MIF QoS request),
  not the request channel. The DT also lists channel 1 for devfreq MIF/INT and the DVFS manager (dtsi:2511,2543,2640).
  So the earlier assumption "Samsung's DT says DVFS = channel 5" was a misreading; stock set_rate is on channel 0.

[E] What our firmware's channel table says (logs/loader-v23.txt:100-112):
```
ch0 type=1(QUEUE)  len=7 size=16 poll=1 owner=0x10   <- stock set_rate channel, owner mask 0x10 = plugin 4
ch1 type=1         len=3         poll=1 owner=0x10   <- devfreq MIF/INT, same plugin 4
ch2 type=1         len=3         poll=1 owner=0x20   <- PMIC/MFD (plugin 5)
ch3 type=2(BUFFER) len=1         poll=0 owner=0x8    <- what dvfs_channel() in our acpm.c picks (plugin 3 = the dynamic dvfs.fw)
ch4 type=1         owner=0x1 (framework)  <- loopback works
ch5/ch6 type=2 len=1 poll=0 owner=0x10       <- APM->AP notification slots
```
`dvfs_channel()` (tmp-a53/acpm.c, search "dvfs_channel") deliberately selects "owner == 1<<3 && type == 2", i.e. channel 3, because
the plugin-3 firmware log showed `ipc_dvfs` after a ch3 doorbell. That log line proves only that the framework dispatched
our ch3 interrupt to plugin 3, not that plugin 3 handles rate requests.

[E] What plugin 3 does with a ch3 interrupt (disassembly of dvfs_recovery.fw, base 0x3800):
- Plugin init at 0x3800 stores plugin_ops = 0x89c0; ops words at file offset 0x51c0: ipc_handler=0x3d71, irq_handler=0x3841,
  timer=0x383d (stub), extern=0x6675.
- `ipc_handler` (0x3d70..0x3e4a) NEVER reads its `cmd` argument (r0 is overwritten at 0x3db0) and never looks at the
  buffer slot. It reads the mailbox register 0x4004008c (= AP mailbox +0x8C = SR3) and compares it to the magic words
  0xabcd0000 / 0xabcd0001 / 0xabcd0002 (literal pool at 0x3e4c..0x3e64: 4004008c abcd0000 000093d0 40040008 deadbeef
  abcd0001 abcd0002). With SR3 = 0 (our loader never writes it) it falls into the "unknown" branch (0x3dc0..0x3de6: two
  debug-log calls 0x91 / 0x93, clears SR3, returns 0). A state machine keyed on SR3 magics is an init / calibration
  handshake (the blob also contains the strings "autocal", "plug_st."), not a rate-request path.
- This matches the log: after our request the APM prints `irq_num` + `ipc_dvfs` (dispatch) and ~0.7 ms later `ipc_dvfs` (handler
  done, val=0), PLL and BUCK3 rail (B3OUT=0x4a) unchanged (loader-v23.txt: "ipc_dvfs" entries e=0x28/0x29, "PMIC rails after dvfs req").
- Reading `rc=-15` (0xfff...f1): our TYPE_BUFFER branch waits for a reply in the RX slot / INTSR1 bit 3; plugin 3 never
  produces one. So -15 is the timeout for a message nobody parses. [E for the timeout, G that the handler is not the rate path]

[G, medium-high] Rate requests (cmd = {idx, kHz, FREQ_REQ, 0}) are processed by the resident plugin 4 (id 4, base 0x1a31,
attached, stay_attached; owner of channels 0, 1, 5, 6), reached via channel 0 (a QUEUE, len 7, poll=1, AP polls INTSR1 bit 0).
Plugin 3 (dvfs.fw) supplies the DVFS data (fvmap follows it in SRAM, acpm.c fv_table; stock `fvmap_init(fw_base + size)`, acpm.c:152)
and is why it must be attached, which we already do correctly.

## 2. Stock bring-up vs ours, step by step

(a) Stock order:
1. `acpm_ipc_probe` (arch_initcall, acpm_ipc.c:686-755): IRQ 39, map MBOX 0x16440000 + SRAM 0x16500000 (0xB400), initdata at
   SRAM+0x2850 (DT initdata-base), `log_buffer_init`, `channel_init`.
2. `channel_init` (acpm_ipc.c:638-684): channels = initdata->ipc_ap_max; per channel copy id/type/poll, RX = ch.tx_* (APM->AP),
   TX = ch.rx_* (AP->APM); `INTMR1 = mask of polling channels` (line 681, polling bit = 1 means masked/polled).
   Channel array is indexed by channel number (`channel[channel_id]`, line 476). Ours finds by id, equivalent here (id == idx).
3. `exynos_acpm_init` (acpm.c:374) registers the acpm driver; `exynos_acpm_binary_update` (fs_initcall_sync, acpm.c:380-390) sets
   BUSY_WAIT and runs `plugins_init()` (acpm.c:82-155): for each plugin with {attached=0,stay=1}: copy fw to SRAM base&~1,
   attach (cmd = DP_A | id<<26) on ch4; for {attached=1,stay=0}: detach. Then `fvmap_init`. Ours (acpm_attach_dvfs) matches,
   verified by the APM log (`dyn_plug`, `attachid 3`).
4. `fs_initcall exynos_acpm_dvfs_init` (acpm_dvfs.c:292-306) resolves the DVFS channel from the cal_if device (channel 0).
No further 'ready' flag, no timestamp handshake: the only SRAM write by the AP at init is the ktime slot (`timestamp_write`,
acpm_ipc.c:53-70, informational for APM log timestamps, not required for IPC).

(b) Init command before the first set_rate: none in this source. `DATA_INIT 5` and `POLICY_REQ 4` are defined
(acpm_dvfs.h:23-24) but have zero callers (grep over samsung-src). `exynos_acpm_set_rate` is called directly from
`cal_dfs_set_rate` (cal-if.c:49-50). The only extra DVFS commands in stock: MARGIN_REQ/COLDTEMP_REQ from TMU notifiers and
AUTOCAL_START (`exynos_acpm_vdd_auto_calibration`, cmd[0]=enable, cmd[2]=6). [E]

(c) Message encoding. Stock `acpm_ipc_send_data` (acpm_ipc.c:462-578): `cmd[0] |= (seq & 0x3f) << 16`, seq = 1..63 per channel,
16-byte element written at tx.base + size*front, cmd[1..3] zeroed in the caller copy, `front = (front+1) % len` written to tx.front, doorbell
`INTGR0 = (1<<ch) << 16`. Response: if `channel->polling && cfg->response`, spin on INTSR1 bit and `check_response()` scanning the
RX queue for the same seq (bits 21:16), dequeue by moving `rear`, clear INTCR1 when rear == front. Our queue branch
(acpm.c, "uint32_t front = ..." onward) is byte-for-byte the same protocol: same seq shift, same element layout, same doorbell,
same dequeue. [E] RSP/OWN/INDIRECTION bits (31/30/29) are not set by the AP for plain commands (acpm_ipc_ctrl.h:14-16). For a
non-polling channel (ch3, ch5, ch6 here, poll=0) stock does NOT wait at all (`if (channel->polling && ...)`, line 529) and returns 0, so
`exynos_acpm_set_rate` over a buffer channel could never see a reply either. Our TYPE_BUFFER branch has no stock counterpart:
stock `acpm_ipc_send_data` has no TYPE_BUFFER special case for TX (only `dequeue_policy` has one, line 314, for the RX notification path);
with len=1 the stock "buffer full" test (`tmp_index(0) != rear(0)`) would always time out. That is further proof stock never sends
requests on a len-1 buffer channel. [E]

(d) Why -15: we poll the RX slot of ch3 for a reply from a plugin that does not implement the request protocol (section 1).
Nothing about reply routing (RX queue vs slot, INTSR1 bit) is wrong in principle; on ch0 (QUEUE, poll=1) the reply is an entry
in the ch0 APM->AP queue with matching seq, and INTSR1 bit 0 is set. [G for the exact reply layout, E for the queue protocol]

## 3. Ranked causes

1. [E+G] Wrong channel: DVFS requests belong on channel 0 (owner plugin 4, QUEUE, polled), we use channel 3 (plugin 3 SR3-handshake handler). Confidence ~75%.
2. [G] If ch0 is also silent: plugin 4 may require the dvfs plugin (3) to have completed its SR3 handshake (SR3 = 0xabcd0000 / 1 / 2 states) after attach. In stock nothing visible in the AP source writes SR3 (grep "abcd" found nothing in samsung-src); so it
   is probably written by the bootloader or never needed. Test: write SR3 magic on ch3 and watch the plugin-3 log / state var at SRAM 0x93d0. Confidence ~10%.
3. [G] Requests ignored while the target cluster is off / unclocked: Mongoose PLL (CPUCL0) cannot be requested to change before cluster power sequencing; A53 requests should work already on CPUCL1. Not applicable to the A53 symptom. ~5%.
4. [G] Interrupt-mode mismatch: ch3 has poll=0 so APM->AP completions go through GIC 39 (unmasked in INTMR1); we poll INTSR1 on a channel the framework never answers. Subsumed by cause 1.
5. [G] Timing: attach_dvfs then immediate request: stock also does it back-to-back (ch4 attach, later set_rate), no issue seen. ~2%.

## 4. Mongoose PLL (CPUCL0 PLL_CON0 ENABLE=1, STABLE=0) (e)

[E] Stock cluster-0 power-on is only `cluster0_on[]` (pmucal_cpu_exynos8895.h:82-89): (1) PMU 0x16480000+0x3C20 bit0 = 1, wait 0x3C24
bit0 (QCH of embedded CMU_CPUCL0); (2) 0x45C0 bit0 = 1, wait 0x45C4 bit0 (CLKSTOP); (3) write 0x16840000+0x1000 bit0 = 1
(DD4_DD_DA_INPUT_0); (4) 0x2488 bit0 = 0 (CPUSEQUENCER_OPTION). Our log shows qch cfg/st = 1/1, clkstop 1/1, cl0 seq opt 0x1001 -> 0x1000
(bit0 cleared) and dd0=0x1 after CPU_ON: the same steps are done. No other register write (mux, sw_user, PLL enable) appears in
pmucal; PLL programming for CPUCL0 is a DVFS operation (ACPM_DVFS_CPUCL0 = idx 2, fvmap domain 2, init level 14 = 1066 MHz), i.e. done by the APM.
[G] Therefore the Mongoose PLL most likely stays unlocked because the APM never received a valid CPUCL0 request (same channel
bug) and because the PLL reference/lock needs the power sequencer + APM to program it; ENABLE=1 with STABLE=0 is the state the bootloader leaves
(PLL enabled but cluster-0 domain never brought through the APM's sequence). After fixing the channel, send CPUCL0 (idx 2)
FREQ_REQ 1066000 (init level) BEFORE CPU_ON and re-read PLL_CON0 (bit 29 STABLE).

## 5. Concrete changes for acpm.c

### Change 1 (top cause): send DVFS requests on the clock-controller channel 0 (queue, polled)

```diff
 /* req: ACPM_FREQ_REQ / ACPM_FREQ_GET. Logs the response words. */
 static uint32_t dvfs_channel(void)
 {
-	uint32_t tab = sram_rd(INITDATA + 8), n = sram_rd(INITDATA + 24), i;
-	if (n > 16 || !ok_off(tab, 72 * n))
-		return 5;
-	for (i = 0; i < n; i++)
-		if (sram_rd(tab + 72 * i + 8) == (1u << 3) && sram_rd(tab + 72 * i + 12) == 2)
-			return sram_rd(tab + 72 * i + 0);
-	return 5;
+	/* Stock: cal_if_init(clock-controller node) -> acpm_dvfs.ch_num = DT 'acpm-ipc-channel' of
+	 * clock-controller@15a80000 = 0 (type QUEUE len 7, polled, owner plugin 4). */
+	return 0;
 }
@@ int acpm_dvfs(...)
-	int rc = acpm_send(ch, cmd, 150);
+	int rc = acpm_send(ch, cmd, 300);
```
Keep the existing "FREQ_REQ then wait for PLL" flow in a53_set_freq(). For this channel a reply is expected (queue, polled), so
`rc` should become 0 and `resp0` should carry the seq in bits 21:16.

Log line proving it: `dvfs ch=0x0 dvfs domain=0x3 req=0x0 khz=0x19c990 rc=0x0 resp0=0x<seq>0003`, followed by
`A53FREQ: step ok, pll kHz=...` with CMU_CPUCL1 PLL_CON0 (0x16900120) M/P/S changing from 0xa0f30311 and `B3OUT` rising from 0x4a, and
the APM log showing a plugin-4 entry (a different tag than the bare `ipc_dvfs`).
If rc is still -14 (queue timeout), dump ch0 txF/txR/rxF/rxR before and after (acpm_dump) to see whether the APM consumed the entry
(txR advancing = consumed, plugin 4 not replying).

### Change 2 (cheap control experiments, same build): try both candidate channels and GET before REQ

```diff
+	/* experiment: FREQ_GET on ch0 for CPUCL1 first; reply cmd[1] = current kHz (stock exynos_acpm_get_rate) */
+	{
+		uint32_t g[4] = { ACPM_DVFS_IDX_CPUCL1, 0, ACPM_FREQ_GET, 0 };
+		int grc = acpm_send(0, g, 300);
+		kv("GET ch0 rc", (uint64_t)(int64_t)grc);
+		kv("GET kHz", g[1]);
+	}
+	/* if rc != 0 also try ch1 (devfreq/MIF-INT queue, same owner plugin 4) with domain 0 (MIF) */
```
Proves the channel when `GET ch0 rc=0x0` and `GET kHz` ~ 1053000 (the PLL reads 1053 MHz = init level 6). A reply on a GET is the cleanest
proof of the right channel because it changes no hardware state.
Second fallback if ch0 fails: write SR3 (MBOX+0x8C) = 0xabcd0000 then ring ch3 once and watch state var SRAM 0x93d0 (cause 2).

## 6. Not resolved / needs phone

- Which plugin (3 vs 4) really implements FREQ_REQ: we did not disassemble plugin 4 (it lives in the APM's resident
  image at SRAM 0x1a30 and is not in the repo; dump SRAM 0x0..0x9000 from the next loader run for this).
- Whether `cal_if_init` is really called with the clock-controller node (clk driver source not in samsung-src; strongly implied by DT).
