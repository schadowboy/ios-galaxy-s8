# iOS 16.7.16 (iPhone10,3 kernelcache) kext inventory vs. the Galaxy S8: what each module needs from our kernel

Source: `ipsw kernel kexts` on the user's own IPSW kernelcache, 239 kexts (full list: scratch/agx/kexts-all.txt, not in git).
User directive (2026-11-15): do for EVERY module what is being done for the GPU: (1) read Apple's kext = what it promises userland,
(2) read the userland client (dyld cache, decompiled with Ghidra) = what it asks, (3) implement a kernel-side stand-in (IOKit classes with
the same names/selectors/properties) on the S8's real hardware, (4) test boot by boot, log-first. No edits of iOS userland.

Method kit (all in tools/): kc.py / agx-dispatch.py / agx-legacy.py / agx-vtable.py (kext tables), kdis.py (kernelcache disassembly + xrefs),
ldis.py / objc-imp*.py / sel-callers.py (userland), ghidra-run.ps1 + ghidra-scripts/DecompileFns.py (decompiler), S8Agx.cpp pattern
(IOService + IOUserClient that logs selectors/properties and answers), agx-logsum.py (log summary).

Legend: DONE = working in our kernel, PART = partly, TODO = nothing yet, N/A = no S8 counterpart (stub only), OUT = outside the clearance
(Audio/Sensors/GPS/cameras/NFC/fingerprint/iris).

| # | Module | Apple kexts (iPhone X) | S8 hardware | Strategy | Status |
|---|---|---|---|---|---|
| 1 | CPU, SoC platform | AppleARMPlatform, AppleInterruptController, AppleT8015, AppleT8010SOCTuner, AppleARMPMU | Exynos 8895 A53 cluster, GIC | our own platform expert + device tree from the loader | DONE (A53 only; big cores blocked) |
| 2 | Power manager, DVFS, perf control | ApplePMGR, AppleT8015PMGR, AppleT8015CLPCv3, AppleT8015PPM, ApplePMP(+Firmware) | ACPM (APM firmware) | stand-in for CLPC/PMGR calls userland/kernel make; real work through ACPM | PART (DVFS via ACPM; idle states TODO) |
| 3 | Thermal | AppleTemperatureSensor, AppleEmbeddedTempSensor | TMU blocks | map TMU to the sensor interfaces thermalmonitord reads | PART (kernel-side caps; iOS-facing sensors TODO) |
| 4 | Watchdog, reset, RTC | AppleARMWatchdogTimer, AppleSMC, AppleSPMI | PMU/WDT/RTC | stand-ins | PART (reset-to-TWRP only; real shutdown/RTC TODO) |
| 5 | Storage | AppleStorageDrivers (ANS), IONVMeFamily, AppleNANDConfigAccess, IOStorageFamily, apfs/hfs | UFS | our UFS block driver presents the same block device | DONE (HFS+ root) |
| 6 | Security / crypto | AppleS8000AES, AppleSEP*, AppleSSE, AppleSART, AppleImage4, CoreTrust, AppleMobileFileIntegrity, AppleMobileApNonce, AppleEffaceable*, FairPlayIOKit | none (no SEP) | stand-ins that answer like the real ones (AKS, AMFI done) | PART (AKS, AMFI done; SEP-dependent services stubbed) |
| 7 | IOMMU | AppleT8015DART, IODARTFamily | Exynos SysMMU | stand-in so DMA drivers get mappings | TODO (needed once DMA devices appear) |
| 8 | Display | AppleMobileDispH10P, IOMobileGraphicsFamily (IOMFB), AppleSynopsysMIPIDSI, AppleSummitLCD, AppleAcornDisplayPMU, AppleM2ScalerCSCDriver | DECON/DSIM + panel | IOMobileFramebuffer stand-in (SweetFramebuffer) | PART (CPU copy, no vsync/scaler) |
| 9 | Surfaces | IOSurface | n/a (software) | stand-in (SWEET SURF) | DONE (basic) |
| 10 | GPU | IOGPUFamily 65.60.1, AGXG10P, AGXFirmwareKext*RTBuddy, RTBuddy | Mali-G71 MP20 | AGX stand-in (S8Agx.cpp) translating to the Mali driver (S8Mali) | PART (stand-in answers; Metal init not yet through; shaders TODO) |
| 11 | Video codecs, JPEG | AppleAVE2, AppleAVD, AppleJPEGDriver | MFC, JPEG block | stand-ins, lower priority | TODO |
| 12 | Touch, buttons, haptics | AppleMultitouchSPI, AOPTouchKext, AppleM68Buttons, AppleHIDTransportSPI, AppleActuatorDriver, AppleHapticsSupportCallan, IOHIDFamily | S6SY761 touch, keys, haptic motor | HID stand-ins (SweetHID) | PART (polled touch, buttons DONE, haptics TODO) |
| 13 | Wi-Fi | AppleBCMWLANCore, AppleBCMWLANBusInterfacePCIe, IO80211Family, BCMWLANFirmware4357.Hashstore | BCM4361 (PCIe) | IO80211Controller stand-in over a native BCM4361 driver (firmware boots, loader W1 DONE) | PART (W1) |
| 14 | Bluetooth | AppleBluetooth(+Debug) | BCM4361 BT (UART/HCI) | HCI transport stand-in | TODO |
| 15 | Cellular | AppleBasebandPCI(+MAV/ICE), AppleBasebandD101/D20 | Samsung Shannon modem | Apple expects a PCIe Intel/Qualcomm baseband; S8 modem is a different design: research first | TODO (analysis only) |
| 16 | PCIe | IOPCIFamily, AppleEmbeddedPCIE, AppleT8015PCIe | DWC PCIe (Wi-Fi) | PCIe controller driver (loader code proves link) | PART (loader only) |
| 17 | USB | AppleEmbeddedUSB(Host), AppleS5L8960XUSB, AppleSynopsysOTGDevice, IOUSBHost/Device families, AppleUSBDeviceMux | DWC3 | controller driver + lockdown/mux interfaces | TODO |
| 18 | Battery, charger | AppleSmartBatteryManagerEmbedded, AppleC26Charger, AppleIDBusCharger, AppleTriStar | S2MU/MAX77 charger + fuel gauge | stand-ins reading the S8 charger | PART (SweetBattery) |
| 19 | Buses, GPIO | AppleS5L8940XI2C, AppleSamsungSPI, AppleSamsungSerial, AppleS5L8920XPWM, AppleGPIOICController | Exynos USI/I2C/SPI/UART/PWM | native drivers (Samsung IP, close to the S8's) | PART |
| 20 | Audio, sensors, GPS, camera, NFC, biometrics | AppleEmbeddedAudio, AppleMCA2, IOAudio2Family, AppleCS42L75Audio, AppleCS35L26Amp, AppleAOP*, AppleALSColorSensor, AppleProxDriver, AppleEmbeddedGPS, AppleH10CameraInterface, AppleStockholmControl, ApplePearlSEPDriver, IOBiometricFamily | various | OUT of the current clearance | OUT |

## Working order (proposal)
1. GPU (in progress: Metal device init, then memory, queues, shaders).
2. Display: vsync, scaler, fast flip (pairs with GPU: the compositor must get GPU frames onto the panel).
3. Wi-Fi (W2 msgbuf, W3 net interface, W4 IO80211 stand-in), then Bluetooth, USB, battery/charger, PCIe/IOMMU as needed, cellular last.
4. Per module: a doc like analysis/agx-kernel-interface.md (selectors, properties, userland behaviour) and a patch like 0150.
