# W3d: why is the receiver deaf? (after v167: control plane perfect, scan completes in ~2.1 s, count 0)

## 1. How Samsung picks the NVRAM variant [E bcmdhd4361 dhd_pcie.c:1918-2230, dhd_custom_cis.c:766-]
Not by androidboot.revision. For BCM4361 (`SUPPORT_BCM4361_MIXED_MODULES`, `USE_CID_CHECK`) the driver reads the chip's OTP CIS tuples through the backplane
(`dhd_parse_board_information_bcm4361`: write 0x18010324 (OTP control 1) = 0x00fa0000 and 0x18012618 (PMU min resource mask) = 0x0e4fffff, then read words 0x18011110..0x18011167),
finds the vendor-ID tuple (tag 0x81, 2 bytes) and the boardtype tuple (tag 0x1b), maps the VID through `vid_info[]` to a cid string, and the cid through `bcm4361_naming_table[]` to the file suffix.
Chip rev comes from ChipCommon (rev 3 = B0, `CHIP_REV_B0`): file `nvram.txt_<variant>` plus firmware `bcmdhd_sta.bin_b0`; only for rev A0 a boardtype switch (ePA/iPA) is used instead. The Android board revision (`system_rev`) only matters for the 4358 code.
VID {b0, 0x22} = Murata: 12 -> r012_a1, 13 -> r013, 14 -> r014, 15 -> r015, 20..24 -> r020..r024, 30..34 -> r030..r034 (all "1kl_b0"), 50 -> r020_1qw_b2 ...;
VID {b0, 0x33} = SEMCO: 51 r01d, 61 r01f, 62 r02g, 71 r01h, 81 r01i, 82 r02j, 91 r02a_b2, a1 r02b_b2; 30/31/32 r01a/r02a (A1).
=> the variant we load (murata_r020_b0) was a guess; the chip's own OTP names the real module. Implemented as experiment E1 (`wifi_w3_otp`).

