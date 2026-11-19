---
name: project-s8-hardware-plan
description: "S8 iOS port: full hardware list (23 items), what the user cleared me to integrate, and the ORDER to work it (GPU, then big cores #2, then the rest); update statuses as items finish"
metadata:
  node_type: memory
  type: project
  originSessionId: 7d1691f7-4dbc-4151-81ff-8d44d595e26f
  modified: 2026-11-12T07:21:15.489Z
---

User goal (set 2026-11-12 via /goal): **full clearance to integrate: Compute and System, Display and Input, and Power and connectivity (in that category begin with Wi-Fi, Bluetooth, cellular).** Do not stop until each cleared item is done or genuinely blocked; if blocked, say exactly what is missing. Audio/sensors (#21-23) and cameras/NFC/fingerprint/iris were listed but NOT cleared by that goal (ask before starting them).

**ORDER (user decision 2026-11-12): work item by item, one at a time.** Current: #3 GPU. **#2 (Mongoose big cores) is next after the GPU is done.** Then the rest of Compute and System (#4-#8), then Display and Input (#9-#13), then Power and connectivity: Wi-Fi (#18), Bluetooth (#19), cellular modem first, then #14-#17, #20.

## Compute and system
| # | Hardware | State (2026-11-12) | Effort | Notes |
|---|---|---|---|---|
| 1 | 4x Cortex-A53 | Works at 1.69 GHz (loader sends ACPM DVFS on channel 0) | Done | No idle states, DVFS or thermal control yet (#4, #5) |
| 2 | 4x Mongoose M2 big cores | Parked. APM accepts CPUCL0 requests and retunes PLL+rail, PLL STABLE never sets, core marked powered in PMU but never executes (experiments mexp1-3: rail forced on, clock switch, cache/visibility all ruled out) | High, uncertain | Blocker likely in secure firmware (PSCI/EL3 reset-vector or cluster sequencer). analysis/mngs-bringup.md, loader mngsexp=1|2|3, mngsfreq= |
| 3 | GPU Mali-G71 MP20 (0x13900000) | Kernel self-test PASS (WRITE_VALUE jobs), IOKit driver S8Mali built; first user-space test PANICKED (phystokv illegal PA 0x952b00000 outside RAM) -> panic hook 0134 + PA validation added, retest pending; Mesa 25.0.7 iOS dylib built; CoreAnimation hook analysis done (ca-hook-port.md) | High | Fixes UI speed. Next: injection into backboardd via kernel (0135 s8_inject, built), hook_log experiment, retest Mali driver, wire Mesa+shim |
| 4 | CPU/GPU DVFS + idle | Fixed clocks. Patch 0136 (ACPM client + governor + TMU) was pushed by an agent before I stopped it, CI unverified, untested | Medium | Heat and battery |
| 5 | Thermal sensors (TMU) + throttling | In 0136 (untested) | Medium | Safety at 1.69 GHz |
| 6 | RAM | 2 of 4 GiB used (ADT dram-size 0x80000000) | Low | Unlock the rest once stable |
| 7 | Real shutdown / reboot | Panic/reboot reset into TWRP (s8recov, PMU INFORM2/3 + SWRESET) | Low-medium | Needs proper PMIC power-off and a normal reboot path |
| 8 | RTC | Seeded from boot-arg s8rtc | Low | Real RTC read/write |

## Display and input
| # | Hardware | State | Effort | Notes |
|---|---|---|---|---|
| 9 | Panel + display controller (DECON/DSIM) | CPU copies frames to panel | Medium | vsync, scaler, panel power, fast flip; pairs with #3 |
| 10 | Brightness/backlight | Fixed | Low-medium | |
| 11 | Touch (S6SY761) | Works, polled | Low | Interrupts, multi-touch, palm rejection |
| 12 | Buttons | Work | Done | |
| 13 | Haptic motor | None | Low | |

## Power and connectivity
| # | Hardware | State | Effort | Notes |
|---|---|---|---|---|
| 14 | PMIC rails | Partly (APM drives DVFS rails; pmic.c reads/writes S2MPS17 over SPEEDY) | Medium | |
| 15 | Battery gauge + charger | Not integrated | Medium | |
| 16 | USB device mode | Not in iOS | Medium-high | Biggest productivity gain (logs/files without TWRP trips) |
| 17 | USB host/OTG | None | Medium | |
| 18 | Wi-Fi (Broadcom BCM4361, PCIe0) | None. Plan: analysis/connectivity-plan.md. iPhone X uses BCM4357, so Apple firmware is NOT a drop-in; our kernel cannot load Apple kexts (no prelinked KC) -> built-in driver. Samsung firmware extracted to C:\github\ios-s8\firmware\samsung\wifi | Very high | Stage 0 = loader PCIe link probe reading the chip ID |
| 19 | Bluetooth | None. Samsung HCD files extracted (firmware\samsung\bt). UART1 0x10830000 | High | Stage 0 = HCI_Reset probe |
| 20 | SD card | None | Medium | |
| - | Cellular modem (Shannon 355) | None. User listed it as FIRST-wave in this category. sda10 backup is a valid modem image | Very high | Data-only realistic first; see connectivity-plan.md |

## Not cleared / likely out of scope
Audio (#21 codec+ABOX DSP; DSP firmware already pulled to firmware\samsung\audio), sensors (#22), GPS (#23); cameras (ISP), NFC, fingerprint, iris (secure-element/TEE).

**Why:** user wants real hardware integration, not tuning; wants progress reports and to be talked to.
**How to apply:** keep one item active, mark state here when an item changes, and consult [[feedback-one-task-at-a-time]] for the working rules. Workspace details: [[project-s8-ios-port]].
