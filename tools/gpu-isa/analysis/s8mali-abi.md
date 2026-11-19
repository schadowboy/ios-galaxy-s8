# S8Mali IOKit ABI (patch 0133, boot-arg `s8mali_drv=1`)

Legend: [E] = evidence (read in source / verified by dry-run), [G] = guess, not yet run on the phone.
Nothing of the driver has run on hardware yet (0130 self-test did, see gpu-plan.md). Kernel: `xnu/patches/0133-s8-mali-iokit.patch`,
files `osfmk/kern/s8_mali_drv.c` (hardware) and `iokit/Kernel/arm/S8Mali.cpp` (service + user client).

## Service

- Registered by a kernel thread started from `IOKitBSDInitialize` when boot-arg `s8mali_drv=1` (default off). Independent of `s8mali=1` (the 0130 self-test); do not enable both. [E]
- Name: `#define S8MALI_SERVICE_NAME "S8Mali"` at the top of S8Mali.cpp (registry name). The instance is of class `HakiosMali`, a subclass of `S8Mali`, so BOTH `IOServiceMatching("S8Mali")` and `IOServiceMatching("HakiosMali")` find it (IOServiceMatching matches by class, subclasses included) [G for the subclass matching, standard IOKit behaviour; S9+ shim looks up "HakiosMali" so it should work with NO change]. User client class: `S8MaliUserClient`.
- Open: `IOServiceOpen(service, mach_task_self(), 0, &conn)`. The first open resets the GPU and powers L2/shaders/tiler (needs the G3D domain on at boot: it is); the last close powers off and resets. Nobody needs root (no check, as S9+).
- All requests: `IOConnectCallStructMethod(conn, selector, in, size, out, &size)`, in and out are the same struct (same size). Return codes map as in hakios.drm.c `call_raw()`: kIOReturnNoMemory->ENOMEM, BadArgument->EINVAL, Timeout->ETIMEDOUT, NotFound->ENOENT, Unsupported->ENOSYS, NoSpace/other->EIO.
- Buffers: `IOConnectMapMemory64(conn, handle /*memory type*/, task, &addr, &len, kIOMapAnywhere)` maps the WHOLE buffer (what `hakios_drm_map` does), write-combined.

## Selectors (identical numbers and layouts to the S9+ HakiosMali, `hakios.mali.cpp:82-173`)

Selector = low byte of the Linux ioctl code (DRM_COMMAND_BASE 0x40 + panfrost number; GEM_CLOSE is generic 0x09). All structs natural alignment, little endian.

