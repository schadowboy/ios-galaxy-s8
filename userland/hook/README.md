# userland/hook: putting CoreAnimation's GLES renderer on the Mali (S8, iOS 16.7.16 / 20H392)

Port of the *structure* of the S9+ port's backboardd injection (ref/s9plus/core/userland/src/inject.c
and eagl.c, MIT, Copyright (c) 2025-2026 the HakiOS authors) to our cache. Full analysis:
`analysis/ca-hook-port.md`. Nothing here is built in CI yet and nothing has been run on the phone.

| file | what |
|---|---|
| `hook_log.c` | FIRST EXPERIMENT: log-only dylib for backboardd; every EAGLContext method calls the original. |
| `hook_inject.c` | constructor for backboardd: checks cache UUID, hooks two EAGLServer virtuals (log), dlopens the stage-2 GL library when present. |
| `hook_addrs_20H392.h` | the derived address table (old 20F66 values side by side, how each was found). |
| `hook_gl.h` | interface the stage-2 library implements (OSMesa/Panfrost, surface-as-texture, wait). |

## Design

* ObjC methods (all 8 EAGLContext selectors, `-[CAWindowServer _detectDisplays]`, ...) are replaced by
  **name** with `class_replaceMethod`; the S9+ port already does that for EAGLContext, only its
  C++ vtable patches are address based.
* C++ virtuals (`EAGLServer::renderer`, `render_update`, `GLESContext::function_*`) are patched in the
  vtable at `unslid + slide`; slide = `_dyld_get_shared_cache_range()` - 0x180000000. Local symbols
  cannot be dlsym'ed, so these are the only absolute values; they are verified (the slot must hold the
  expected function) and the cache UUID is checked first.
* The GL dispatch table (`-getMacroContextPrivate`) uses the S9+ `eagl_slots.txt` unchanged.

## How the dylib gets into backboardd (open: needs a kernel change)

S9+ does NOT use a launchd plist: launchd refuses `DYLD_*` from job descriptions. Their XNU patch adds
`sysctl kern.exec_inject = "LIB NAME NAME...[; LIB NAME...]"` (xnu/hakios.patch around line 720, APSL;
exec_extract_strings appends `DYLD_INSERT_LIBRARIES=` for matching program names), set by their init
before launchd. Our kernel patches have **neither** that sysctl **nor** the AMFI stand-in's dyld policy
reply: S9+ `bsd/kern/kern_minimac.c` answers AMFI policy syscall 0x5a ("check dyld policy") with
allow-all (0xff) and 0x60 (boot-arg state 0x86); our `sweet_amfi.c` patches register no such handler
(grep of all patches: `mpo_policy_syscall` exists only in the "Sandbox" policy; the AMFI one is named
"SWEET AMFI" and has none), so dyld most likely ignores DYLD_* in a platform binary (unverified on
device: depends on how dyld4 treats a failing `amfi_check_dyld_policy_self`). Two ways, pick one (both need the kernel/root image, not done here):
1. port the two S9+ pieces (exec_inject sysctl + AMFI 0x5a/0x60 handler) as a new kernel patch (APSL), or
2. add an `LC_LOAD_DYLIB` to `/usr/libexec/backboardd` (our AMFI is permissive: signature irrelevant),
   plus the dylib in the root image. Simpler, no kernel change, but modifies the binary.

## Stage-2 TODO (documented, not written)

* `libhook_gl.dylib` = eagl.c equivalent: constructor replaces the 8 EAGLContext methods
  (`initWithAPI:properties:`, `API`, `sharegroup`, `setParameter:to:`, `getMacroContextPrivate`,
  `texImageIOSurface:...invert:`, `attachImage:toCoreSurface:invertedRender:`, `dealloc`), keeps a table
  of `HOOK_GLTABLE_SIZE` bytes whose entry at slot S (eagl_slots.txt) calls the Mesa function of that
  name with `table[0]` = our context struct as first argument. Reuse S9+ `mkeagl.py`.
* `hook_gl_*` over OSMesa ES profile (S9+ `gl_osmesa.c`) with Mesa 25.0.7 + Panfrost + S8Mali shim.
* NOT replaced by S9+ (and not by us): `+setCurrentContext:`. Our cache's body (0x1d3f517ec) reads
  `[self+8]` (the `_private` ivar, never set by a replaced init, so NULL) and compares it with
  TSD[0xf0] (also NULL since nothing ever sets it): equal -> returns YES. That is why it works.

## First experiment on the phone

Build `hook_log.c` with the userland workflow, put it at `/usr/lib/libhook_log.dylib` in the root image
(a new file, root image otherwise untouched), get it inserted into backboardd (see above), boot, read
`/private/var/tmp/hook_log.txt`. Expected first visible result: the line `hook_log: loaded in
backboardd`, then `-[EAGLContext initWithAPI:3 ...]` calls (from `EAGLServer::renderer()` ->
`GLESContext` creation) with the result `nil` or a stock context. If `initWithAPI:` is never logged
then CoreAnimation never reached the GLES road (Metal assumed present or renderer() not called) and
the S9+ `_detectDisplays`/Metal device handling is needed first (ADDR_MTL_DEVICE_ARRAY in the table).
