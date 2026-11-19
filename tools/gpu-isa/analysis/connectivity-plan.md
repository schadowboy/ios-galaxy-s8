# Connectivity plan: Wi-Fi, Bluetooth, cellular on the S8 under iOS 16.7.16

Evidence tags: **[E]** read from a file (path given), **[I]** inference, **[G]** guess / needs verifying.
Nothing here touched the phone. Inputs read: iOS root image (mounted ro via `tools/cn_*.sh`), prelink info + kernelcache in
`/root/work/analysis/kc` (WSL), `analysis/kexts-d22ap.txt`, `analysis/d22ap-adt-tree.txt`, `analysis/s8-devices.txt`,
`samsung-src` (DT + CAL headers only), decoded stock DTs `analysis/kernel-re/sda7_*.dts`, stock kernel strings, `backup/sda10.img`.

## 0. Headline findings (read these first)

1. **The iPhone X does NOT use a BCM4361. It uses a BCM4357** [E: ADT `/arm-io/apcie/pci-bridge3/wlan compatible=wlan-pcie,bcm4357`,
   dext/kernelcache strings `AppleBCMWLANChipManagerPCIe4357`, firmware dir `C-4357__s-B1`, BlueTool `BCM4357B1 Olympic ...`].
   The S8 has a **BCM4361** [E: stock kernel contains `dhd_check_module_bcm4361`, `dhd_parse_board_information_bcm4361`].
   iOS 16's Wi-Fi driver ships chip managers for **4350, 4355, 4357, 4364, 4378, 4387 only** [E: kernelcache class names
   `AppleBCMWLANChipManagerPCIe43xx`]. **There is no 4361 chip manager and no 4361 firmware in iOS.** The premise
   "iOS already ships the firmware for this exact chip" is false; it ships a sibling-generation chip's firmware.
2. **Apple kext binaries cannot be loaded into our kernel as they are.** The iPhone X kernelcache is an old-style *prelinked*
   single Mach-O (type EXECUTE, flags NoUndefs, kexts linked at fixed addresses, e.g. AppleBCMWLANCore at
   0xfffffff005475700 while kernel text is at 0xfffffff0071b8000) [E: `ipsw macho info`, `ipsw kernel kexts`]. Kernel-symbol
   references are resolved to absolute addresses at KC build time [I, standard kxld/prelink behaviour]. Our kernel is a plain
   xnu build with **no `__PRELINK_*` segments and no kext loading at all**; every driver so far is compiled in
   (`SweetKeyStore`/AppleKeyStore, `SweetWatchdog`, `SweetFramebuffer`, `SweetBattery`, `sweet_usb.c` ECM net) and matched through
   `iokit/KernelConfigTables.cpp` [E: upstream `xnu/patches/0124-dreamlte-s8.patch`, `NOTES.md` "no closed kexts needed"].
3. **Source availability** [E: GitHub apple-oss-distributions search 2026-11-12]: `IOPCIFamily` IS open; there is **no
   IO80211Family / AppleBCMWLAN / AppleBaseband / AppleBluetooth repo** (search "IO80211" returns nothing). Networking core
   (ifnet, Skywalk) is in xnu itself (Skywalk self-tests already pass in our boot [E NOTES.md 0020]).
4. **BT uses a plain UART tty**: BlueTool opens `/dev/uart.bluetooth` / `/dev/cu.bluetooth` and downloads firmware (HCD blobs are
   *embedded inside the BlueTool binary*, keyed by chip names). Provider kexts are tiny: `AppleSamsungSerial` (IONameMatch
   `uart-1,samsung`, 1.9 KB) + IOSerialFamily + `AppleBluetooth` (BTReset, matches ADT node `bluetooth`, 2 KB) [E].
   Cheapest of the three.
5. **Cellular: the S8 modem is Samsung Shannon (SIPC over shared DRAM + mailbox); iOS expects an Apple-qualified baseband over PCIe
   (QMI/MBIM-like Skywalk).** Full integration is not realistic; a reduced "data + SIM + SMS" path via a CommCenter replacement is a
   multi-month project. Do last.
