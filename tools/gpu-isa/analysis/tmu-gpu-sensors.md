# Exynos 8895 TMU: GPU and non-CPU sensors

DTS = ref/s8kernel-tgp-p/arch/arm64/boot/dts/exynos/exynos8895.dtsi ; DRV = ref/s8kernel-tgp-p/drivers/thermal/samsung/exynos_tmu.c

## Nodes
| node | tmu_name | base (size 0x400) | `sensors` mask | sensing_mode | cite |
|---|---|---|---|---|---|
| tmuctrl_0 | MNGS | 0x10080000 | 61 = 0b111101 (ids 0,2,3,4,5) | balance | DTS:4537-4549 [E] |
| tmuctrl_1 | APOLLO | 0x10080000 | 2 (id 1) | max | DTS:4551-4560 [E] |
| tmuctrl_2 | GPU | **0x10084000**, irq SPI 452 | **1 (sensor 0 of 2nd block)** | max | DTS:4562-4580 [E] |
| tmuctrl_3 | ISP | 0x10084000 | 6 = 0b110 (ids 1,2) | max | DTS:4582-4591 [E] |

The "earlytmu" node also maps 0x10080000 (DTS:4532-4535) [E]. It is not a separate sensor block.

## Registers (same layout in both blocks, base + offset) [E]
- TRIMINFO for sensor n: `base + 0x0 + 4*n`. temp_error1 = bits[8:0], temp_error2 = bits[17:9] (DRV:502-504; masks DRV:112-114).
- cal_type is read from block TRIMINFO(0) bit 23 (1 = two-point) (DRV:497-498, DRV:110-111). It is one value per block, taken from sensor 0's word.
- CURRENT_TEMP: 0x40 holds sensors 0 and 1 (bits[8:0] and [17:9]). 0x44 holds sensors 2,3,4. 0x48 holds sensors 5,6,7. Each field is 9 bits wide (DRV:66-68, DRV:944-953).
  - For n<2: reg 0x40, shift 9*n. For n>=2: reg 0x40 + ((n-2)/3+1)*4, shift 9*((n-2)%3).
- GPU: sensor 0 -> TRIMINFO @ 0x10084000, CURRENT_TEMP @ 0x10084040 bits[8:0] [E, derived from DTS:4568 + DRV:944-953].
- ISP: sensors 1,2 -> 0x10084040 bits[17:9] and 0x10084044 bits[8:0] (derived, [E]).
- TOTAL_SENSORS = 8 per block (DRV:149).

## Calibration [E]
- Two-point: T = (code - err1) * (85 - 25) / (err2 - err1) + 25 (DRV:281-286).
- first_point_trim = 25, second_point_trim = 85, efuse_value = 55, default_temp_offset = 50 (exynos8895-tmu-sensor-conf.dtsi:18-21).
- Fallback: if err1 == 0, use efuse & 0x1ff = 55. If err2 == 0, use (efuse >> 9) & 0x1ff (DRV:517-526).
- One-point: T = code - err1 + 25 (DRV:288).
- Clamp to EXYNOS_MIN_TEMP/EXYNOS_MAX_TEMP (DRV:296-299). The values were not looked up [G: roughly 15..125].
- This is identical to the CPU path. The only change is the base address, 0x10084000 instead of 0x10080000.

## GPU DT trips (zone GPU_THERMAL, sensor tmuctrl_2) DTS:4735-4781 [E]
- gpu-cold 20C (hyst 5C, active)
- gpu-alert0 78C (hyst 2C, active) -> mali cooling
- gpu-alert1 88C (hyst 5C, passive) -> mali cooling
- gpu-hot 115C (hyst 5C, hot)
- GPU cooling table: 546, 455, 385, 338, 260 MHz (DTS:4573-4578).
- Power allocator: sustainable-power 2000 (DTS:4741).

## Guesses
- [G] The 0x10084000 block may sit in a different power or clock domain than the CPU TMU. It could need an enable or clock before CURRENT_TEMP is valid. The driver writes CONTROL (0x20) for enable at DRV:603-620 [E], but whether the bootloader leaves block 2 enabled is unknown.
- [G] GPU sensor 0 of block 2 shares TRIMINFO(0) with the block's cal_type.