| sel | name | struct (size) | in -> out |
|---|---|---|---|
| 0x44 | GET_PARAM | `{u32 param; u32 pad; u64 value}` (16) | param -> value |
| 0x42 | CREATE_BO | `{u32 size; u32 flags; u32 handle; u32 pad; u64 offset}` (24) | size,flags -> handle, offset = GPU VA (kernel chosen, 0x2000000..4 GiB, 16 KiB aligned, whole BO). flags: NOEXEC=1, HEAP=2 |
| 0x43 | MMAP_BO | `{u32 handle; u32 flags; u64 offset}` (16) | handle -> offset = handle<<12 (ignored; map with IOConnectMapMemory64) |
| 0x45 | GET_BO_OFFSET | `{u32 handle; u32 pad; u64 offset}` (16) | handle -> GPU VA |
| 0x40 | SUBMIT | `{u64 jc; u64 in_syncs; u32 in_sync_count; u32 out_sync; u64 bo_handles; u32 bo_handle_count; u32 requirements; u32 jm_ctx_handle; u32 pad}` (40; 32-byte form also accepted) | jc = GPU VA of the job chain; requirements bit0 FS -> JS0, else JS1. Back: **jc = job number** (the shim puts the address back, as S9+) |
| 0x41 | WAIT_BO | `{u32 handle; u32 pad; i64 timeout_ns}` (16) | timeout is RELATIVE ns (the shim converts Mesa's absolute CLOCK_MONOTONIC deadline, `left_ns()`). Jobs finish inside SUBMIT, so it returns 0 at once (NotFound for a bad handle) |
| 0x7d | WAIT (job number) | `{u64 seq; i64 timeout_ns}` (16) | returns 0 for any number SUBMIT gave out |
| 0x48 | MADVISE | `{u32 handle; u32 madv; u32 retained}` (12) | retained = 1 always |
| 0x09 | GEM_CLOSE | `{u32 handle; u32 pad}` (8) | frees the BO and its GPU pages |
| 0x49 | SET_LABEL_BO | any | accepted, ignored |
| 0x4c | SYNC_BO | any | accepted, ignored (buffers are write-combined) |
| 0x7c, 0x7e, 0x7f | IOSurface seq / IOSurface BO / show | - | NOT IMPLEMENTED: kIOReturnUnsupported (ENOSYS). `hakios_drm_surface()`/`drmPrimeFDToHandle` will fail |
| other | | | kIOReturnUnsupported |

drmGetVersion: done in user space by the shim (name "panfrost", 1.2), nothing in the kernel.

### GET_PARAM (Linux DRM_PANFROST_PARAM_*) [E from S9+ table, values read live from G71 registers]
0 GPU_PROD_ID = `(GPU_ID>>16) & 0xf00f` = **0x6000** (raw is 0x60a0; Mesa 25.0.7 compares exactly; for Mesa 26.3 the raw value would be wanted, not provided) ; 1 REVISION = GPU_ID&0xffff (=1);
2/3/4 SHADER/TILER/L2_PRESENT (64-bit, regs 0x100/0x110/0x120; SHADER_PRESENT=0x17771777); 5 STACK_PRESENT (reg 0xe00, reads 0 on Bifrost); 6 AS_PRESENT; 7 JS_PRESENT; 8 L2_FEATURES; 9 CORE_FEATURES; 10 TILER_FEATURES; 11 MEM_FEATURES; 12 MMU_FEATURES; 13 THREAD_FEATURES (0xac); 14 MAX_THREADS (0xa0); 15 THREAD_MAX_WORKGROUP_SZ (0xa4); 16 THREAD_MAX_BARRIER_SZ (0xa8); 17 COHERENCY_FEATURES (0x300); 18..21 TEXTURE_FEATURES0..3 (0xb0..0xbc); 22..37 JS_FEATURES0..15 (0xc0+4n); 38 NR_CORE_GROUPS = 1; 39 THREAD_TLS_ALLOC = max(reg 0x310, MAX_THREADS); 40 AFBC_FEATURES = 0 (Mesa 25.0.7 reads 0 as AFBC supported). Others: kIOReturnBadArgument.

## Semantics

- One AS0 for all clients and BOs; 4-level AArch64-4K tables, TRANSCFG walk non-cached; every 4 KiB of a BO has a PTE (RW, inner shareable, MEMATTR 0x88888888 as 0130). PAGE_SIZE 16 KiB in the kernel: BO sizes are rounded up to 16 KiB.
- HEAP BO: Mesa asks 128 MiB; the VA range is reserved in full but only `min(size, s8mali_heap_mb MiB)` (boot-arg, default 64, range 4..128) is backed and mapped. [G] If the tiler runs past the backed part: MMU fault -> soft reset -> job lost, logged.
- Jobs: synchronous inside SUBMIT (polling, 1 s timeout). On job fault/timeout/MMU fault: dump `S8MALI:` state, GPU soft reset, cores back on, AS0 reprogrammed, the call still succeeds (job number returned, pixels not drawn), nothing panics. Failed submits are counted ("failed" in the close log).
- Caches: GPU is non-coherent. BO memory is `IOBufferMemoryDescriptor` kIOMapWriteCombineCache (kernel_task, wired), zeroed and `FlushPoC_DcacheRegion`-ed at creation and before free; user mapping inherits write-combine, so no CPU cache maintenance is needed around submit/wait [G: depends on the user mapping really being WC, as on S9+]. Page tables are cleaned to the PoC after each change. If frames come out stale/torn, the fallback would be an explicit flush in SUBMIT/WAIT (not implemented).
- Logging: `S8MALI:` lines for open/close/bo create/free/submit/fault, first N lines then every 1024th (faults are always logged for the first 20).
