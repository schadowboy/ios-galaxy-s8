# Wi-Fi stage W0: PCIe link bring-up to the BCM4361, from Samsung's own source

Source tree: `ref\s8kernel-tgp-p` (Samsung SM-G950F kernel, the one that was missing in `connectivity-plan.md` section 1.3).
Legend: [E] = read in that source (file:line), [G] = guess / still unknown. Paths below are relative to the tree.

## 1. Which controller and which pins [E]

- Wi-Fi uses **PCIe channel 0** (`pcie0@116A0000`, `arch/arm64/boot/dts/exynos/exynos8895.dtsi:3637`): enabled for dreamlte in `exynos8895-dreamlte_common.dtsi:243`. bcmdhd also says channel 0 on 8895: `drivers/net/wireless/bcmdhd4361/dhd_custom_exynos.c:93-98` (`SAMSUNG_PCIE_CH_NUM 0` for `CONFIG_SOC_EXYNOS8895`). pcie1 (`0x116B0000`) is not the Wi-Fi link.
- Register windows of pcie0 (DT 3641-3646): ELBI `0x116A0000` (0x1000), PHY `0x116D0000` (0x1000), sysreg `0x11421044` (0x30), DBI `0x11700000` (0x1000), PCS `0x116C0000` (0x1FC), config window `0x11BFF000` (0x1000); outbound memory `0x11800000` + 4 MiB; IRQ SPI 345; `ip-ver 0x889500`, 1 lane, legacy INTx (`use-msi = "false"`).
- Pins (`dreamlte_common.dtsi:249`, `exynos8895-pinctrl.dtsi:370-400`): **WLAN_EN gpj1-3** (function 1 = output), **PERST gpj1-2** (function 1 = output), **CLKREQ gpj1-0** (function 2), WLAN host wake gpa0-7 (function 0xf). pcie_wake pin: see `pcie_wake` in the same pinctrl file.
- GPIO bank: `gpj1` is the first bank (offset 0x000, 8 pins) of the **FSYS1 pin controller at `0x11430000`** (`drivers/pinctrl/samsung/pinctrl-exynos.c:1811-1815`). Standard Samsung layout inside a bank: CON +0x00 (4 bits per pin), DAT +0x04, PUD +0x08, DRV +0x0C, CONPDN +0x10, PUDPDN +0x14 [E for the layout from pinctrl-samsung; verify against a register read].

## 2. Clocks: none to program [E]

`pcie-clk-num = <0>` and `phy-clk-num = <0>` for pcie0 (DT 3664-3665), and `exynos_pcie_clock_get/enable` loop over exactly those counts (`drivers/pci/host/pci-exynos.c:363-443`). So Linux enables **no** gate clock for channel 0: S-Boot leaves the PCIe/FSYS1 clocks running (the socpll 100 MHz branch is also "already set in bootloader", `pci-exynos8895_cal.c:43`). pcie1 is the one with a gate (`gate_pciewifi1`).

## 3. The sequence (`exynos_pcie_poweron`, pci-exynos.c:1549-1681)

1. PMU (base `0x16480000`): clear the PCIe wake-up mask bit in `WAKEUP_MASK` (`0x610`, bit `WAKEUP_MASK_PCIE_WIFI + ch`) (:1561-1564; define pci-exynos.h:202). [G] exact bit number: read `WAKEUP_MASK_PCIE_WIFI` in pci-exynos.h.
2. Pinctrl default state: CLKREQ function 2, PERST output, WLAN_EN output (:1574; pins above). Chip power (REG_ON) is `gpio_direction_output(wlan_pwr_on, 1)` from bcmdhd (`dhd_custom_exynos.c:131-154`, with a `WIFI_TURNON_DELAY` msleep at :233).
3. PMU `PCIE_PHY_CONTROL` (`0x071C`, + 4 per channel) mask 0x1 := 1 (:1581-1583; pci-exynos.h:204-205): PHY power on.
4. `phy_all_pwrdn_clear` (`pci-exynos8895_cal.c:83-103`): PHY reg `0x20*4` := 0xC0 (ch0), `0xA7*4` := 0x7E.
5. `phy_config` (`pci-exynos8895_cal.c:145-229`), the full table is in that function:
   - sysreg `[0]` clear bit 1; sysreg `[0xC]` field update; ELBI `0x290`/`0x28C`/`0x288` reset toggles with 10 us delays;
   - PHY common block: 48 values written to `phy + i*4` (i = 0..47); transceiver block: 156 values to `phy + (0x30+i)*4`; tx amplitude `phy + 0x5C*4 := 0x14`; PCS `+0xF8 := 0x70`, `+0x100 := 0x87`, `+0x104 := 0x50`; PCS `+0xC` L1SS timeout bits; ELBI `0x290 := 1`, `0x28C := 1`; then both blocks written AGAIN ("Re-write at Kangchen only"). Values 26 MHz gen1, copied verbatim into the probe.