6. **Recommended order: (a) Wi-Fi as a plain ifnet (dev channel: ssh without TWRP), (b) Bluetooth over UART,
   (c) real IO80211 integration, (d) cellular.** See section 6.

## 1. Wi-Fi

### 1.1 What iOS 16.7.16 uses on the iPhone X [E]
Kernelcache (prelinked, xnu-8796.142.1.703.8~1 RELEASE_ARM64_T8015, 239 kexts):
- `com.apple.iokit.IO80211Family` 1200.13.0 (470 KB), no personalities (library).
- `com.apple.driver.AppleBCMWLANCore` (535 KB). Personality `AppleBCMWLAN PCIe` (provider class `AppleBCMWLANBusInterfacePCIe`,
  category WiFiDriver, probe score 1000, `IOResourceMatch IOBSD`); an SDIO personality too (unused).
- `com.apple.driver.AppleBCMWLANBusInterfacePCIe` (86 KB). Three personalities: **`IOPCIDevice` with `IOPCIMatch = 0x000014e4&0x0000ffff`**
  (any Broadcom vendor id 14e4, any device id), `UART` (provider `AppleOnboardSerialSync`, IONameMatch `wlan`), and
  `AppleARMIODevice` (IONameMatch `wlan`). The device id is NOT matched; the chip manager reads the chip id from the backplane.
  Chip managers in the kexts: `AppleBCMWLANChipManagerPCIe4350/4355/4357/4364/4378/4387`.
- Links: IOPCIFamily, **IOSkywalkFamily**, IONetworkingFamily, IOTimeSyncFamily, CoreAnalytics, AppleOLYHAL, AppleOnboardSerial,
  AppleEmbeddedPCIE, AppleARMPlatform, AppleMobileFileIntegrity, corecapture, IOImageLoader, corecrypto.
- `AppleEmbeddedPCIE` (23 KB library) + `AppleT8015PCIe` (6 KB: controller `apcie,t8015`, `apcie-bridge` = `ApplePCIEHostBridge`, DART per port)
  = the **A11 PCIe host side**; on the S8 we must supply our own.
- `BCMWLANFirmware4357.Hashstore` (13 KB): firmware hash store via IOImageLoader (consumer `com.apple.driver.AppleBCMWLANCore`); verifies, does not contain the firmware.
- iOS 16 also ships a **DriverKit** variant: `/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext` (4 MB, contains `AppleBCMWLANChipManagerPCIe4357`)
  and `BroadcomWLANDriverKit.framework`. Which one drives the iPhone X in 16.7 is [G]; `wifiFirmwareLoader` references both `WiFiUserClientPrepareMappingDK` and `WiFiUserClientPrepareMapping`.
- Userland: `wifid` (/usr/sbin) -> IO80211.framework -> `Apple80211*` ioctls on the IO80211 interface (en0); `wifiFirmwareLoader`, `wifianalyticsd`, `wifip2pd` (AWDL), `wifivelocityd` (/usr/libexec).

### 1.2 Firmware / NVRAM / CLM / txcap and delivery [E]
Location: `/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4357__s-B1/` (`/usr/share/firmware/wifi` does NOT exist; `/usr/share/firmware` only has `isp`; wifiFirmwareLoader lists `/usr/share/firmware/wifi/` as a fallback):

| file | size | purpose |
|---|---|---|
| `herculese.trx`, `herculese-JP.trx`, `herculesk.trx` | 977,677 B | firmware (first words `19f2 50ba`: not a plain Broadcom TRX header) |
| `*.clmb` | 8.1-8.3 KB | CLM regulatory blob |
| `*.txcb` | 630 B | txcap table |
| `P-herculese_M-GINS_V-{m,u}__m-{4.3,5.1,5.3,6.1}.txt` (+JP, herculesk variants) | 6.4 KB | board NVRAM text (`devid=0x440a vendid=0x14e4 xtalfreq=37400 sromrev=11 boardtype=0x081b ccode=XZ`) |
| `PlatformConfig/iOS/herculese-PlatformConfig.plist` | 354 B | ring sizes (rx/tx/llw 256) |

