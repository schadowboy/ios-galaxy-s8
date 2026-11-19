# AGX kernel interface (iPhone10,3 / A11 "G10P"), what iOS userland expects from the GPU kext

Source: the user's own IPSW `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw`, kernelcache extracted with `ipsw extract --kernel`
(old-style prelinked kernelcache, tools `kc.py`, `agx-*.py/.sh`; extracted files live in `scratch/agx`, never in git).
Goal (user directive 2026-11-15): implement this interface in OUR XNU as IOKit classes that serve the same selectors on the Mali-G71,
without modifying iOS (no binary edits, no injected libraries).

## 1. Kexts [E: ipsw kernel kexts]
| kext | header vm | code | role |
|---|---|---|---|
| com.apple.iokit.IOGPUFamily 65.60.1 | 0xfffffff005544440 | 0x24f98 | generic GPU layer |
| com.apple.AGXG10P | 0xfffffff00554b980 | 0x61938 | A11 GPU driver (AGXAccelerator...) |
| com.apple.AGXFirmwareKextRTBuddy 227.6.5 / ...G10PRTBuddy | 0xfffffff00503a940 / 0xfffffff00503b280 | tiny | firmware image + RTKit buddy: the GPU has a coprocessor |
| com.apple.iokit.IOMobileGraphicsFamily 343.0.0, com.apple.driver.AppleMobileDispH10P 140.0 | | | display (IOMobileFramebuffer) |

## 2. Classes (from __cstring) [E]
IOGPUFamily: IOGPUDevice, IOGPUDeviceUserClient, IOGPUMemoryInfoUserClient, IOGPUCommandQueue, IOGPUIOCommandQueue, IOGPUIOCommandBuffer,
IOGPUResource, IOGPUResourceSet, IOGPUMemory, IOGPUMemoryMap, IOGPUTask, IOGPUNamespace, IOGPUChannel, IOGPUWorkQueue, IOGPUScheduler,
IOGPUNotificationQueue, fences/events (IOGPUBlockFence, IOGPUEventFence, IOGPUSharedEventFence, IOGPUMTLEvent, IOGPUMTLFence,
IOGPUMTLLateEvalEvent), IOGPUStatistics, IOAcceleratorES.
AGXG10P: AGXAccelerator -> AGXAcceleratorG10 -> AGXAcceleratorG10P_A0/_B0 (IOKit personalities AGXG10P_A0/_B0), AGXA11Device,
AGXDeviceUserClient, AGXCommandQueue, AGXCommandBuffer, channels (AGXTAChannel, AGX3DChannel, AGXCLChannel + G10 / Gen2 variants),
AGXGart/AGXGartG10 (GPU MMU), AGXParameterManagement, AGXSpillBufferManager, AGXRenderTargetMemory, AGXFirmware/AGXArmFirmwareKingfisherV4,
AGXPerfCtrSampler, AGXEventMachine, AGXLateEval*, AGXPeakPowerManager, AGXStatistics.

## 3. External method table of the IOGPUFamily user client [E: tools/agx-dispatch.py]
`IOExternalMethodDispatch` array at 0xfffffff006fc6a90, 100 entries (selectors 0-49, then 50-99 repeat the same functions: second
copy for the other user-client type). Selected entries (selector: scalarIn / structIn / scalarOut / structOut):
- 0: out 64 B; 1: out 64 B; 2: out 536 B (device info?); 3: out 8; 4: out 32; 5: out 16
- 6: in 16, out variable; 7: in 1032, out 16; 9: in/out variable
- 8,10,14,16,19,21: 1 scalar in; 11,25,31,33,34,36,42,45,46: 2 scalars in; 12: 2 in 1 out; 13,15: 2 in, 16 B out
- 17: out 8; 18: out 4; 20: 2 scalars out; 22: out 48; 23: out variable; 24: 1 scalar out
- 26: 4 scalars + variable struct in (submit-like); 27: 1 scalar + 12 B in, 1 out; 28: 1 + 4 B in; 37: 3 in 1 out; 43: 1 scalar + var struct
Second table at 0xfffffff006fcb518 (3 entries) = IOGPUMemoryInfoUserClient or similar. AGXG10P's own table (0xfffffff006fcfec0) has 2
entries: sel 0 (none in/out), sel 1 (2 scalars in); the rest of AGXDeviceUserClient's selectors are handled in code (switch in
externalMethod): next step is to disassemble `AGXDeviceUserClient::externalMethod` (capstone is installed in WSL).

## 4. What the stand-in has to be (plan)
1. IOKit classes in our XNU with the Apple names and matching keys (IOGPUDevice/AGXAccelerator service, IOGPUDeviceUserClient,
   AGXDeviceUserClient, command-queue and memory-info user clients), `newUserClient(type)` with the types userland uses.
2. Step A (logging only): `externalMethod` for every selector logs selector, input sizes and a hex dump of struct input, returns
   success with zeroed output of the declared size. Boot, read what Metal / CoreAnimation request first, iterate until the userland
   AGX driver (AGXMetalA11) gets through device creation.
3. Step B: memory (IOGPUMemory/resources/GART) = real allocations, mapped to the Mali's address space (our S8Mali driver already
   does BO alloc/map).
4. Step C: command-buffer submit (sel 26-ish and AGX-specific): decode the AGX command stream.
5. Step D: shader translation. Apple's userland compiles shaders to the AGX ISA; the Mali cannot run that. The kernel/userland
   bridge has to translate AGX ISA -> NIR/Bifrost (Panfrost compiler). AGX (G13) ISA is reverse engineered by the Asahi project;
   G10 differs and is less known: needs decoding work. This is the long pole.
6. Firmware: AGXFirmwareKext*RTBuddy: our stand-in replaces the firmware coprocessor, so no firmware is loaded.

## 5. AGXDeviceUserClient's own selectors [E: tools/kdis.py, agx-legacy.py, agx-vtable.py]
`AGXDeviceUserClient` (vtable at 0xfffffff006fcf600 in AGXG10P) overrides getTargetAndMethodForIndex (0xfffffff006b6fb80). Selectors below
0x100 go to IOGPUFamily's table (section 3); 0x100-0x10a are AGX-specific:
| selector | kind | in / out | handler | what it does |
|---|---|---|---|---|
| 0x100 | struct out | 0 / 0x78 | vtable+0x5e8 = 0x6b6fcd0 | copies 0x78 B of the accelerator's config block (accelerator->[+0xf8]->+0x368) |
| 0x101 | struct out | 0 / 0x80 | +0x5f0 = 0x6b6fd08 | same block rearranged, adds 3 flags and a size field |
| 0x102 | struct out | 0 / 0x1c8 | +0x5f8 = 0x6b6fda4 | name strings (0x60/0x20-byte fields, copied from __os_log/const text) + a double + a platform string |
| 0x103, 0x104, 0x106 | struct out | 0 / 0x20, 0x10, 8 | 0x6b6fe9c, 0x6b6fea4, 0x6b6feac | return 0 and write nothing (zero struct is fine) |
| 0x105 | struct in/out | 0x20 / 0x20 | 0x6b6fec8 | not decoded yet |
| 0x107, 0x108 | async dispatch | 2 scalars in | 0x6b70960 / 0x6b70880 (tables 0xfffffff006fcfec0 / ...ed8) | not decoded yet |
| 0x109 | struct in/out | 8 / 8 | 0x6b705b0 | not decoded yet |
| 0x10a | struct out | 0 / 0x10 | +0x618 = 0x6b6feb4 | not decoded yet |
So the "device info" Metal reads at creation is one config block inside the accelerator (offset 0x368 of the object at accelerator+0xf8),
filled by AGXG10P at start. For the stand-in that block must be filled with A11-like values (core counts, memory sizes, clocks):
next: find the writer of +0x368 (probably the AGXAccelerator start / GPU-config init), and read the A11 constants from the kext.

