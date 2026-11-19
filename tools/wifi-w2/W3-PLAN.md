# W3a: radio up + scan over the MSGBUF ioctl channel

Status: code written, compiled with the loader flags (-Wall -Wextra -Werror, 0 ABS64 relocations, `tools/wifi-w2/_check.sh full`); not run on the phone.
Legend: [E] read in Samsung's bcmdhd4361 source (`ref/s8kernel-tgp-p/drivers/net/wireless/bcmdhd4361`), [A] assumption.

## Files and edit points
- `tools/wifi-w2/wifi_w3.c`: `wifi_w3()`; follows `wifi_w2.c` in the same translation unit (uses its statics). `wifi_w2.c` was generalised: `w2_ioctl(cmd, label, in, incopy, inlen, outlen, timeout, rs)`
  (W2a's `w2_iovar` is now a wrapper with identical wire behaviour), a response buffer is re-posted before the next request (as DHD does after every completion), and `w2_ready` is set at the end of the ring setup.
- `shim/dreamlte/smp_probe.c`: `_append.sh` appends wifi_w2.c + wifi_w3.c between the `BEGIN/END wifi_w2.c` markers (done; idempotent, replaces the previous copy).
- `shim/dreamlte/loader.c` (done): (1) `wifiprobe=` now accepts TWO digits (before, `wifiprobe=10` would have parsed as 1 = the read-only probe!); (2) `wp == 9 || wp == 10` runs `wifi_w2`, `wp == 10` then `wifi_w3`. `wifi_link` gets stage 10 (>= 8: stages 7 and 8 run).

## Build steps
1. `cp tools/build-v164.sh tools/build-v165.sh` (name as you like), change `wifiprobe=9` to `wifiprobe=10`, new image name.
2. Run it: it copies `shim/dreamlte/{start.S,smp_probe.c,loader.c}` from this worktree into /root/work/upstream and builds the boot image (nothing else to copy, wifi_w3 lives inside smp_probe.c).
3. Flash/run as for v164. Runtime: W2a ~1 s + country/mpc/UP ~1 s + 4 s scan wait + results (+ one 3 s retry) = about 10-15 s, far below 180 s.

## Sequence and why (each step justified from bcmdhd; nothing else is sent)
1. `WLC_GET_UP` (162) before: logs whether the radio is already up (read only).
2. `country` iovar (`WLC_SET_VAR` 263, `"country\0"` + `wl_country_t` {abbrev "DE", rev -1, ccode "DE"} = 12 bytes) [E dhd_linux.c:11300 `dhd_preinit_ioctls`, done while the radio is down, before WLC_UP].
   Best effort: the nvram already has a ccode, so a failure is only logged (change with `-DW3_COUNTRY='"US"'`). It fixes the channel list a scan uses.
3. `mpc 0` iovar (4-byte zero) [E dhd_linux.c:17656/17690: bcmdhd toggles `mpc`]. With mpc 1 the firmware powers the radio down while unassociated; best effort, logged.
4. `WLC_UP` (cmd 2) with a 4-byte zero [E dhd_cfg80211.c:193 `wl_dongle_up`]. This is the only mandatory prerequisite found in DHD's bring-up (country/roam/event_msgs are optional for an unassociated scan).
   300 ms later `WLC_GET_UP` must read 1.
   Skipped on purpose: `roam_off` (irrelevant unassociated), `event_msgs` (we post no event buffers), PM mode, band, scan channel time.
5. `WLC_SCAN` (cmd 50) with `wl_scan_params_t` [E wlioctl.h:671]: 64 bytes fixed, all fields below; channel list empty (`channel_num` 0 = every channel of the country; the console said RF band cap 5G only, so expect 5 GHz APs).
   offsets: ssid.len u32 @0 (=0, broadcast), ssid[32] @4, bssid @36 (ff:ff:ff:ff:ff:ff), bss_type s8 @42 (2 = DOT11_BSSTYPE_ANY), scan_type u8 @43 (0 = active default),
   nprobes s32 @44, active_time @48, passive_time @52, home_time @56 (all -1 = default), channel_num s32 @60 (0).
   input_buf_len = output_buf_len = 64 (DHD copies the same length both ways).
6. Wait 4 s busy-wait (fixed; the legacy scan has no completion we can see without event buffers).
7. `WLC_SCAN_RESULTS` (cmd 51): input 16 bytes with `buflen` = 8192 (what the wl utility passes), input_buf_len 2008 and output_buf_len 8192 exactly like DHD does for a 8 KiB query (DHD caps the request at 2048-40 bytes; the posted response buffer is 8192).
   If the count is 0 or the call failed: wait 3 s more and retry once.

Why not escan: it requires event buffers (`MSG_TYPE_EVENT_BUF_POST` on the control ring) and `WLC_E_ESCAN_RESULT` parsing; WLC_SCAN + WLC_SCAN_RESULTS needs none. If WLC_SCAN is refused/unsupported by this firmware the log shows the status; the escan path would be the follow-up (W3b).

## Structures parsed [E wlioctl.h]
`wl_scan_results_t` (version 109): buflen u32 @0, version u32 @4, count u32 @8, then `count` variable-length `wl_bss_info_t` records, each `length` bytes (do not assume a fixed size).
`WL_BSS_INFO_VERSION` = 109 in this tree (the firmware 13.38.63 predates version 109_1; the log prints the version of the first record). Offsets inside a record (natural alignment):
version u32 0, length u32 4, BSSID 8, beacon_period u16 14, capability u16 16, SSID_len u8 18, SSID[32] 19, bcnflags 51, rateset {count u32 @52, rates[16] @56}, chanspec u16 72, atim u16 74, dtim u8 76, accessnet u8 77,
RSSI s16 78, phy_noise s8 80, n_cap 81, nbss_cap u32 84, ctl_ch u8 88, vht maps 92/94, flags 96, vht_cap 97, basic_mcs[16] 100, ie_offset u16 116, ie_length u32 120, SNR s16 124.
Printed per BSS: index, channel (ctl_ch, else chanspec low byte), RSSI (dBm), security, first 3 BSSID octets (rest masked as xx:xx:xx), SSID.
Security from the IEs (walked with bounds checks, byte reads): RSN IE 48 -> AKM suites 00-0f-ac:{2,4,6} PSK, {1,3,5,11,12} EAP, {8,9} SAE (WPA3), 18 OWE; vendor IE 221 00:50:f2:01 -> WPA; none and capability bit 4 (privacy) -> WEP; else OPEN.
The first record also logs version, length, ie_offset, ie_length and raw words, so a wrong layout assumption is visible.

## RESULT line
`W3a RESULT bits (0 up, 1 scan started, 2 results ok, 3 >=1 BSS)`: bit0 = WLC_UP status 0 or GET_UP reads 1, bit1 = WLC_SCAN status 0, bit2 = SCAN_RESULTS status 0 with a header, bit3 = at least one BSS record parsed. 15 = full success.
At the end (and via `w2_dump_state` when any bit is missing: ring indices, MAILBOXINT, trap/assert) the firmware console since W3 start is printed; it usually names the reason of a refused command.

## How a failed log is read
| log | meaning |
|---|---|
| `W3: ABORT: W2a did not finish` | W2a rings not set up in this boot |
| a W3 step says `NO COMPLETION` | transport problem (see the `W2: ioctl TIMEOUT`, ring index and console lines), not an iovar problem |
| `status` -4 BCME_NOTUP on country/mpc/scan | radio not up yet: look at the WLC_UP status and GET_UP after |
| `country` fails but later steps work | nvram ccode is used; harmless |
| WLC_UP status != 0 (e.g. -1, -9) | firmware refused to bring the radio up: console tail (calibration, nvram, regulatory, RF) |
| WLC_SCAN status -23 (unsupported) | legacy scan not in this firmware: switch to escan (W3b: needs event buffers) |
| SCAN_RESULTS status 0, count 0 twice | radio up but nothing heard: antenna/regulatory (country), 5G only, scan restricted; try other country, `W3_SCAN_WAIT_MS` |
| count > 0 and "no record parsed" or implausible length | bss_info version/layout differs: read the first-record version and raw words |
| SCAN_RESULTS status BCME_BUFTOOSHORT (-14) | results larger than 8192: expected only in very crowded places; increase is not possible with DHD's 8 KiB, use escan |
| trap/assert flag in the state dump | firmware crashed on a command (trap_t words + console) |

## Assumptions
- A1: legacy `WLC_SCAN`/`WLC_SCAN_RESULTS` still work in the 13.38.63 STA firmware without the host posting event buffers (the firmware simply drops events when no buffers are posted).
- A2: `mpc` and `country` are accepted while the radio is down; failures are non-fatal.
- A3: the bss_info record is version 109 with the offsets above (ie_offset/ie_length at 116/120 are what Linux brcmfmac `struct brcmf_bss_info_le` also uses).
- A4: the 8 KiB response buffer is large enough for the scan; one scan on 5 GHz fits easily in a normal environment.
- A5: unassociated scanning needs only WLC_UP, no `event_msgs` mask, no `roam_off`.

# W3b: CLM blob download before country/up (after the v165 result)

v165 showed: transport fine; `country DE` -> -2 BADARG, `mpc 0` OK, WLC_UP status 0 but GET_UP 0, WLC_SCAN -26, WLC_SCAN_RESULTS -30 (version 109 confirmed, buflen 12, count 0).
Hypothesis (strong): this STA firmware has no built-in regulatory (CLM) data; bcmdhd's `dhd_preinit_ioctls` (dhd_linux.c:11130) calls `dhd_apply_default_clm` BEFORE the country set and BEFORE the radio is brought up.
Its country check (`dhd_check_current_clm_data`, dhd_common.c) treats ccode `#n` (WL_CCODE_NULL_COUNTRY) as "FW does not include CLM data". With no CLM no country is valid -> country BADARG, and the radio stays down.

## Encoding [E dhd_common.c:5105-5200, wlioctl.h:555-585]
- per chunk iovar `clmload` (WLC_SET_VAR 263): `"clmload\0"` + `wl_dload_data` + data. `struct wl_dload_data { u16 flag; u16 dload_type; u32 len; u32 crc; u8 data[]; }` (12 bytes header).
- flag = (DLOAD_HANDLER_VER=1) << 12 | DL_BEGIN 0x0002 (first chunk only) | DL_END 0x0004 (last chunk only) -> first 0x1002, middle 0x1000, last 0x1004 (0x1006 if one chunk). dload_type = DL_TYPE_CLM = 2. len = number of data bytes in this chunk. crc = 0 (DL_CRC_NOT_INUSE is not set by DHD either).
- chunk size MAX_CHUNK_LEN = 1408 (8*8*22); the payload after the name is padded to a multiple of 8 (`len = ROUNDUP(len, 8)`), input/output length = whole iovar buffer; every chunk is one WLC_SET_VAR. Blob 15988 bytes -> 12 chunks (11 x 1408 + 500).
- afterwards DHD checks `country` get: ccode must no longer be `#n`. On a failure it reads `clmload_status` (int). We also read `clm_ver` (text) on the way out.
- must happen while the radio is down: it is the first thing after W2a.

## country (-2 BADARG): encoding vs missing CLM
`wl_country_t` = { char country_abbrev[4]; s32 rev; char ccode[4] } = 12 bytes [E wlioctl.h:1144]; DHD sends exactly this (dhd_linux.c:11300; rev 0/-1 "-1 = unspecified on set"). Our 12-byte buffer (rev -1) matches, so the BADARG most likely came from the missing CLM (code not in the locale list).
The code logs `country` get before and after the CLM load and tries three set forms in order (rev -1, rev 0, abbreviation only 4 bytes) so the log tells whether the encoding matters. `W3_COUNTRY` default "DE" (CLM must contain it: log it; if rejected after a good CLM try "US" via `-DW3_COUNTRY='"US"'`).

## New order and bits
GET_UP -> country get -> clmload (12 chunks, each status checked) -> clm_ver -> country get -> country set -> mpc 0 -> WLC_UP + GET_UP (second attempt after 1 s) -> WLC_SCAN -> 4 s -> WLC_SCAN_RESULTS (+retry) -> console tail.
`W3a RESULT` bits: 0 up (now: GET_UP really reads 1, not just status 0), 1 scan started, 2 results ok, 3 >=1 BSS, 4 CLM loaded (all chunks status 0). Success = 31. No event buffers are posted (not needed for clmload/up/scan).

## Embedding and staging (exact steps)
- `shim/dreamlte/wifi_fw.S` (worktree) now also exports `wifi_clm` / `wifi_clm_end` (`.incbin "fw/wifi_clm.bin"`). `shim/dreamlte/fw/` is gitignored.
- Run once in WSL (it copies `firmware/samsung/wifi/bcmdhd_clm.blob` to `/root/work/upstream/shim/dreamlte/fw/wifi_clm.bin` and the worktree's `wifi_fw.S` over `/root/work/upstream/shim/dreamlte/wifi_fw.S`; `tools/wifi-fw-stage.sh` must have been run before, as for v132/v164):
  `wsl -u root -d Ubuntu -- bash /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/wifi-w2/wifi-clm-stage.sh`
- Then rebuild v165 with `wifiprobe=10` (build-v165.sh copies smp_probe.c + loader.c as before; the object list already contains wifi_fw.o, which now carries the CLM too; the link fails with "undefined reference to wifi_clm" if the staging script was not run on a tree whose wifi_fw.S is old).

## Reading a failed log
| log | meaning |
|---|---|
| `W3: no CLM blob embedded` / link error wifi_clm | staging step missing |
| `CLM chunk FAILED, offset N` + status | firmware refused chunk (status -2 BADARG: header encoding; -23: no clmload iovar in this fw; -14/-24: size), then `clmload_status` raw bytes (DHD: CHIPID_MISMATCH value) |
| clmload OK, `clm_ver` empty/failed | blob accepted but not applied; look at console |
| country get still `#n` after CLM | blob not for this chip/rev (bcmdhd_clm.blob vs bin_b0): try other country, check console |
| country OK, UP status 0, GET_UP 1 | radio is up: the scan lines follow |
| GET_UP still 0 after two UP attempts | something else blocks up (console tail: nvram/phy/cal errors); next suspects: `event_msgs`, `PM`, `wlc` mode flags |

# W3c: diagnostics, polling, and the event/escan path (after v166: radio up, legacy scan results empty)

v166: CLM loaded, country GB->DE OK, UP and GET_UP = 1, WLC_SCAN status 0, WLC_SCAN_RESULTS status 0 but count 0 after 4 s and 7 s; console: `dngl_sendpkt: dropped pkt`, `wl0 enable 1`, `wl1 enable 1` (rsdb).
`dngl_sendpkt: dropped pkt` is consistent with the firmware trying to hand a packet (an event) to a host that has not posted buffers.

## What W3c does (wifi_w3.c; wifi_w2.c got only an event hook + message-type counters + 8 event buffers in the DMA region: L_EVENT 0x18000, L_TOTAL 0x1C000)
1. Diagnostics, best effort, printed before the scan and after the legacy scan: WLC_GET_RADIO (37; bit0 sw disable, bit1 hw disable), WLC_GET_BAND (141 in this tree, not 62; 0 auto 1 5G 2 2G 3 all), WLC_GET_CHANNEL (29; channel_info {hw, target, scan}),
   WLC_GET_PM (85), iovar `scansuppress` (get), WLC_GET_VALID_CHANNELS (217; `wl_uint32_list {count, element[]}`: logs the list start and the number of 2.4 GHz and 5 GHz channels the CLM allows for DE), `country` get.
2. Legacy scan polled every 2 s (`W3_LEGACY_POLLS`, default 6 = 12 s; stops at count > 0), count printed each time.
   The totals are kept below ~40 s on purpose: an earlier note says a hang is cut by a watchdog after ~40 s and a busy loader loop might look the same; raise `-DW3_LEGACY_POLLS=10 -DW3_ESCAN_MS=15000` once the watchdog is known not to be kicked by the loader.
3. If the legacy count stays 0: event path (`w3_escan`):
   - 8 event buffers of 2048 bytes (DHD_FLOWRING_RX_BUFPOST_PKTSZ) posted on H2D ring 0 as MSG_TYPE_EVENT_BUF_POST (0x0D): the SAME 40-byte layout as the ioctl response buffer post (hdr, request_id = pktid 0x200+i, host_buf_len u16 @8, address @16) [E dhd_msgbuf.c dhd_prot_rxbufpost_ctrl: both use ioctl_resp_evt_buf_post_msg_t]. Each consumed buffer is re-posted.
   - `event_msgs`: get the 16-byte mask (WL_EVENTING_MASK_LEN), set bit 26 (WLC_E_SCAN_COMPLETE) and bit 69 (WLC_E_ESCAN_RESULT; mask[8] bit 5), set it back (dhd_linux.c:11671-11775).
   - iovar `escan` set: `"escan\0"` + `wl_escan_params_t` { u32 version = 1 (ESCAN_REQ_VERSION), u16 action = 1 (WL_SCAN_ACTION_START), u16 sync_id (0x1234), wl_scan_params_t (the same 64 bytes) } = 72 bytes.
   - D2H control ring pump (up to `W3_ESCAN_MS`, default 10 s, stops at WLC_E_STATUS_SUCCESS or SCAN_COMPLETE): MSG_TYPE_WL_EVENT (0x0E) items: `wlevent_req_msg_t` = hdr(8), status u16 @8, flow_ring_id u16 @10, event_data_len u16 @12, seqnum u16 @14, rsvd, marker;
     the frame sits at the start of the posted buffer (dma_rxoffset = 0 measured in the W2 log): ether header 14 (ethertype 0x886c at byte 12, BIG endian) + bcmeth_hdr 10 + `wl_event_msg_t` at byte 24, all fields BIG endian:
     version u16 @24, flags u16 @26, event_type u32 @28, status u32 @32, reason u32 @36, auth_type u32 @40, datalen u32 @44, addr @48, ifname[16] @54, ifidx @70, bsscfgidx @71; data from byte 72.
     For WLC_E_ESCAN_RESULT the data is little endian `wl_escan_result_t` { buflen u32 @72, version u32 @76, sync_id u16 @80, bss_count u16 @82, bss_info[] @84 } (same wl_bss_info v109 records, printed like before). Status 8 (PARTIAL) = a result, 0 (SUCCESS) = finished.
   - The first 30 events are logged (type, status, reason, datalen, event_data_len, ethertype) plus the first frame's raw words; per-type counts of all D2H control messages and of all WLC events are printed at the end.
4. RESULT bits (`W3a RESULT bits`): 0 up, 1 scan started, 2 results call ok, 3 legacy BSS found, 4 CLM loaded, 5 all 8 event buffers posted, 6 any event seen, 7 BSS found via escan events. (v166 was 0x17 = bits 0,1,2,4.)

## Reading the next log
| log | meaning |
|---|---|
| diagnostics: GET_RADIO != 0 | radio disabled by sw/hw state |
| valid channels: 5 GHz > 0 but 2.4 GHz = 0 (or count 0) | band/CLM restricts scanning; 5G-only matches the banner (wl0 5G-only); count 0 = CLM country has no channels (bad CLM/country pair) |
| scansuppress = 1 | scans are suppressed by the firmware (clear with `scansuppress 0`) |
| legacy poll counts all 0, escan BSS > 0 (bit7) | legacy scan needs the event path / was not the right API: use escan from now on |
| event buffers posted (bit5) but `event seen` 0 and no escan BSS | firmware did not deliver events: check `event_msgs set` status, the D2H type counts (an event would show as type 14) and the console |
| events seen, but no ESCAN_RESULT (type 69) among event types | event delivery works; escan produced SCAN_COMPLETE only: radio hears nothing (antenna/regulatory/band) |
| ESCAN_RESULT seen, `frame words` ethertype not 0x886c / implausible bss length | frame offsets wrong: the first-frame raw words are logged to correct them |
| `escan start` status -23 | firmware has no escan: only legacy remains |
| everything works but still 0 BSS in both | physical: antenna path / nvram RF parameters (5G-only banner), try `-DW3_COUNTRY` other, check `phy_noise` via console |