Delivery: `wifiFirmwareLoader` resolves the paths ("Resolved to Firmware/NVRAM/TxCap file path", NVRAM via `wifi-nvram-path`) and hands firmware+nvram+regulatory+signature buffers to the driver through an IOKit user client. The driver then writes the image to chip RAM (halt ARM core, copy image, append NVRAM, release; set up msgbuf rings in host memory) [I; standard dhd sequence]. The Hashstore kext checks it (Apple-signed) [E partial].
**4357 firmware will not run on 4361 silicon** [I, high confidence]. We need Samsung's 4361 firmware.

### 1.3 S8 side [E unless marked]
- Chip BCM4361 (Wi-Fi + BT) [E stock kernel strings]. GPS is NOT in it: separate BCM4753 (`analysis/s8-devices.txt`; stock DT reserved `gnss_if` 0xffc00000+4 MB); sensor hub BCM4773. Not BCM4375/47752.
- Stock Android driver is out-of-tree **bcmdhd (PCIe msgbuf)**: kernel strings `dhd_wifi_platform_load_pcie`, `dhd_prot_ring_attach`, `dhd_check_module_bcm4361`, `concate_revision_bcm4361`. Android file names in the stock kernel: `/vendor/etc/wifi/bcmdhd_sta.bin`, `bcmdhd_clm.blob`, `nvram_net.txt`; runtime `/data/vendor/conn/{rtecdc.bin,roml.bin,logstrs.bin}`.
- PCIe host: Exynos 8895 Synopsys DWC, **pcie0: elbi 0x116A0000, PHY 0x116D0000, PCS 0x116C0000 (0x1FC), sysreg 0x11421044 (0x30), DBI 0x11700000, config window 0x11BFF000, outbound mem 0x11800000+4 MB**, 1 lane, IRQ SPI 345, `ip-ver 0x889500`, `msi-base 200`, `ch-num 0`, **`use-cache-coherency="true"`**, `use-msi="false"` (legacy INTx), `use-sicd`. PERST **gpj1-2**, `WLAN_EN` **gpj1-3**, `PCIE_CLK_REQ_L` gpj1-0, WLAN host wake gpa0-7 [E `exynos8895.dtsi` 3665-3705, `dreamlte_common.dtsi` 243-253, `dreamlte_gpio_*.dtsi`].
- pcie1 (elbi 0x116B0000, DBI 0x11710000, cfg 0x11C00000, SPI 346, clock `gate_pciewifi1`, PERST gpj1-6): in the stock DT it also carries `shmem,ipc_size/zmb_*` props [E `sda7_22f1000.dts` ~8248]; role = [I] CP-side link on PCIe-modem variants; unused for Wi-Fi.
- Clock gates in our headers: `GATE_PCIE_PCIE0_MSTR/PCS/PHY/DBI`, `PCIE` (FSYS1) [E `include/dt-bindings/clock/exynos8895.h` 330-336], `VCLK_PCIE` in `cal-if/exynos8895/cmucal-vclk.h`.
- **Not in `samsung-src`**: `exynos-pcie.c` / PCIe PHY CAL (PHY/PCS init values, LTSSM sequence, L1SS), pinctrl bank/function definitions for gpj1/gpa0, bcmdhd platform glue (power delays), the WLAN rail list. These must come from the Samsung Open Source tarball for SM-G950F [G: path `drivers/pci/host/pcie-exynos.c`, `drivers/net/wireless/bcmdhd4361`], or RE of `analysis/kernel-re/sda7_Image.bin` (has `exynos_pcie_*` symbols).
- Power/clock [I, unverified]: WLAN_EN gpio = chip REG_ON; 32 kHz sleep clock from the PMIC (S2MPS17 CLK32K) and an IO rail; `samsung-src/drivers/regulator/s2mps17.c` exists but the WLAN rails are not named in the DT we have: MISSING.
- DMA: `use-cache-coherency="true"` suggests the PCIe master is I/O-coherent [I]; our UFS path assumes non-coherent (`flush_dcache64`, `sweet_ufs.c`). Verify with a ring test. No SMMU on this link (SMMU only cam/ISP [E mapping.md row 4]) so DMA addr = phys; the firmware uses 64-bit host addresses (DRAM at 0x80000000 / 0x880000000), fine.