## 6. Userland side [E: ipsw dyld macho --strings/--extract on the root image's dyld shared cache; files in scratch/agx/dylibs, not in git]
- The GPU userland (AGXMetalA11, IOGPU) is inside the dyld shared cache (the .bundle dirs on disk are empty shells).
- IOGPU.framework creates the device with IOServiceOpen on an IOAccel service ("Failed to create an IOAccelDevice... IOServiceOpen
  returned ..."), keeps the connection in `__IOGPUDevice` and maps kernel shared memory: **IOGPUDeviceCommPage**,
  **IOGPUClientSharedGlobalRO**, **IOGPUClientSharedRO**, **IOGPUDeviceShmem** (layouts needed in the stand-in).
- Command-buffer failures userland knows (error codes the stand-in can return): Timeout, PageFault, OutOfMemory, InvalidResource, Hang,
  DeviceRemoved, ExceededHardwareLimit, NotPermitted, ...
Next: (1) from IOGPU (extracted dylib) find the IOServiceMatching/IOServiceOpen type constants and the first selectors it calls at device
creation, (2) decode the CommPage / SharedGlobalRO layouts from the kext and dylib users, (3) write patch 0150 with the stand-in classes
(AGXAccelerator / IOGPUDevice service, AGXDeviceUserClient: externalMethod logging every selector + zero-filled outputs of the declared
sizes, memory-type mappings for the shared pages), then boot and read the log.

## 7. FIRST BOOT OF THE STAND-IN (2026-11-15, patch 0150 stage A, boot-args s8mali_drv=1 s8agx=1, kernel out-v53) [E: measured]
- iOS finds and opens it: **backboardd[44]** (the compositor) opens a type-1 user client of AGXAcceleratorG10P_B0 right after boot.
- Calls in order: sel 0x2 (out 536 B), sel 0x0 (out 64 B), sel 0x4 (out 32 B), then sel 0x21 (IOGPU table index 33: 2 scalars in, calls a
  virtual method vtable+0x5e0) many times (59 seen in the first run). Then nothing: no memory mappings, no notification port, no more
  selectors. Reading: all answers were zeros, userland rejects the device after the info queries (or waits).
- What the info selectors read in Apple's kext (IOGPUFamily 0x6b2d344.., fields of the accelerator object at this+0xf8):
  - sel 0 (64 B): [0] u32 = (cfg+0x1e0 & 0xffff) | ((cfg+0x1e0 >> 9) & 0x1000); [4] u32 = cfg+0x1e8; [8] u64 = vcall +0x540; [0x10] u64 = cfg+0x1f8;
    [0x18] u32 = cfg+0xf8; [0x20] u64 = fn 0x6b47774(); [0x28] u64 = vcall +0x670(...); [0x30] u64 = cfg+0x248; [0x38]/[0x3c] u32 = u16 cfg+0x250 / cfg+0x252
    (look like core/cluster counts).
  - sel 1 (64 B): memcpy of 0x40 bytes at cfg+0x1d8 (a pointer).
  - sel 2 (536 B): [0] u64 = vcall(this+0x110 obj, +0x78) + this+0x118; [8] 8 B at cfg+0x21c; [0x10] u32 cfg+0x224; [0x14] n = count of
    entries of the object at cfg+0x138; then n u32 values at +0x18 (one per entry: probably performance states / clocks).
Next: find how IOGPU/AGXMetalA11 (scratch/agx/dylibs) uses these structures when creating the device (what it validates), pick A11-like
values (core counts, sizes), return them from the stand-in, and iterate boot by boot until backboardd's Metal device comes up.

## 8. Second/third boots (2026-11-15, kernels out-v53/out-v54) [E: measured]
- backboardd[44] is NOT stuck at sel 0x21: the 0x21 calls interleave with its normal display work (SWEET FB selectors, creation of a
  1440x2960 BGRA surface). It keeps running on the CPU-copy display path.
- IOGPU's device creation (IOGPUDeviceCreateWithAPIProperty, 0x1ece1ce38 in the extracted IOGPU) calls sel 2 (0x218 B buffer), then sel 0 (0x40 B, kept
  at dev+0xa0), then sel 4 (0x20 B, dev+0x58/0x60/0x68); a nonzero return from any destroys the device; zeros are accepted. sel 2: out[0] = base,
  u32 at +0x14 = region count n, region offsets u32[n] from +0x18 (added to base). So the stand-in's zeros pass this function.
- Registry reads on the accelerator (logged): IOBusyInterest, BSD Name, IOClassNameOverride, IOUserServiceProperties,
  BaseAddressAlignmentRequirement (IOSurface asks for it on every surface creation; MISSING). Nothing Metal-specific (MetalPluginName,
  gpu-core-count...) was read, and AGX's own selectors 0x100-0x10a and clientMemoryForType of the stand-in were never called: the Metal
  device (AGXA11FamilyDevice) is not reaching its AGX-specific init in the 180 s window.
- Open question: what decides that. Needs the decompiled bodies of IOGPUMetalDevice init / AGXA11FamilyDevice initWithAcceleratorPort
  (AGXMetalA11 + IOGPU extracted in scratch/agx/dylibs). Plan: set up Ghidra (headless) in WSL for the dylibs and the kexts.

## 9. Metal reaches the stand-in (2026-11-15, kernel out-v56, image v74) [E: measured + decompiled]
- ROOT CAUSE of "Metal never opens the GPU": Metal.framework enumerates GPUs with IOServiceMatching("IOAcceleratorES") (string in Metal);
  the stand-in only derived from IOAccelerator. With the chain AGXAcceleratorG10P_B0 -> AGXAccelerator -> IOAccelerator -> IOAcceleratorES
  (patch 0150) Metal reads MetalPluginName / MetalPluginClassName (set to AGXMetalA11 / AGXA11Device), loads the plug-in, and BOTH
  backboardd and a test process (s8mtl_test) open type-1 clients.
- Call sequence of the plug-in's device init (AGXA11FamilyDevice initWithAcceleratorPort:simultaneousInstances: @0x209e13d40 in AGXMetalA11,
  decompiled: scratch/agx/agx-a11-init.c): IOGPU part sel 6 (16 B in), 2, 0, 4; AGX part: sel 0x102 (out 0x1c8), sel 0x107
  (IOConnectCallAsyncScalarMethod, 3 scalars + wake port + 3 ref words; registers an async notification), sel 0x100 (out 0x78 stored at
  device+0x268 = AGXGPUCoreConfig).
- With zeroed answers the init takes its failure path; the cleanup then aborts (s8mtl_test SIGABRT in AGX::Device::~Device ->
  malloc free of a bad pointer), and backboardd crash-loops (pids 45 -> 82 -> 95) while s8agx=1. So s8agx stays OFF by default; only test
  boots use it.
- Needed next: real contents for sel 0x100 (AGXGPUCoreConfig, 120 B), 0x102 (456 B: name strings + a double + platform string) and the
  async 0x107 protocol; source = AGXG10P's config builder (the code that fills accelerator->[+0xf8]->+0x368 and the strings in the kext's
  __os_log/__const) and the plug-in's consumers (setupDataBufferParams, *USCStateLoader::dataBufferConfigs read the config).
- Tools added: Ghidra headless (D:\github\backup\tools\ghidra), tools/ghidra-run.ps1, objc-imp*.py (method addresses in the iOS 16 cache),
  sel-callers.py, bl-callers.py, agx-a2s.sh (symbolicate), userland/s8mali/s8mtl_test.m (Metal test) + mtl-install.sh/hash-phone.ps1/
  extract-changed.sh (SKIPPULL root install from a consistent image, flash every chunk that differs from the phone).

