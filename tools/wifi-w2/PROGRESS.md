# Wi-Fi (BCM4361, PCIe0) progress, Galaxy S8 iOS port

State as of 2026-11-19. Settings > Wi-Fi in iOS does NOT work yet: there is no network interface (en0) and no IO80211 driver in the kernel.
Everything below runs in the loader (before XNU), as bring-up experiments. All of it is reproducible with `wifiprobe=N` boot-args.

| Stage | boot-arg | Result |
|---|---|---|
| W0 | wifiprobe=3/4 | PCIe link trains, endpoint read (vendor 0x14e4, dev 0x441f) |
| W1 | wifiprobe=5..8 | BARs/ATU set, CR4 halted, Samsung `bcmdhd_sta.bin_b0` + nvram downloaded to TCM, firmware boots (`RTE (SDIO-MSG_BUF) 13.38.63 ... BCM4361 r3`) |
| W2a | wifiprobe=9 | Host side of the PCIe MSGBUF protocol: rings in loader RAM (0x8c000000), host-ready doorbell, ioctl on the control ring. Firmware answered `cur_etheraddr` and `ver`. Inbound DMA works with chip address == CPU physical. Code: `wifi_w2.c`, plan `W2-PLAN.md`, log `logs/loader-wifi9.txt` (not in git) |
| W3a | wifiprobe=10 | `WLC_UP`: status 0 but the radio stays down; `country` -> BCME_BADARG; scan -> -26 |
| W3b | wifiprobe=10 | Loading Samsung's `bcmdhd_clm.blob` (15988 B) via `clmload` (12 chunks of 1408, `wl_dload_data` flags 0x1002/0x1000/0x1004, type 2) fixes it: country reads GB then DE, `WLC_UP` -> GET_UP 1, scan accepted |
| W3c | wifiprobe=10 | Event path works (8 event buffers posted, `event_msgs`, `escan`; `WLC_E_ESCAN_RESULT` arrives after ~2.1 s) but the scan result is EMPTY (0 BSS), legacy and escan alike; GET_RADIO 0, band auto, channels hop; console only `dngl_sendpkt: dropped pkt`. The receiver hears nothing |
| W3d | wifiprobe=10 | Analysis: Samsung selects the NVRAM by the vendor ID in the chip's OTP/CIS (VID 0x22 = Murata r013/014/020 (identical files), 0x33 = SEMCO r01d/r01f/r02g/r01i/r02j: different boardtype, femctrl, swctrlmap, rxgains), not by androidboot.revision. Our `nvram.txt_murata_r020_b0` was a guess. New code reads the OTP before the firmware download, queries valid channels (1 KiB buffer), per-channel noise (`WLC_GET_PHY_NOISE`), passive escan, `btc_mode 0`. Plan `W3d-PLAN.md` |

## W3d result (run on the phone, image v168-wifi10d)
- OTP CIS read works: VID bytes `82 33` = SEMCO module, variant `r02j` (NOT Murata r020, which was a guess). `nvram.txt_r02j_b0` is in firmware/samsung/wifi.
- Valid channels for DE: 13 x 2.4 GHz + 24 x 5 GHz (CLM works). Noise floor -101 dBm on channels 1, 6, 11, 36 (the receiver hardware is alive). btc_mode 0 did not change the empty scan.
- Run with `nvram.txt_r02j_b0` staged with `tools/wifi-nvram-prep.py` into `shim/dreamlte/fw/wifi_nvram.bin` (image v169-wifi-r02j, log logs/loader-wifir02j.txt): **THE SCAN WORKS: 11 BSS found** by the legacy WLC_SCAN/WLC_SCAN_RESULTS (version 109 records, WPA2-PSK, channel 40 (5 GHz), RSSI about -78 dBm, including the user's own AP); noise floor -98 dBm. So the wrong NVRAM variant (Murata r020 instead of SEMCO r02j) was the cause of the deaf receiver. The Wi-Fi module is SEMCO (OTP VID `82 33`).

## Next (not started)
W4: join (SET_SSID / WPA2 key via 'wsec', 'wpa_auth', 'sup_wpa', 'wsec_pmk' iovars or the 4-way handshake in host: the firmware has an internal supplicant, 'sup_wpa 1' + PSK), DHCP needs a data path: tx/rx flowrings (flowring create via MSG_TYPE_FLOW_RING_CREATE, rx post ring) + the kernel driver (en0 ifnet) and the IO80211 family shim so Settings shows Wi-Fi. All of that must be moved from the loader into the XNU kernel (the loader exits at "JUMP TO XNU").

## Open
1. Deaf receiver: wrong NVRAM variant (check the OTP VID line `W3: OTP VID bytes`), BT coexistence (`btc_mode=1` with BT down), rsdb slice, or no networks in range during the test.
2. After scan works: join/auth, then data path (rx/tx flowrings), then the kernel driver (en0 ifnet, IOKit IO80211 family shim) so iOS Settings can see it. The loader-level code must be ported into the kernel; nothing of this exists there yet.

## How to run
`tools/build-v168.sh` pattern (copies the worktree's `shim/dreamlte/{start.S,smp_probe.c,loader.c}`, `wifiprobe=10`), `tools/wifi-w2/wifi-clm-stage.sh` stages the CLM, `tools/wifi-w2/_append.sh` re-appends `wifi_w2.c`/`wifi_w3.c` into `smp_probe.c`, `tools/wifi-w2/_check.sh full` compile-checks. Flash only BOOT with `tools/flash-boot.sh` (hash verified), reboot, pull `tools/pull-loaderlog.ps1`. Never put the MAC address, SSIDs/BSSIDs of neighbours or firmware blobs in git.