### 1.4 What AppleBCMWLAN expects from the PCIe host [I]
- An `IOPCIDevice` provider: IOPCIFamily (open source, can be built in) + a host bridge (`IOPCIBridge` subclass, our analogue of `AppleT8015PCIe`/`ApplePCIEHostBridge`) publishing the device. Driver uses config read/write, BAR0 (32 KB backplane window repointed via config regs) and BAR2 (TCM), interrupt registration on the IOPCIDevice, `IODMACommand` for ring buffers (DART on Apple; 1:1 on S8 is fine).
- Link/PM (L1/L1.2) from `AppleEmbeddedPCIE`: we provide perst/refclk handling ourselves.
- UART/ARMIODevice personalities (`wlan` node) are debug/ADT-match paths; the ADT needs a `wlan` node.
- Regulatory: NVRAM `ccode` + CLM blob + wifid prefs; WirelessCoexManager talks to the baseband, which is absent (must not block).

### 1.5 Can we load the real kexts? Three vehicles
Evidence nothing loads kexts today: 137 patches, every driver built in (patches 0038, 0040, 0075, 0124...), matching only through `KernelConfigTables.cpp`, no `__PRELINK_INFO`.
- **A. Re-prelink:** extract each kext range from the iPhone X kernelcache, retarget all references to kernel symbols (iPhone X kernel symbol list `/root/work/ksyms.txt`, 70,677 symbols [E]) to our kernel's same-named symbols (same xnu-8796 family: 141.3 vs 142.1, layouts probably match [I]), add `__PRELINK_TEXT/__PRELINK_INFO` (Info dict already in `prelink_info.xml`). Problems: `__kalloc_type` views in kexts, AMFI/code-signing hooks, and ~15-20 dependent closed kexts (Skywalk, IONetworking, IOTimeSync, IOReport, CoreAnalytics, corecapture, AppleOLYHAL, AppleOnboardSerial...), Apple-private KPIs missing from open xnu. PAC is not an obstacle (kernelcache is plain arm64, cpusubtype 0 [E]). Effort 3-6 weeks to a "probe prints chip id", unbounded tail. Even then no 4361 chip manager, so a binary patch of the chip-id switch to alias 4361->4357 would be needed with unknown backplane/core map differences [G].
- **B. Built-in clean-room driver (recommended first):** like `sweet_usb.c`: PCIe bring-up + Broadcom msgbuf (open knowledge: Linux `brcmfmac` pcie.c/msgbuf.c plus Samsung's bcmdhd) + Samsung 4361 firmware, exposed as a raw `ifnet`. iOS sees Ethernet-like networking (configd/mDNSResponder handle it). Association via firmware (`wsec`/`sup_wpa` iovars) from a small helper or boot config. Estimate: PCIe+PHY+link 1-2 wk (with source), firmware+rings+data 2-3 wk, WPA2 +1 wk: **~4-7 weeks**.
- **C. IO80211 shim:** our own `IO80211Controller`/`Interface` look-alike publishing the IORegistry properties and ~40 `Apple80211` ioctls wifid uses so Settings works. IO80211Family is closed; only symbol strings in IO80211.framework document the ioctls. **4-8 weeks** after B. AWDL (AirDrop, wifip2pd) needs Apple firmware: will not work [I].

### 1.6 Still broken in iOS even after B/C [I]
AWDL/AirDrop/Personal Hotspot P2P, Wi-Fi Aware, Wi-Fi calling/WirelessCoex (need baseband), location-from-Wi-Fi, firmware-signature-dependent features; Wi-Fi in Settings until C.

### 1.7 Files
Have: iOS 4357 firmware set (key names/format reference), DT GPIO/PCIe data, stock kernel to RE.
**Missing (from the phone's Android SYSTEM/VENDOR, read-only via TWRP, nothing pulled now):** `/vendor/etc/wifi/` (`bcmdhd_sta.bin` + any `_b0/_b2` variants, `bcmdhd_apsta.bin`, `bcmdhd_clm.blob`, `nvram_net.txt`, `*.cal`, `.cid.info`), `/system/etc/wifi/`, `/vendor/firmware/`, `/system/vendor/firmware/`; EFS `wifi/.mac.info` from our `backup/sda3.img` (exists, UNREAD by me); Samsung kernel source tarball (online).

## 2. Bluetooth

### 2.1 iOS side [E]
- Daemons `bluetoothd`, `BlueTool` (/usr/sbin, `com.apple.BlueTool.plist`), `bluetoothuserd`, `bluetoothuiservice`; frameworks MobileBluetooth, BluetoothManager, CoreBluetooth.
- **Transport = tty `/dev/uart.bluetooth` + `/dev/cu.bluetooth`**, from ADT `/arm-io/uart1` with child `bluetooth compatible=bluetooth,n88` [E d22ap-adt-tree.txt]. Kexts: `AppleSamsungSerial` (IONameMatch `uart-1,samsung`, 1,911 B, needs IOSerialFamily + AppleOnboardSerial) and `AppleBluetooth` (`BTReset`, IONameMatch `bluetooth`, 2 KB).
- BlueTool options: `-b` baud, `-w file.hcd`, `-F` RAM image from `/etc/bluetool/` (directory absent in our root: firmware is **embedded in BlueTool**, chip tags "BCM4357B1 Olympic Hercules-E Murata", "BCM4355C0 ...", "BCM4350 UART 37.4 MHz ...", "BCM4345C1 ..."), `aci_init.hcd`/`aci_boot.hcd`.
- BT-over-PCIe path (`AppleConvergedTransport`) is for newer chips; ignore.
- **No BCM4361 in BlueTool's table** (only 4345C1, 4350, 4355*, 4357B1 found) [E]. It will not download firmware for an unknown chip.

### 2.2 S8 side
- UART1 @ **0x10830000** (samsung,exynos-uart, FIFO 256, SPI 389, clocks `GATE_UART_BT` + `UART_BT` PERIC1 +23/+2, `samsung,rts-gpio-control`, pinctrl `uart1_bus`, `_rts`, `_tx_dat`). BT_EN **gpj1-7**, BT_WAKE gpj1-4, BT_HOST_WAKE **gpa2-3** [E dts]. Stock node `samsung,bcm43xx`.
- Our S8 ADT (22 nodes) has no uart1/bluetooth; the kernel needs a built-in Exynos UART tty (Exynos UART code exists in `pe_serial.c` for the A5xelte port [E patches 0003/0124]). Apple's `AppleSamsungSerial` also needs closed IOSerialFamily, so re-implement built-in.
- Firmware: BCM4361 BT patchram `.hcd`, names [G] `/vendor/firmware/bcm4361*.hcd`, `/system/vendor/firmware/*.hcd` or `/vendor/etc/bluetooth/`. **Not in our files.**
- Fix for the chip table: (i) binary-patch BlueTool's table/hash, or (ii) our own `bt-init` that downloads the Samsung HCD (HCI_Reset, 0xFC2E Download_Minidriver, 0xFC4C Write_RAM, 0xFC4E Launch_RAM, 0xFC18 baud) and lets bluetoothd/BlueTool start with firmware already running. Apple-specific features (Handoff, Magic Pairing, AirPods) need Apple firmware: out. Classic A2DP/HFP + BLE basics should work [I].

### 2.3 Effort
B0 UART + HCI_Reset 3-5 d; B1 HCD + 3 Mbaud 3-5 d; B2 tty + BlueTool patch + bluetoothd sees controller 1-2 wk. **~3-4 weeks**, risk medium (LPO 32 kHz clock, flow control, sleep).

## 3. Cellular

### 3.1 S8 modem [E]
- Stock DT reserved memory: `modem_if` **0xF4C00000 size 0x9000000**, `cp_ram_logging` 0xFDC00000 (32 MB), `gnss_if` 0xFFC00000 (4 MB) [E sda7_22f1000.dts 80-95]. This **overlaps the 0xb0000000..0xfd000000 range XNU uses today** (NOTES.md): carve it out before any modem work.
- `mif_pdata` (`sec_modem,modem_pdata`): `mif,name="ss355ap"`, `ipc_version=0x32` (SIPC5), link `shmem` (`link_types=0x200`), 20 io devices incl. `umts_ipc0/1` (app "RIL"), `umts_rfs0` (RFS), `umts_csd`, `umts_router`, `umts_dm0` (DIAG), `rmnet*` (data). Shared buffer `buff_offset=0x500000, buff_size=0x900000`, second pool 0xe9000000+0x4000000. Mailbox @0x15B40000 (SPI 97): mbx ap2cp_msg(0), cp2ap_msg(1), ap2cp_united_status(2), cp2ap_united_status(3), mif_value(4), dvfs reqs(5-7), kerneltime(9); cp2ap interrupts msg/status/active/wakelock/ratmode. CP_REV GPIOs XGPIO32-34,38. Stock kernel functions `ss310ap_on/boot_on/boot_done/reset/dump_start`, `cp_boot_done`, `cp2ap_*` show the boot driver structure.
- Link is shared DRAM + AP-internal mailbox: no PCIe/USB between AP and CP [I].
- **`backup/sda10.img` (RADIO, 44,040,192 B) is a valid Samsung "TOC" modem image** [E]: header `TOC`, entries `BOOT` (off 0x200, load 0x40000000, size 0x1eb4), `MAIN` (off 0x20c0, ~38.5 MB, ARM vector table), `VSS` (off 0x24b4500, size 0x4842ec), `NV`, `OFFSET`; strings: `S355` x195, `G950FXXUCDUD1`. Also have `sda3.img` (EFS), `sdb.img`, `sdc.img` (likely modemst1/2).
- Stock boot sequence [I]: copy BOOT/MAIN/VSS/NV into the modem region, set PMU CP regs (release reset), mailbox handshake, `cp_boot_done`, SIPC5 link, IPC channels. A Samsung TEE/SMC step to verify/protect the CP region is likely [G]: first thing to test.

### 3.2 iOS side [E]
- Kexts: `AppleBasebandD20` (`baseband,n41`; iPhone X ADT node `/device-tree/baseband`), `AppleBasebandD101`, **`AppleBasebandPCI`** (IONameMatch `baseband-pcie`, IOPCIDevice, Skywalk), `AppleBasebandPCIICEControl/ICEPDP` (Intel), `AppleBasebandPCIMAVControl/MAVPDP` (Qualcomm, MHI/QMAP).
- Userland: `CommCenter` (+ MobileHelper/RootHelper), `atc`, `atcrtcomm`, CoreTelephony, AppleBasebandServices/Manager, TelephonyUtilities; baseband firmware is TSS/FDR-signed; ADT `baseband-vol`.
- CommCenter speaks Qualcomm QMI / Intel ATC-ETL; Shannon speaks Samsung RIL over SIPC5 (`umts_ipc0`): a different message set.

### 3.3 Honest feasibility
Full iOS cellular (voice, SMS, data, carrier bundles, eSIM, IMS/VoLTE, emergency calls) is **not realistic**: needs CP boot under a TEE we do not control [G], a Samsung RIL <-> CommCenter translator, Apple's IMS stack assumptions, emergency-call obligations. Reduced scope, by value:
1. CP boot + SIPC link + read modem/SIM state: 4-8 wk (needs Samsung `modem_ctl`/`link_device_shmem`/sipc5 sources from the kernel tarball).
2. Data only: `rmnet0` as `ifnet` + daemon doing RIL PDP activation: +4-6 wk.
3. CommCenter replacement serving the XPC interfaces CoreTelephony clients use (`com.apple.commcenter.xpc`, `com.apple.commcenter.atcs.xpc`): 6+ months; voice/IMS no.
Overall: data-only 2-3 months, UI-level cellular 6+ months, voice no.

## 4. S8 vs iPhone X summary

| item | iPhone X | S8 | bridge |
|---|---|---|---|
| Wi-Fi chip | BCM4357 B1, PCIe (`apcie` port 3) | BCM4361, DWC pcie0 | new host driver + 4361 support (not in iOS) |
| PCIe host | AppleT8015PCIe + ApplePCIEHostBridge + DART | DBI 0x11700000, cfg 0x11BFF000, mem 0x11800000 | built-in IOPCIBridge subclass, no IOMMU |
| BT | BCM4357 UART HCI, `/dev/uart.bluetooth` | BCM4361 UART1 0x10830000, BT_EN gpj1-7 | built-in UART tty + HCD download |
| Cell | Intel/Qualcomm over PCIe (port 2) | Shannon 355, mailbox 0x15B40000 + shared DRAM | translation layer |
| Firmware | 4357 in dext | Samsung 4361 on Android partitions | extract from SYSTEM |

## 5. Staged plans

### Wi-Fi
- **W0 (loader, 3-6 d; needs Samsung PCIe source/RE).** (1) FSYS1 clocks (`GATE_PCIE_PCIE0_MSTR/PCS/PHY/DBI`, `PCIE`; CMU regs in `cmucal-sfr.h`); (2) PMU/power domain (`pmucal_local_exynos8895.h`); (3) pinctrl: gpj1-0 CLKREQ, gpj1-2 PERST (low), gpj1-3 WLAN_EN, pcie_wake; (4) 32 kHz + IO rails (S2MPS17/S2MPB02; MISSING); (5) WLAN_EN high, wait 10-20 ms; (6) sysreg 0x11421044 RC mode/refclk, PHY 0x116D0000 + PCS 0x116C0000 init values (MISSING); (7) DBI 0x11700000 setup, ELBI 0x116A0000 LTSSM enable; (8) release PERST, poll L0; (9) config read through 0x11BFF000: vendor 0x14e4, device id [G, read it], class 0x0280. Deliverable: vendor/device id + BAR sizes in the log. Risk: S-Boot leaves PCIe off, PHY values unknown.
- **W1 (1 wk):** BAR windows, ChipCommon chipid (expect 0x4361), core scan, halt ARM core, upload `bcmdhd_sta.bin`, NVRAM at RAM end, release, find pcie_shared; INTx on SPI 345.
- **W2 (1-2 wk):** msgbuf rings, ioctl (`cur_etheraddr`), events, scan, join (wsec/sup_wpa).
- **W3 (1 wk):** `ifnet` attach (reuse `sweet_usb.c` glue), SSID/PSK from boot-arg/file.
- **W4 (later):** IO80211 shim.

### Bluetooth
- **B0:** BT_EN gpj1-7 (after LPO clock), UART1 clocks `GATE_UART_BT`/`UART_BT`, pinctrl, 115200 8N1, send `01 03 0C 00`, expect `04 0E 04 01 03 0C 00`; Read_Local_Version `01 01 10 00` (manufacturer 0x000F).
- **B1:** HCD download, baud 3M (0xFC18), BD_ADDR from EFS [G].
- **B2:** kernel tty `/dev/uart.bluetooth`, ADT `bluetooth` node, BlueTool table patch, run bluetoothd.

### Cellular
- **C0 (1-2 wk, may be TEE-blocked):** carve 0xF4C00000..0x100000000 region out of XNU; parse TOC from sda10; copy BOOT/MAIN/VSS/NV to CP load offsets (CP view 0x40000000 -> AP phys mapping [G], check `modem_ctl` source); release CP reset (PMU CP regs: MISSING sequence); mailbox ap2cp_msg(0) boot command; wait cp2ap_msg(1). Success: nonzero cp2ap_msg or log in `cp_ram_logging`.
- **C1:** SIPC5 shmem link, `umts_ipc0` RIL ping. **C2/C3:** per 3.3.

## 6. Order, effort, risk

| # | stage | effort | risk | gain |
|---|---|---|---|---|
| 1 | W0-W3 Wi-Fi as ifnet | 4-7 wk | high (PHY source, rails, 32k clock) | dev channel + real networking (USB ECM already exists, patches 0072-0081: do in parallel) |
| 2 | B0-B2 Bluetooth | 3-4 wk | medium | accessories; validates UART + LPO + PMIC knowledge |
| 3 | IO80211 shim | 4-8 wk | medium-high | Settings > Wi-Fi |
| 4 | Kext re-prelink (A) | 3-6 wk | high | only if many later drivers benefit; IOPCIFamily is open, so build it in instead |
| 5 | C0-C1 modem boot | 4-8 wk | very high (TEE, PMU) | proof of life |
| 6 | cellular data / CommCenter shim | 2-6 months | very high | reduced |

Do first: (i) fetch Samsung kernel/bcmdhd sources and the Android firmware set; (ii) PMIC rails + 32 kHz mapping (shared by Wi-Fi and BT); (iii) W0 probe in the loader (cheap failure, no XNU rebuild).

## 7. Files: have vs need (Android SYSTEM sda17 etc. reportedly intact on the phone [from user, not verified by me])

Have: iOS 4357 firmware/NVRAM/CLM/txcap, BlueTool with embedded BT firmware (4350/4355/4357/4345), kernelcache + `prelink_info.xml` + `ksyms.txt` (WSL `/root/work`), `backup/sda10.img` (valid Shannon TOC), `sda3.img` EFS (MAC/BT addr, nv data; unread), `sdb/sdc.img`, stock DTs + kernel images.
Need from the phone (TWRP read-only, e.g. mount sda17 ro, `adb pull`; nothing extracted now):
- Wi-Fi: `/vendor/etc/wifi/*`, `/system/etc/wifi/*`, `/vendor/firmware/`, `/system/vendor/firmware/`.
- Bluetooth: `*.hcd` (BCM4361) under `/vendor/firmware`, `/system/vendor/firmware`, `/vendor/etc/bluetooth`; `bt_vendor.conf`.
- Modem: `libsec-ril*.so` (RIL message set), modem configs under `/vendor/etc`, `/efs` (from sda3).
- Later: GNSS (BCM4753) and sensor hub (BCM4773 `ssp`) firmware, audio mixer paths, OIS (`ois_fw_*.bin`), touch firmware.
- Online: Samsung SM-G950F open-source tarball (`exynos-pcie.c`, PCIe PHY CAL, bcmdhd4361, `modem_ctl`, regulator DT).

## 8. Evidence vs guess
Evidence: 4357 vs 4361 chips, chip-manager list, IOPCIMatch, firmware files/sizes, NVRAM sample, PCIe/GPIO/UART/modem DT values, sda10 TOC, our kernel's no-kext-loading state, prelinked KC format, IOPCIFamily open source / IO80211 not.
Guesses: 4361 PCI device id; PHY/power sequence; WLAN rails and 32 kHz source; PCIe DMA coherence; TEE gating of CP boot and CP-to-AP address mapping; BT HCD names; BlueTool behavior on unknown chip; re-prelink details; Apple firmware ABI features (AWDL etc.).