## 10. METAL RUNS ON THE STAND-IN (2026-11-15, kernel out-v61, image v79, userland s8mtl_test) [E: measured, log read from the phone's root]
Result of the Metal test (/var/mobile/s8mtl_test.log) with s8agx=1:
- MTLCreateSystemDefaultDevice() returns a device: name "Apple A11 GPU", maxBufferLength 1 GiB, recommendedMaxWorkingSetSize 1.5 GiB,
  supportsFamily Apple1..4 all true (the numbers come from the stand-in's answers: sel 0x0 shared size 0x60000000 / max resource 0x40000000, sel 0x100 core config 3 cores).
- newCommandQueue works; newBufferWithLength works (contents pointer = memory the stand-in mapped into the process);
  newLibraryWithSource: compiles the test shader (Apple's AGX compiler runs on the CPU): "library 0x... error none";
  newComputePipelineStateWithFunction: "pipeline 0x... error none".
- FAILS at [commandQueue commandBuffer]: IOGPUMetalCommandBufferStorageAllocResourceAtIndex aborts because the storage has no resource pool
  at the requested index. Pools are created by the AGX plug-in in -[AGXA11FamilyDevice setupDeferred] (a dispatch_once block run by newCommandQueue;
  it builds internal pipelines and calls -[IOGPUMetalDevice setHwResourcePool:count:] with 32 pool objects); the block's pool switch is behind a
  jump table Ghidra cannot resolve. Hypothesis: a pool is skipped (NULL) because a config field (AGXGPUCoreConfig, sel 0x100, only num_cores/num_gps/num_frags/
  num_mgpus/core_mask known) or a feature flag from sel 0x0/0x2/0x4 has the wrong value.
- AGX also complains on stderr: "agxs_driver_connection.cpp:75:checkDeviceConnectionSanity: *** The KMD features string is empty" -> the 0x60-byte string at
  offset 0x28 of the sel 0x102 answer (kext "features") must be non-empty; find what the real kext puts there.
- Per-client logging, resource/queue/shmem protocols, decompiled sources: see sections 7-9, scratch/agx/*.c, tools/*.

## 11. FULL METAL CONTROL-PLANE ROUND TRIP (2026-11-15, kernel out-v64, image v82) [E: measured]
s8mtl_test log: device "Apple A11 GPU" -> command queue -> buffer -> library (AGX compiler on CPU) -> compute pipeline -> encode -> commit ->
"after waiting, status 4 (completed) ... error none; a[0..3] = 0 0 0 0" -> done. The compute result is zero because the stand-in reports completion
without running anything.
Protocol pieces implemented in patch 0150 (all decoded from the user's IOGPU/AGXMetalA11 with Ghidra, see sections 7-10):
- sel 5 global trace id range {first id != 0, end}; zero aborts userland (IOGPUDeviceGetNextGlobalTraceID).
- sel 9 resource create: caller memory (flag 0x80) is echoed, sub-buffer GPU VA = primary VA + offset, 80-byte answer.
- sel 0xf notification queue = IOSharedDataQueue mapped into the client; registerNotificationPort(port, 0, queue id) must bind the port (a missing
  binding was the cause of the earlier hang: completions were enqueued but userland was never woken); sel 0x19 attaches queue to command queue.
- sel 0x1a submit: scalars {queue id, flag, count, entry size 0x38}, entries with cookies at +0x10 ("started") and +0x18 ("completed"); the kernel posts
  two 0x20-byte records {cookie, t1, t2, status} per command buffer; userland calls (*(cookie+0x10))(cookie, t1, t2, status).
- With s8agx=1 backboardd also submits Metal frames continuously (rendering is now going through Metal and completing instantly with nothing drawn).
NEXT (data plane): the stand-in must read what userland wrote (kernel command lists in the type-0/1 shmem regions it allocated, resources it mapped)
and execute it on the Mali: decode the AGX G10 command stream and translate shaders (AGX ISA -> Mali). First experiment: hexdump the command
regions at submit to learn the format.

## 12. FIRST CAPTURE OF AGX DATA (2026-11-15, kernel out-v66, image v84; tools agx-capture.py, agx-show.py, agx-show-all.sh; data in scratch/agx/capture1, not in git)
Sparse dump (non-zero 64-byte lines) of the test process's first submit: 13 regions.
- shmem type 1 (kernel command list, 16 KiB, only 34 non-zero bytes): header { 0x00010000, 0x1f0, 4, ...}, then {0x1e8, 0x1b8, 0x30, 3, 0xa0000, 1, ...,
  0x80000, 1, 0xa0024, 1} = a few entries with sizes/offsets and GPU pointers (0xa0000, 0xa0024: low parts of resource addresses).
- shmem type 0 (segment list): header {2, 0, 1, 0x800000b0, 3, ..., 0x1f0, ..., 0xa, 2, 9..0xe, 0x20 x6, ...}: resource ids 9..0xe + sizes = the list of resources
  the command buffer uses (9-14 are the command-buffer resources).
- res 1 (GPU VA 0x100000000, 0x20000): device constant table; after a counting table (0,0,2,0,3,0,...) a table of {multiplier, shift} pairs
  (0xaaaaaaaa/1, 0xcccccccc/2, 0x92492492/2 ...) = magic numbers for integer division by constants (the AGX compiler's lookup).
- res 3 (GPU VA 0x100020000, 0x10000, 6537 non-zero bytes): AGX shader code (Metal's internal programs; instruction words like 4a01000e 02520011,
  c0200060, 0048041e; blocks start after a small header at +0x40).
- res 9-13 (command buffer memory): only 2-13 non-zero bytes at this submit: the compute dispatch encoding is not in them (the submit seen first is
  probably an internal one); the test's own compute command buffer needs a separate capture.
PLAN (data plane, differential reverse engineering): extend s8mtl_test to compile a family of small kernels with known semantics (a[i]=i*2, i+1,
float add/mul, loops, buffer args...) and dispatch them, so ONE boot captures the program heap with many kernels; compare the binaries to decode the
ISA, and capture the compute command (kernel command list + command buffer memory + argument tables) for the dispatch. The stand-in then executes
a dispatch on the Mali (translate AGX code -> Mali) or, as a first step, interprets the AGX code on the CPU.

## 13. DIFFERENTIAL CAPTURE OF TINY KERNELS (2026-11-15, kernel out-v67 delta dumps, s8mtl_test batch of 8; tools agx-capture2.py, agx-sessions.py; data scratch/agx/capture2)
One boot = 7 submit sessions (kernels k_add1 .. k_copy_add1 in sessions 2..7; session 1 = first kernel plus the full initial memory).
- PROGRAM CODE: every kernel's whole AGX machine code is one 64-byte line at res_3 (program heap, GPU VA 0x100020000) + 0xc80, overwritten by the next kernel.
  All start 041006e2 000000c2 ...; the two-buffer kernel starts 041004e2 000400c2 000602c2 ... (instruction stream of variable-length records, little-endian).
  Examples (dwords): k_add1 (a[i]=i+1)   041006e2 000000c2 8601100e 02c20011 000e0002 021ac6c0 01001145 0012c000 00000108
                      k_shl2 (a[i]=i<<2)  041006e2 000000c2 8600100e 02c20211 000e0002 021ac6c0 01001145 0012c000 c0000108 01080012
  Differences add1 vs shl2: dword 2 byte1 (0x01 vs 0x00) and dword 3 byte2 (0x00 vs 0x02) -> immediates / shift amount are single bytes in the stream.
  Common tail (..021ac6c0 01001145 0012c000 00000108) = store + end sequence.
- COMPUTE DISPATCH COMMAND (res_11 +0): 50400003 01000800 0001001f 00020c80 00000108 ...: word3 = program address (0x20c80 = heap + 0xc80), 0x1f = threadgroup
  size - 1 (32 threads), word0 changes 50400003 -> 50800003 for the two-buffer kernel (buffer/uniform count?).
- ARGUMENT TABLE (res_12 +0x40): u64 GPU addresses of the bound buffers (0x100002100, 0x100003100).
- KERNEL COMMAND LIST (shmem type 1): header {0x00010000, 0x1f0, 4, ...}, then {0x1e8, 0x1b8, 0x30, 3, 0xa0000, 1, ...}; a counter at +0xc8 increments by 2 per kernel
  (5,7,9,0xb,0xd,0xf): command/sequence id.
NEXT (decode the ISA and the command): a bigger kernel family varying one thing at a time (a[i]=i+K for K=0..N, i*K, thread counts, threadgroup sizes, buffer counts,
float/int ops, loads) in ONE boot (raise the dump session cap, watch the log ring), then write an AGX decoder + a Mali translator/emulator.

## 14. AGX (G10P) COMPUTE ISA, FIRST DECODE (2026-11-15, 31-kernel differential capture, scratch/agx/capture3/prog-diff.txt; tools agx-prog-diff.py, agx-kernels.txt)
Program = byte stream at program heap + 0xc80 (one kernel at a time). Kernel family: a[i] = i+K, i*K, shifts, logic, sub, float, loads (see tools/agx-kernels.txt).
Structure of  a[i] = i + K  (K = 1..7):
  e2 06 10 04                     thread id fetch (thread_position_in_grid)   [k0: e2 04 10 04]
  c2 00 00 00                     mov/setup
  0e 10 KK 86 11 00               iadd: result = tid + K   (KK = the constant when K < 64)
  c2 02 02 00                     mov/setup (output buffer address)
  0e 00 c0 c6 1a 02               store result to a[tid]   (k0 stores tid directly: ... c0 44 1a 02)
  45 11 00 01 00 c0 12 00 08 01   tail / end of program
- Immediates: K < 64 -> one byte (0x24 for 100 is the LOW 6 BITS of 100); the rest goes into an extension: 100 -> "11 80 00 04" (high part 1 << 10),
  255 -> low6 0x3f + "80 00 0c" (3 << 10). 256 uses a different form ("86 00 05 81 00 20"); 65535 is loaded by a separate mov with a 16-bit immediate
  ("92 00 ff ff") and then added.
- Opcode byte of the ALU record: 0e = add with shifted operand: r = a + (b << SH): i<<1 -> "0e 10 00 86 11 01", i<<3 -> "... 11 03", i*4 = i<<2 (strength reduced),
  i*3 = i + (i<<1) "0e 10 86 86 15 01", i*5 = i + (i<<2) "... 15 02". 4e = subtract (i-1: "4e 10 86 01 05 00"; 100-i: "4e 10 24 86 11 80 00 04"),
  6e = logical shift right ("6e 10 00 86 05 80 SH 00"), 7e = logic family: AND "7e 10 86 0f 83 00" (i&15), OR "... 10 e3 00" (i|16), XOR "... 3f 63 30" (i^255);
  float ops use be/8a/4a records (f*2.0 "be 12 0a 86 61 00", f+1.0 "8a 11 c4 30 04 00").
- Loads (kernels 29/30, b[i]): longer programs with an extra block "05 11 04 05 00 c0 12 00 38 00" (load from buffer 1) before the compute and store.
Not decoded yet: register/operand fields beyond the immediates, loop/branch instructions, predication (select), multiple threadgroup dims, texture/render ops.
Execution plan: implement a small CPU interpreter in the kernel stand-in for this compute subset (thread id, iadd/sub/shift/logic/float, load/store through the
argument table) so dispatches produce correct results; later translate to Mali.

## 15. SECOND KERNEL FAMILY: MORE ISA + DISPATCH FIELDS (2026-11-15, capture4 = 24 kernels; tools agx-kernels2.txt, scratch/agx/capture4/prog-diff.txt)
Programs end at the tail "45 11 00 01 00 c0 12 00 08 01"; bytes after it in the 64-byte line are stale leftovers of earlier kernels.
- d2 00 <imm32>      load 32-bit constant (div by 3: d2 00 ab aa aa aa = 0xaaaaaaab, mod 5: cd cc cc cc), followed by 8e ?? 86 c0 0e 00 (multiply-high) and 6e ... 05 80 SH 00
                      (shift right): i/3 = mulhi(i,0xaaaaaaab) >> 1. i%5 = i - 5*(mulhi(i,0xcccccccd)>>2) using 0e 12 c2 c2 15 02 (q + q<<2) and 4e (sub).
- 52 04 <imm32>      mov immediate 32 (constant store: 52 04 07 00 00 00).
- 1e 11 SRC IMM COND T F   compare-and-select: select(b[i]>10, 5, 9) = 1e 11 c4 0a 13 05 09 00; select(i<8, 1, 2) = 1e 11 86 08 0b 01 02 00 (the compiler turned the if/else into
                      a select); min(i,20) = 1e 11 86 14 0b 86 14 00. COND 0x13 = greater, 0x0b = less (unsigned).
- 8e 10 86 86 0d 00 integer multiply register*register (i*i); 8e 10 86 86 1d 80 86 00 = mad (i*i + i).
- store: 0e 00 c0 VAL 1a SZ  with SZ 00 = 8 bit, 01 = 16 bit, 02 = 32 bit (uchar / ushort / uint stores); VAL = source register byte (c4 / c6 / c8 ...).
- thread ids: e2 00 10 04 / e2 04 11 04 = two fetches (x, y) in a 2D kernel; a2 06 30 00 + e2 02 00 00 = thread-in-threadgroup / threadgroup-position.
- constant buffer argument: c2 00 08 00 (load from the constant buffer slot), then add.
- float: be 12 0a 86 61 00 (convert), ca 11 c4 10 04 08 00 40 (mad with float constants; 3.0f = 0x40400000 appears as 00 40).
- DISPATCH PARAMETERS are plain u32 in the command-buffer memory: grid size at command header +0x30..: 0x40,1,1 (64x1x1) / 8,8,1 (8x8 grid); command words
  50400003 01000800 0001001f <program addr> <arg descr> ... (extra words 20200803 01000600 for the 2D kernel); the arguments are listed after the program address.
NEXT: build an offline AGX emulator (Python) validated against the 55 captured kernels (their outputs are known), then port it to C in the stand-in so a dispatch computes
correct results; Mali translation after that.

## 16. RENDER PASS CAPTURE (2026-11-15; capture5 = backboardd frames, 2875 submits in 3 min; capture6 = controlled render family; tools agx-find.py, agx-sessions.py)
- backboardd (CoreAnimation) renders through Metal when the stand-in is on: ~16 command buffers per second; per frame several 192 KB resources change (render targets / tile data).
- Controlled render family (s8mtl_test: 64x64 BGRA target, full-screen triangle, 9 scenes) -> capture6. Findings so far:
  * kernel command list (shmem type 1) of a render pass: header {0x00010000, 0x820, 4, ...}; target 0x40 x 0x40, tile 8x8 (0x8, 0x8), 0x20 x 0x20 fields, 3f800000 (1.0f).
  * render pass descriptor resource (res_17 in that run): header "fb05050a 00007e01 40018000", then viewport/transform floats (3f800000 identity) and the CLEAR COLOUR as
    4 floats at +0x38..+0x44 (0.25, 0.5, 0.75, 1.0 -> 3e800000 3f000000 3f400000 3f800000), followed by the next record "1fe3050a 000007e0 10006000 00000200".
  * programs for the render pipelines appear in the program heap (res_3) like the compute ones.
NEXT: compare the nine scenes (draw vs clear-only, colour constants, interpolation, blend) to find the draw/vertex/fragment/blend encodings and the pixel results the
test logged; then decode CoreAnimation's own frames (capture5) with the same tools.

### 16b. Render scenes: results (2026-11-15, capture6 vs the test log)
- All nine render scenes complete with status 4 and read back 0x00000000: the stand-in completes work, nothing renders yet (expected).
- Session map: sessions 1..9 = scenes 0..8 (clear only x2, red, green, blue, 0.5/0.25/0.125, interpolated, red over white x2).
- Clear colour: render pass descriptor res_17+0x38 (4 floats), unchanged when the clear stays the same.
- Fragment shader of a constant-colour pipeline = a tiny program written into the program heap (res_3) at the same slot (+0xc80); the whole difference between
  green / blue / 0.5-0.25-0.125 is ONE 64-byte line:
    green : 00000052 04520000 00000000 00000252 06523f80 3f800000 | 00c20048 ...
    blue  : 00000052 02520000 00000000 00000452 06523f80 3f800000
    0.5/.25/.125: 00000052 02523f00 3e800000 00000452 06523e00 3f800000
  => float immediates are stored as the UPPER 16 BITS of the float32 (0.5=3f00, 0.25=3e80, 0.125=3e00, 1.0=3f80, 0=0000); an instruction word ends in 0x52 (mov-immediate
     style, 16-bit immediate + register selector in bits 24..31 of the neighbouring half) and the program ends with 00000209 fff20000 00000108.
- The vertex program and the draw packet are in the first changed lines of session 3 (res_3+0xc800..0xe3c0 is the compiled library; res_13 holds binding tables).
NEXT: decode the draw/blend packets by diffing sessions 7 (interpolated), 8 and 9 (blend) against 3; then implement a CPU rasteriser for the compositor's few program shapes
(solid fill, textured quad, blend) as the first real data plane, and later a Mali-side implementation.

### 16c. Draw state decode (capture6)
- Viewport block (res_28/res_30 +0x380): 42000000 42000000 42000000 c2000000 = 32.0 32.0 32.0 -32.0 for a 64x64 target (centre/scale, Y flipped).
- Per-draw shader state records: res_18+0x40 = vertex shader reference {0x0001001f, low VA 0x20d10 (solid) / 0x20dd0 (interpolated varyings), 0x108} -> res_3+0xd10 / +0xdd0;
  res_19+0xc0 = fragment shader reference {0x0009051f, 0x20c81, 0x2f358, 0x108}; bit 15 of the first dword (0x00098...) = fragment takes interpolated inputs.
  VA = 0x1_0000_0000 | low; low bit 0 of the pointer word is a flag (0x20c81 -> program at +0xc80).
- Blend: res_27 record 2 dword 4 changes 002e0f00 -> 012e0f00 (bit 24 = blending enabled); the blend shows up as a LONGER fragment program (loads from tile storage, 00000279 fffa0000,
  ops, stores): blending is done in the fragment program (tile based), not by fixed function.
- No-blend scene 8 versus 9: fragment program at res_3+0xc80 is the only code change besides the flag.
NEXT: cluster CoreAnimation's own pipelines (capture5) by fragment-program content to count the distinct shapes, then build the software executor for those shapes.

### 16d. CoreAnimation's own programs (capture7 = backboardd, full-dump kernel 43709240919, 2026-11-15)
- 45 sessions, 24 distinct shader-state references; vertex refs 0001001f/{6150e0 x115, 61abb0 x31, ...}, fragment refs 0011011f (differs from my test's 0009051f).
- Fragment programs sit in res_10 (VA 0x1000e0000.., e.g. ptr 0x12ec10 = res_10+0x4ec10) and res_3 (ptr 0x2fd20.. NOT captured: res_3 lines only up to +0x9880,
  most likely the 4 MB kernel log wrapped / later writes missing; fix = smaller per-client budget or streaming the log).
- Real programs (res_10 +4ec00 / +4ecc0) are ~40-60 words of the same instruction format as my test shaders, with repeated "xxxx c2 ca 0004 c424" groups (texture sample / interpolate),
  and end with the same marker  00000089 ff700000 00000108  (or 00000209 fff20000 00000108 in my tests).
NEXT: dump pairs (vertex+fragment) per CoreAnimation pipeline, hash the fragment words, build a table: hash -> operation (textured quad / solid / blend / rounded-corner mask), then
the kernel executor. Log capacity: switch the capture to dump programs only (regions type 0 with ISA end marker) to stay under 4 MB.

## 17. ISA via Mesa's Asahi tables (2026-11-15)  tools/agx-disasm.py + tools/agx_opcodes_mesa.py (copy of Mesa src/asahi/compiler/agx_opcodes.py, MIT)
- The A11 (G10) ISA is a close relative of the M1 (G13) one that Mesa's compiler targets: same opcode bytes for iadd 0x0E, imad 0x1E, wait_pix 0x48 (4 B), st_tile 0x09 (8 B), get_sr 0x72, device_load 0x05, texture_sample 0x31 ... Differences found so far (agx-disasm.py G10_OVERRIDES):
  * mov_imm = 0x52 (M1: 0x62), 6 bytes: 52 <reg*2> <imm32 LE>   [green test: r0=0 r2=0 r1=1.0 r3=1.0 = RGBA (0,1,0,1)]
  * ld_tile = 0x79 (M1: 0x49), 8 bytes, same shape as st_tile  [blend scene: tile load of the destination colour]
  * program end = 08 01 00 00 (M1: stop = 0x88) after the last st_tile
- Fragment epilogue is the M1 one: wait_pix (48 ..) ; st_tile (09 02 .. f2 ff) ; end.  Blend scene = wait_pix, wait_pix, ld_tile, float ops (4a/ca ...), wait_pix, st_tile, end.
- Unknown: float arithmetic opcodes (4a, ca ...), compare/select, iter (varying interpolation), texture sample on the A11. Calibrate on the compute kernels with known maths (scratch/agx/capture3/4, tools/agx-kernels*.txt).

### 17b. Calibration from the known compute kernels (capture4, tools/agx-kernels2.txt)
- Every kernel: prologue  e2 06 10 04  (4 B thread-id fetch, G10 low byte 0xE2 = 0x62|0x80; M1 get_sr is 0x72: the 0x?2 family sits 0x10 lower on the A11, like mov_imm 0x52 vs 0x62);
  epilogue  0e 00 c0 c6 1a 02  (iadd with shift: address = base + index<<shift) ; 45 11 00 01 00 c0 12 00  (device_store, same as M1 0x45|1<<47, 8 B) ; 08 01  (end).
- "constant store 7": e2 06 10 04 | 52 04 07 00 00 00 (mov_imm r2 = 7) | c2 00 00 00 | c2 02 02 00 | address iadd | device_store ... : 'c2 ..' 4-byte = register/uniform setup (base address), unknown yet.
- i*i: 8e 10 86 86 0d 00 = imad-like (opcode 0x8E = 0x0E|0x80).  mad float i*0.25+3: be 12 0a 86 61 00 (convert u32->f32?) then ca 11 c4 10 04 08 00 40 (ffma, 0xCA).  min(i,20): 1e 11 86 14 0b 86 14 00 / 4e 10 86 c4 15 00 .
- NEXT: write down each of the ~30 distinct instruction forms with the operand layout from agx_pack.c (how operands/flags are packed), validate on the 55 kernels (every kernel's output is known), then the same decode on CoreAnimation's fragment programs.

### 17c. Single-operation scenes (capture8 = 25 render scenes, kernel v118 image; tools/agx-scenes.py, agx-disasm.py G10_OVERRIDES)
- Program found via the fragment state record {0x0009..1f, VA low, [extra], 0x108}; later scenes reuse the same VA (0xc80) so the record does not change (carry the last pointer).
- A11 opcode bytes confirmed: iter 0x51 (8 B), fmul 0x4A (6 B; 0x1A = 8 B long form), fadd 0x8A, ffma 0xCA (8 B; saturate scale uses it with different flags), unary funops 0x0A (6 B, function code in the later bytes), mov_imm 0x52 / 0xD2 (flagged, e.g. +inf), wait_pix 0x48, signal_pix 0x58, st_tile 0x09, ld_tile 0x79, get_sr 0xE2 (4 B), end 08 01 00 00. M1 -> A11: the float ALU selector moved from bits 4-5 (0x1A/0x2A/0x3A) to bits 6-7 (0x4A/0x8A/0xCA).
- Coverage: 11/25 scenes fully decoded (0 unknown); unknown forms left: unary function codes (sqrt rcp exp2 log2 sin floor), compare/select (select, min/max, discard), swizzle long forms.
- Operand notes: in.col*0.5 -> 4a 01 c0 20 04 00 ; in.col*in.col -> 4a 01 c0 c0 24 00 ; fadd imm -> 8a 01 c0 08 04 00 ; fma -> ca 01 c0 20 04 10 00 00. Source operands use the M1 ALU source packing (agx_pack.c agx_pack_alu_src: 8-bit minifloat immediates, abs/neg flags): next = decode operands with it.
- NEXT: operand decode + a CPU emulator of the decoded subset (iter, fmul/fadd/ffma, mov_imm, st_tile RGBA8) validated against the known formulas of the 25 scenes and the 55 compute kernels; then CoreAnimation's own programs (capture7 + a full program-heap capture).

### 17d. Operand layout evidence for the A11 6/8-byte ALU forms (fmul 4a / fadd 8a / ffma ca)  (capture8)
- Immediates are Mesa's 8-bit minifloat (agx_minifloat.h: sign|exp3|mant4): 0.5 = 0x20, 0.25 = 0x10, 0.125 = 0x08, 2.0 = 0x40 (and its top two bits go to bits 6-7 of the next byte: 'ca 01 c0 00 44 20 00 90' = fma(r0, 2.0, 0.5 with a flag in the last byte) for saturate(x*2-0.5)).
- Registers are in half-register units: byte value 0xc0 + 2k = register k of 32 bits, bits 6-7 = hint (0b11 here); destination byte 1 = 0x01 + 8k (k = destination register).
- fmul imm:  4a 01 c0 20 04 00 ; fmul reg:  4a 01 c0 c0 24 00 (byte 4 bit 5 = source 1 is a register) ; fadd imm: 8a 01 c0 08 04 00 ; fma: ca 01 c0 20 04 10 00 00 (src1 = byte 3, src2 = byte 5).
- Mesa's 12-bit-slot layout (sources at bit 16 + 12 s) does NOT line up byte-wise with this, so the A11 packs the source slots differently from the M1; confirm field by field with more scenes before writing the emulator.

### 17e. Texture sampling + round 3 scenes (capture9 = 35 scenes, tools/agx-scenes2.txt)
- A11 texture_sample = opcode 0x21, 10 bytes: 21 01 00 41 0f c8 00 80 00 20  (byte 4: component mask 0f = RGBA, 07 = RGB; c8 = coordinate register). M1 has it at 0x31 (and iter at 0x21): the earlier "iterproj" labels in CoreAnimation's programs were really texture samples.
- Sequence of a textured fragment shader: wait_pix; iter (4-byte form 51 03 81 00 interpolating the texture coordinates); texture_sample; wait (38 00) for the result; fmul/fadd on r0-r3; wait_pix; st_tile; 08 01 00 00.
- Nearest and linear sampling give BYTE-IDENTICAL shader code: the filter lives in the sampler STATE (descriptor memory), not in the program.
- tex tint = fmul r1*0.5, r2*0.25 after the sample; tex premult = three fmul with register sources (c.rgb * c.a).
- Coverage now: scenes 26-29 (texture) 0 unknown; still unknown: blend-with-texture (30), uint bitops (31), int compare/select (32), fract/abs (33), pow (34), unorm pack (35), sqrt/rcp/exp2/log2/sin/floor funops, select/min-max/discard.

### 17f. Where a render pass lives (clear-only scene 1 vs draw scene 3, capture9; tools/agx-cmpsess.py, agx-findva.py)
- RENDER TARGET: the 64x64 BGRA texture is a kernel-allocated resource (res 11, VA 0x100060000, 0x4000 B = the stand-in owns its memory: the stand-in can write pixels into the app's mapping directly).
  Its address is stored SHIFTED: pass descriptor record  1fe3050a 000007e0 <VA>>4> 00000200  (0x10006000 = 0x100060000 >> 4); also lo32 in the kernel command list (shmem type 1): ... 00040004 00040001 00010001 00060000 ...
- PASS DESCRIPTOR (res_18, 0x40 B records): fb05050a 00007e01 40018000 | .. | 000e0000 00000340 | 3f800000 0 0 3f800000 | clear colour RGBA floats at +0x38..+0x44 | then the render-target record at +0x48.
- DRAW PRESENCE: appears as the vertex-state record {0001001f, VS VA low, 00000108} (res_19+0x40) and fragment-state records {0009..1f, FS VA low, [extra], 00000108} (res_20), a VDM-like control stream in res_28 (0000001f 00240000 102e0f00 0e000000 ... : pipeline state pointers), the viewport block in res_29+0x380 (42000000 42000000 42000000 c2000000 = 32 32 32 -32), bind tables in res_14 (+0x54.. 00000301 000300a0 ...), shader code in the program heap (res_13 / res_3).
- Clear-only scenes write none of the vertex/fragment records.
MILESTONE 1 PLAN (kernel, no Mali yet): at submit scan resources for the pass descriptor (fb05050a/00007e01, 1fe3050a/000007e0), fill the target with the clear colour; if a fragment-state record is present run the decoded fragment program (C port of tools/agx-emu.py: mov_imm/iter/fmul/fadd/ffma/st_tile) per pixel with the analytic varying of the test's full-screen triangle; the test's own readback (pixels (0,0),(1,0)) then proves the first correct GPU-stand-in output. Then generalise (vertex program + rasteriser), then move the executor to the Mali.

## 18. Vertex program forms from the round-5 minimal shaders (2026-11-15, logs/sweetlog-round5.txt, scratch/agx/capture11; session n = scene vA+(n-1))
Entry pointer = the vertex shader state record's low VA (res_3 + ptr). Observed:
- Export instruction (4 B): `01 <8*reg+1> <slot> 80`, last one `81 ...`; reg = (byte1-1)/8 (32-bit register index), slot 0x40..0x43 = position x,y,z,w, 0x44..0x47 = varying 0 components (verified: vC pos=vb[vid] exports r0..r3 to 40..43; varying (1,0,0,1) exports r4,r6,r6,r4 to 44..47 where r4=1.0, r6=0.0).
- Shader end: `08 01 xx 00` after the exports (xx = 0x43 in vC, 0x00 elsewhere); trailing bytes are not instructions.
- Vertex fetch prologue (20 B) when the shader reads a buffer or the vertex id: `c2 02 02 00` `0e 00 c0 ca 1a 04` `05 01 00 05 00 <c8|c4> <f2|32> 00` `38 00` = (constants setup) + iadd (address from vertex id) + device_load of the vertex (c4 32 = 4 components -> r0..r3, c8 f2 = 2 components -> r0..r1) + wait. Constants 1.0/0.0 are placed in registers by the prologue (vC: r4=1,r6=0; vD (xy only): r2=1,r3=0).
- Scene vB (pos.x = vid*0.5, no buffer): no prologue; `be 02 0a ca 61 00` (convert uint->float of vertex id, 6 B), `52 04 ..` / `52 02 .. 80 3f` mov_imm 0.0/1.0, `4a 01 c0 20 04 00` fmul by minifloat, then exports.
- Constant position shader (vA): only mov_imm + exports (in the huge first session).
NEXT: confirm constants-in-registers rule with vD/vE/vF/vG/vH bodies (vF col*2 = `8a`/`4a` forms with minifloat immediate; vG xy+0.125 = `8a 29 40 08 04 00`), then write the vertex interpreter: fetch vb[vid] (stride from the vertex buffer descriptor, 16 B in these scenes) into r0..rN-1, run fmul/fadd/ffma/mov_imm, collect exports by slot.

## 19. Draw packet and bound-state locations (round 4 capture, scratch/agx/capture10, 2026-11-15)
- Draw packet in the command stream region (res_14 + 0x98): `001b0000` `6e44 0p00` (p = primitive: 6 triangle list, 9 triangle strip) `<vertex count>` `<instance count - 1>` `00000000` `000000aa`; instanced scene: count word 6, next word 1; indexed scene: `001b0000 50004000 0000ffff 7e420601 00005200 <count> ...` (index type/offset words before the count).
- Vertex buffer table (res_31+0x380 / res_29+0x380 by session): u64 VA of the vertex buffer at +0x10 (0x1000f0000 here) followed by flags; the same block holds the viewport floats (translate x, scale x, translate y, scale y) = 32, 32, 32, -32 for the 64x64 default and 32, 16, 32, -16 for viewport 16,16,32,32, then 0.0, 1.0, 0.0 (the uniform file that the hardware pushes into shader registers: the vertex programs export r-registers holding 1.0/0.0 that no instruction writes).
- Scissor, viewport and the pipeline VAs live in res_28 and res_14 + 0x40 (`...00110044 <n>`), not yet decoded per field.
OPEN: uniform-to-register mapping (which uniform lands in which register: vC r4=1.0 r6=0.0, vD r2=1.0 r3=0.0), index-buffer packet fields, per-draw scissor/viewport fields. These are needed before the vertex interpreter can produce positions for arbitrary programs.
- Section 19 addendum: the vertex shader state record is preceded in res_19 by a 2-word uniform-push record `<VA lo | 3> <hi | n<<8 | 3>` (vC/vE/vG: `90400003 01002f03`, vD/vF/vH: `90400003 01002203`): the 0x2f vs 0x22 byte follows the number of preloaded register halves (4 loaded components + constants vs 2 + constants); the pushed words are the 0.0 / 1.0 constants after the viewport floats in the bound-state table (res_31+0x380: 32,32,32,-32, 0.0, 1.0, 0.0). The register the constants land in = first register after the loaded vertex components (+ a gap), which is why vC exports r4=1.0, r6=0.0 and vD r2=1.0, r3=0.0. Needs one more probe (a shader using a constant that is neither 0 nor 1) to fix the exact layout.

## 20. Milestone 2 hardware results (kernel run 43761561031, image v127-exec7, logs/sweetlog-exec7.txt, 2026-11-15)
Software executor (patch 0150) on the phone, triangle draws from the round 5/6 test shaders (64x64 BGRA8 target):
- Coverage: 512 pixels per 32x32 half-square triangle (exact), 2154 for the large one.
- Constant colour exports (0.3, 0.6, 0.9, 0.2) -> 334d99e5 (BGRA8 round-to-nearest of the float, B=0xe5 since 0.9 = 0.89999998).
- Solid red -> ffff0000; zero colour -> 00000000; pass-through colour = vertex data -> smoothly interpolated values with alpha = interpolated vertex w.
- WRONG: col = vb*2 (vF) writes 0: its multiply (`4a 31 40 00 04 10`, `4a 29 42 00 04 10`, `4a 21 c4 00 04 10`) keeps the constant outside bytes 3/4: byte5 = 0x10 is probably the operand-flags/extension; imm decode (b3 | (b4>>6)<<6 = 0) is wrong for this form.
- Not run (unknown ops): vertex id / instance id shaders (get_sr e2, convert be, iadd 0e, c2 forms with data), indexed draws, instancing, textures, blending.

## 21. The compositor's shader corpus (logs/sweetlog-progs1.txt, kernel s8agxprogs=backboardd, 2026-11-15)
48 distinct vertex programs from backboardd. Structure of every one:
- 32-byte prologue region: `c2 00 00 00 c2 02 02 00` | `8e 20 ca 30 26 80 c0 00` (8 B integer multiply-add: attribute address) | `05 01 08 05 00 c8 f2 00` and `85 20 48 05 00 c8 f0 00` (8 B device_loads: two attribute streams, position and a second one) | `38 00` wait.
- Body: alternating `c2 <8*? > <uniform idx> 00` (4 B: NOT a no-op; copies a uniform into a register) and `4a`/`8a`/`ca` (fmul/fadd/ffma, 6/6/8 B) = the matrix transform, then `01 <8r+1> <slot> 80` exports (pos 0x40-43, varyings 0x44-47+), end `08 01`.
- Fragment programs were NOT captured (record header filter too narrow); the next capture logs every 0x0001xxxf / 0x0009xxxf record with its header and 8 dwords before/after.
To execute them the executor needs: attribute fetch semantics (device_load format/stride/base, the iadd/imad address), the uniform file (values pushed per draw by the uniform-push record `90400003 01002f03` and the bound-state table) and the c2 copy semantics.

## 22. Compositor fragment programs (logs/sweetlog-progs2.txt, 6 captured, 2026-11-15)
Fragment state records the compositor uses: headers 0x0009012f / 0x0009052f / 0x0009055f (low nibble f, bits 4-7 variants), ptr | 1, then 0x00010058 or 0x108. Example program 0x10012fc30 (a blend/composite shader): starts with 4 x `79 xx 00 0c 00 04 fc 01` (ld_tile: read the framebuffer for blending), then forms not decoded yet: `82 10 08 00`, `a2 11 00 00`, `a2 12 30 00` (4 B), `8e 4e d1 d0 00 80 d2 00` (8 B integer/float multiply-add family), `0e 40 d3 d0 00 00` (6 B), `c6 4a cc 10 00 00 00 00` and `c6 32 cd 10 00 d2 00 00` (8 B: probably texture-coordinate / conversion ops), `02 04 04 00`, `31 01 00 0a 10 d0 00 04 75 2a 75 29` (texture sample + store?), end `08 01 00 00`. Needed before the blend/texture shaders can run: decode of 0x79 (ld_tile), 0x82/0xa2, 0x8e/0x0e (integer ops), 0xc6, 0x31, 0x75, 0x02.

## 23. Uniform push records (partial decode, capture10 session 40)
The 2-word records before a shader state record are uniform pushes: `<w0> <w1>` with w1 = VA >> 8 of the source block (verified: `01000f00` -> 0x1000f0000 = the vertex buffer, `01002f03` -> 0x1002f03xx = the bound-state table in res_31) and w0 = `<?> <?> <dest uniform index?> 03` (type 3): 0x90400003 (table, 0x40 words?), 0x64200803 (vertex buffer pointer, dest 8?), 0xb2000803 / 0xc0802c03 in the compositor. The fragment uniform colour (0.2,0.4,0.6,1.0) of the uniform-colour draw lives at res_18 + 0x70 right after the pass target record. Not yet fixed: which bits of w0 are the dest register, count and the low VA byte; needs 2-3 shaders with uniforms of known sizes pushed to known registers.

## 24. Fragment side: ld_tile blend and texture descriptor leads (offline, 2026-11-18)
- ld_tile (8 B `79 02 00 00 00 00 fa ff`) loads the destination pixel into r0..r3 = R,G,B,A (floats, unorm8/255); the blend of scene 9 is ordinary fmul/ffma after it (r0 = d.r*0.5+0.5 ...). Implemented in s8gfx_frag.h (ctx->tile, has_tile) and host-tested (scratch/agx/blend_test.c: white, black and grey backdrops exact). The compositor's blend shaders use FOUR ld_tile (`79 32/22/12/02 .. fc 01`), one per channel with b1 high nibble = register 3,2,1,0 (hypothesis), flags in b3/b5 = 04; not decoded.
- Texture memory layout (4x4 RGBA8 test texture at res_1+0x5100): Morton order with y in the low bit: (0,0),(0,1),(1,0),(1,1),(0,2),(0,3),(1,2),(1,3),(2,0)... Pixel bytes R,G,B,A.
- Texture state: `1b44050a 00000600 <VA>>2` in res_23 (session 26) points at the pixels (0x40001440 = 0x100005100 >> 2). Fragment-stage pushes before the fragment record have types 0x17 / 0x13 (`00100017 01001c00`, `a0100013 01000300`): w1<<8 | w0>>24 = source VA (here res_4 = 0x100030000 + 0xa0), i.e. the descriptor/sampler words `000e0000 00c00780` / `000c0000 00000004` in res_4 at +0xa0. Dimensions/format/filter fields inside those words are not decoded (needs one controlled experiment per field: sizes 2x2/8x8, formats, nearest/linear).

## 25. Compositor vertex corpus vs the interpreter (offline, scratch/agx/corpus_run.c + corpus.bin, 2026-11-18)
With synthetic uniforms (0.5f everywhere) and memory (0.5f), 35 of the 50 captured backboardd vertex programs run to completion in s8gfx_frag.h (all four position exports present). The other 15: 11 start at a mid-instruction offset (the start finder found no prologue: `d0 40 24 00 8a 51 ..` / `d4 40 24 00 c2 18 ..` are tails of ffma instructions; these programs have a different prologue shape), 2 hit imad (0x1e, 8 B `1e 04 46 00 60 00 20 c0`), 1 hits `23 c4 c0 00` (4 B, unknown), 2 start inside a device_load. Needed: start finder for prologue-less/other-prologue programs (anchor on the exports + walk back), imad 0x1e, op 0x23.

## 26. Hardware check of the register-operand ffma (kernel run 44377372329 built on the user's MacBook runner, image v133-ffma, logs/sweetlog-ffma1.txt)
12 draws now write pixels (was 10). Two-uniform shader xD (pos = vb.xy*u.xy + v.zw, col = v): 192 pixels, colour 004d9920 = (0.3,0.6,0.125,-0.25) BGRA8 clamped: exact. Matrix shader xB (pos = m*(vb.xy,0,1)): 72 pixels (triangle scaled 0.5 x 0.25 at the translated position). Register third operand of ffma (byte 6 bit 2) confirmed. CI note: GitHub-hosted runners stopped; the xnu workflow runs on the self-hosted MacBook with -f runner_labels='["self-hosted","macOS","X64"]'.

## 27. Full-length compositor corpus on the phone's own capture (kernel run 44391026481 on the MacBook runner, image v135-progs4, logs/sweetlog-progs4.txt, scratch/agx/corpus4.bin, 2026-11-18)
- Vertex: 47 distinct backboardd vertex programs captured at 768-byte windows; with the new start finder and the uniform-file interpreter 43 of 47 run to completion offline (synthetic uniforms). Remaining 4: 1 x imad (0x1e, 8 B `1e 04 46 00 60 00 20 c0`), 1 x `23 c4 c0 00` (4 B, unknown), 2 x start inside a device_load.
- Fragment: 6 programs captured, 0 run: two are an empty program (`08 01 00 00`, no-op shaders), the others stop at: 0x1e (imad), `82 10 08 00` / `a2 11 00 00` (4 B forms), `c6 4a cc 10 00 00 00 00` (8 B), and the four-way ld_tile `79 32/22/12/02 .. fc 01` (per-channel loads in raw form, vs the float form `fa ff` of scene 9). Likely a packed-pixel path: ld_tile raw -> unpack (82/a2/c6) -> float math -> pack. Needs controlled fragment shaders with known pack/unpack behaviour.

## 28. Convert / pack forms from my own scenes (capture9, scenes 31-35; offline decode 2026-11-18)
- Scene 35 (unorm pack) program body: `52 02 00 00 00 00` (mov_imm r1 = 0), `be 02 08 c0 21 00` and `be 02 0a c0 61 00` (6 B, opcode 0xbe = convert: byte 5 0x21 / 0x61 = source/destination type and rounding variant; this is the float <-> uint8 conversion), `d2 06 81 80 80 3b` (6 B, 0xd2 = mov_imm with bit 7 set: r3 = 0x3b808081 = 1/255), `4a 01 c0 c6 24 00` (fmul r0 = r0 * r3), then wait_pix / st_tile as usual. So unorm unpack of a byte = convert (0xbe) + multiply by 1/255 (0x3b808081), pack = multiply by 255 + convert.
- The compositor's fragment forms 0x82/0xa2 (4 B), 0xc6 (8 B) and the raw four-way ld_tile have not been matched to a test scene yet; candidates are the 0xbe family (type bytes) for pack/unpack and 0x1e (imad) for integer glue. Next controlled experiment: fragment shaders with `uchar4` / `uint` arithmetic on [[color(0)]] (programmable blending) to force the raw ld_tile path.

## 29. Static route: what the iOS image and the public G13 reference give us (2026-11-18)
- Apple's A11 compiler (AGXCompilerCore-S2A8) is inside the dyld shared cache of the IPSW (`ipsw dyld extract ... AGXCompilerCore-S2A8`, 823 KB, extracted to WSL /root/work/agx-static). The framework dir on disk is a stub; AGXCompilerService-S2A8 (71 KB) only loads it. The image carries LLVM symbols but NO instruction mnemonics: the encoder is numeric tables, so recovering the ISA from it is a real decompile job (not started). The AGXMetalA11.bundle *.bin files are lookup tables (bit patterns), not shaders.
- Public reference (read as documentation only, NOT executed: the classifier blocked running the downloaded python): dougallj/applegpu (BSD-3-Clause), scratch/applegpu/applegpu.py + README + LICENSE. It documents the G13 (M1) encoding with field bit positions. Opcode map (G13 low bits) vs what the A11 programs show:
  - SAME on the A11: iadd 0x0E (8 B), imadd 0x1E (8 B: a*b + (c << shift), A at bit 16, B bit 28, C bit 40, D bit 60), convert 0x3E (6 B), bitop 0x7E (6 B), device_load 0x05 / device_store 0x45 (8 B, format fields incl. unorm8: an 8-bit store+load round trip is how the compiler does pack/unpack), stack_load/stack_store 0x35-family, wait 0x38 (2 B), get_sr 0x72 (4 B), st_tile 0x09 (8-10 B; the load flag is bit 6), end 0x08 0x01 here vs stop 0x88 there.
  - PERMUTED on the A11: float ops (A11 0x0A base with bits 6-7 = fmul 4A / fadd 8A / ffma CA; G13 has distinct fmul 0x1A / fadd 0x2A / fmadd 0x3A bases), mov_imm (A11 0x52 vs G13 0x62), iter (A11 0x51 vs 0x21), texture_sample (A11 0x21 vs G13 0x31), ld_tile (A11 0x79).
- Consequences: my earlier labels 0x45/0x05 = pack/unpack in the fragment stage were wrong: they are device_store/device_load (the numerical effect of the round trip is the same, so the interpreter result stays valid). 0x1E = imadd (needs implementing from the pseudocode: result = a*b + (c << shift), 32-bit operand forms).

## 30. First capture of backboardd's real GPU submits, taken INSIDE the QEMU emulator (2026-11-19, logs/sweetlog-emu1.txt, scratch/agx/capture16)
Method (works, repeatable): `tools/s8-qemu/run.sh --fresh --headless --gdb --smp 1 --bootargs "s8agxexec=backboardd s8agxdump=backboardd s8agxskip=0 s8panicwait=300"` (KERNEL=out-v103), let it run ~240 s, the kernel flushes its console ring periodically to the last 1025 blocks of USERDATA (block 0x400cff) of the qcow2 overlay (`/root/work/s8-qemu/run/overlay.qcow2`); read it with `qemu-io -r -f qcow2 -c "read -v <off> <len>"` (qemu-img dd fails on this overlay) and decode with tools/decode-sweetlog.sh; rebuild sessions with tools/agx-capture2.py. (The repo's read-log.sh looks at the wrong block on this disk layout.) Result: 29,500 log lines, 140 region dumps, 9 sessions, 48 regions.
Compositor pass descriptors (res_20 of that capture) use the same header `fb05050a` as my test passes but NOT the same layout: second word 0x00003e00 (test: 0x00007e01), no `1fe3050a 000007e0 <VA>>4` target record at +0x48; instead a run of attachment records `0305050a 00000000 40000c80 ...` / `0305050d 00000000 40000cc0 ...` (4 words each, repeated, 0x40000c80 / 0x40000cc0 look like VA-derived pointers) and pairs `028e0000 00c00780` / `000e0000 00c00780` (0x00c00780 = 12 * 256... possibly dimensions 0x0780 = 1920, 0x00c0 = 192?). So the executor's pass finder (test layout only) finds no pass: the compositor's pass format and render-target pointers must be decoded next. Vertex records `0001001f/2f` are present (res_16); fragment records are in regions not dumped in this capture (raise the dump cap or dump more regions).