## 2. NVRAM variants compared (firmware/samsung/wifi, B0 files)
- murata r013_b0, r014_b0, r020_b0 are byte-identical (diff): any Murata B0 variant is the same file. r012 is A1 (boardtype 0x082a).
- SEMCO files differ from Murata in: boardtype (r01d/r01f/r02g 0x082f, r01i 0x0835, r02j 0x0837 vs Murata 0x0834), boardrev (0x1101 vs 0x1103), `femctrl` (Murata 17; commented out in r01d),
  `swctrlmap_*` (RF-switch truth tables that route antennas to LNA/PA through the chip's own GPIOs; e.g. slice/1/swctrlmap_2g 0x00000802,0x50200000.. Murata vs 0x08020802,0x48020000.. SEMCO),
  `rssicorrnorm*` (8/5 vs 0), `AvVmid`, `pa2ga*/pa5ga*`, `cckpwroffset`, `*bw*po`, `clb*` (coex), r02j boardflags3 0x40002100.
- Common to all B0: sromrev 11, rxchain=txchain=3, aa2g=aa5g=3, antswitch=0, `bandcap=2` (slice 0 = 5G only) and `slice/1/bandcap=1` (slice 1 = 2G only; rsdb), `btc_mode=1`, boardflags 0x10401201, xtalfreq 37400, ccode GB.
- Parameters that make a RECEIVER deaf when wrong: `femctrl` / `swctrlmap_*` (switch routing: antenna terminated or on the wrong port, tens of dB loss), boardtype/boardflags (FEM type), `rxgains*elnagaina/trisoa/trelnabypa` (LNA states), `rxchain`.
  Wrong `pa*`, `maxp*`, `*po` only affect TX; `rssicorrnorm*` only shifts RSSI numbers.

## 3. Board-level pieces outside the NVRAM [E dts, dhd_custom_exynos.c]
- Samsung's wlan node (`exynos8895-dreamlte_common.dtsi:247`) has two GPIOs: WLAN_EN gpj1-3 and WLAN_HOST_WAKE gpa0-7 (plus pcie0). `dhd_wlan_power()` toggles only WLAN_EN. No antenna-switch/LNA/FEM GPIO, no regulator, no clock is controlled by the host: the FEM/antenna switch is driven by the chip's own GPIOs from the NVRAM.
- 32.768 kHz sleep clock: nothing in the wlan/pcie0 DT nodes or the DHD platform code references a PMIC 32k clock (the PMIC output is presumably on by default; not verified). It matters for low-power timing only (the chip has an internal LPO fallback). Our loader neither enables nor disables it (pmic.c has no wlan/32k code).
- Our loader sets gpj1 WLAN_EN/PERST, PMU 0x71c PHY power, PCIe PHY tables, link: the same as Samsung's `exynos_pcie_poweron`. Samsung toggles no other board resource for Wi-Fi.
- BT coexistence: every nvram has `btc_mode=1`. Samsung brings up the BT controller (bt_en, UART1) at boot; we do not. Floating BT-active/GCI lines can make the coex arbiter deny the shared antenna to WLAN.

## 4. Ranked hypotheses and experiments (all in ONE run, v168)
1. Wrong module variant (Murata vs SEMCO) -> wrong swctrlmap/femctrl -> RF path terminated. E1: the loader reads the OTP CIS before the firmware download (`wifi_w3_otp`, stage >= 9) and prints `W3: OTP VID bytes: xx yy  boardtype byte zz` plus the module family.
   Then stage that NVRAM (WSL): `python3 /mnt/c/github/ios-s8/tools/wifi-nvram-prep.py /mnt/c/github/ios-s8/firmware/samsung/wifi/nvram.txt_<variant> /root/work/upstream/shim/dreamlte/fw/wifi_nvram.bin` and rebuild
   (VID 81 33 -> nvram.txt_r01i_b0, 51 33 -> nvram.txt_r01d_b0, 61 33 -> nvram.txt_r01f_b0, 62 33 -> nvram.txt_r02g_b0, 82 33 -> nvram.txt_r02j_b0, 13/14/20 22 -> nvram.txt_murata_r0XX_b0, already r020).
   If yy = 22 (Murata) the variant is not the cause (all Murata B0 files are identical).
2. Nothing to hear: passive escan (receive only) and a noise-floor reading per channel. A working RX gives about -85..-100 dBm and hears any AP nearby. Put a phone hotspot on 2.4 GHz (channel 6) and one on 5 GHz (channel 36..48) next to the phone.
3. BT coexistence (btc_mode=1 with the BT side down): E3: before the event scans the run sets `btc_mode 0` (visible as `SET btc_mode 0`). If BSS appear only afterwards, coex was the cause; fix = btc_mode 0 (or bring up the BT controller) in the iOS Wi-Fi bring-up.
4. rsdb slices: wl0 5G-only, wl1 2.4 GHz; both enabled. The per-channel noise list shows whether one band is dead.
5. 32 kHz LPO / PMIC: unlikely (no host control in Samsung code). Only if all else is excluded: read the S2MPS17 32KHZ output registers from pmic.c.
6. PA/maxp/power-offset mismatch: TX only; cannot explain a deaf passive scan.

## 5. Code added
- `wifi_w3_otp()` (wifi_w3.c; called from `wifi_link` stage 8 right before the download when `stage >= 9`, i.e. wifiprobe=9/10): saves/restores the BAR0 window; read-only attempt at backplane 0x18011110 (22 words); if the first byte is not 0x80 it does DHD's two writes (0x18010324 := 0x00fa0000, 0x18012618 := 0x0e4fffff, the state DHD creates before every download) and reads again; prints raw words, VID, boardtype, module family.
  Risk: first use of those backplane addresses from the loader (each step flushed first; a hang returns to TWRP and the log names it).
- `WLC_GET_VALID_CHANNELS` now sends the list capacity (128) as its input word: the -14 BCME_BUFTOOSHORT was the missing capacity ("in - # of elements").
- `w3_noise()`: WLC_SET_CHANNEL (30) to 1, 6, 11, 36, 64, 100, 149, 165, 200 ms settle, WLC_GET_PHY_NOISE (135) in dBm.
- Passive escan (scan_type 1) when the active escan found nothing; `btc_mode 0` before the event scans; shorter defaults (legacy 3 polls = 6 s, active escan <= 5 s, passive <= 8 s; total run about 35 s).

## 6. Next image recipe (v168)
1. Copy `tools/build-v167.sh` (wifiprobe=10) to build-v168.sh; nothing to stage (nvram r020 and CLM already staged). `smp_probe.c` and `loader.c` come from this worktree (`_append.sh` already applied).
2. Run with 2.4 GHz and 5 GHz hotspots on next to the phone. Read: `W3: OTP VID bytes`, `valid channels`, per-channel noise, `PASSIVE escan` BSS lines, the `btc_mode` line, RESULT bits.
3. If the OTP names a different module than Murata r020: stage that NVRAM (command in 4.1), rebuild as v169, repeat.