6. `establish_link` (pci-exynos.c:587-714): PCS `+0xEC` bit 3 := 1 (no rx-elecidle check during DBI access); ELBI `PCIE_SOFT_CORE_RESET (0x1D0)` 0 then 1 (20 us); **PERST high** then wait 18-20 ms; ELBI `0xF4` (APP_REQ_EXIT_L1_MODE) |= 1 | (1<<4); ELBI `0x1B8` := (1<<1); ELBI `0x2C8` (QCH_SEL) clear the three gating masks for ip-ver >= 0x889500; assert phy reset; ELBI `0x038 := 1`; `dw_pcie_setup_rc` + `exynos_pcie_setup_rc` (DBI programming, standard DesignWare); PCS `+0xEC` bit 3 := 0; **LTSSM enable**: ELBI `0x02C := 1`; poll ELBI `0x074 & 0x1f` for 0x0d..0x14 (up to 2000 x 10 us), else toggle PERST/LTSSM and retry (up to 10 times).
7. Link up: the endpoint's ID is read through the configuration window / viewport (`exynos_pcie_prog_viewport_cfg0`, pci-exynos.c:509): vendor 0x14e4 (Broadcom) expected.

## 4. Still unknown for a loader probe [G]

- Exact bit of the PMU wake mask, and the `pcie_wake` pin definition (read pci-exynos.h:260-270 and the pinctrl file).
- The standard `dw_pcie_setup_rc` + `exynos_pcie_setup_rc` register writes (pci-exynos.c:461-507 and the DesignWare core; the `pcie-designware.c` file is in the tree).
- Rails and sleep clock of the chip: WLAN_EN is the chip's REG_ON, but the 1.8 V / 3.3 V rails and the 32 kHz sleep clock (PMIC CLK32K) are not named in the DT: check `s2mps17` regulator names in the dreamlte DT (`vdd_*wifi*`) and whether S-Boot already leaves them on (stock Android sets them before the kernel; a loader that comes after S-Boot inherits whatever it left).
- Whether S-Boot leaves the PHY/PCS powered or in reset (read-only register dump first: costs nothing).

## 4b. RESULT (2026-11-15, phone in TWRP-reset test cycles, loader log pulled with tools\pull-loaderlog.ps1) [E: measured]

- v57/v58: read-only dump. **`PMU+0x720` read HANGS the bus** (removed). **PHY (0x116D0000) read hangs while the PHY is unpowered**; PCS reads 0xffffffff; ELBI, gpj1 and the FSYS1 sysreg are always readable. At hand-over gpj1 = all inputs (CON 0, DAT 0x23), `PCIE_PHY_CONTROL` 0, wake mask 0, sysreg 0x2 / 0x5d02, ELBI link 0, QCH 0x577.
- v59 (`wifiprobe=2`): pins + PMU `0x71C` bit0 + PHY power-down clear + full PHY/PCS tables: no hang, PHY[0x20] = 0xC0, PCS[0x100] = 0x87.
- v60 (`wifiprobe=3`): `wifi_link()` = Samsung's poweron + establish_link order (PERST high, 20 ms, **phy_config again after PERST**, QCH clear, DBI root-complex setup, LTSSM enable): **link up**: ELBI 0x74 final `0x3ace911` (state 0x11), DBI LNKSTA `0x7011` (2.5 GT/s, x1, DLL link active), first state change at 1 us, link at about 4.9 ms.
- v61 (`wifiprobe=4`): outbound ATU region 0 CFG0 over 0x11BFF000 targeting bus 1: **vendor 0x14e4 (Broadcom), device 0x441f, subsystem 0x4361:0x14e4, class 0x028000 revision 3**, BAR0 = 0x4 and BAR2 = 0x4 (64-bit memory, unassigned), capability pointer 0x48.
- So the chip is powered by WLAN_EN (gpj1-3) alone: the rails and the sleep clock were already on after S-Boot, no PMIC work was needed for the link.

## 5. Probe plan (loader, no XNU rebuild)

`wifiprobe=1` boot-arg in the loader, same style as `mngsexp`: (a) read-only dump of the registers above (GPJ1 CON/DAT, PMU 0x610/0x71C, sysreg, PCS/PHY/ELBI status, ELBI 0x074 link state); (b) if asked (`wifiprobe=2`), run section 3 steps 1-6 and report the final LTSSM state; (c) with the link up, read the vendor/device ID. Every step logs before it runs; a stuck PHY or a wrong rail shows as a link state, never as a hang. Next code step after this doc.

## W1b RESULT 2026-11-18 (wifiprobe=8, image v132-wifi8, logs/loader-wifi8.txt): FIRMWARE RUNS
Stage 8 from the loader: Samsung bcmdhd_sta firmware (0x33d41 words) downloaded to RAM 0x170000 with 0 mismatches, NVRAM (0x508 words) at the RAM end with token 0xfaf70508, reset instruction 0xb83ef170 at address 0, ARM CR4 released (IOCTRL 1). After 0x66 ms the last RAM word was replaced by the firmware's pcie_shared pointer 0x1fc8b4; the shared structure (flags 0x30040007, console buf 0x23f4fc, size 0x2000) shows the firmware console: "RTE (SDIO-MSG_BUF) 13.38.63 (B0 Network/rsdb) on BCM4361 r3", "pciemsgbuf0: Broadcom PCIE MSGBUF driver", "wl0: Broadcom BCM4361 802.11 Wireless Controller 13.38.63 (B0 Network/rsdb)", RF band cap 5G only. NEXT W2: the host side of the msgbuf protocol (rings, doorbells via PCIe mailbox, ioctl over the control rings: "ver", "cur_etheraddr", scan), then a kernel network interface (en0) in the kernel.
