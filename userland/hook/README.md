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
| `eagl_s8.c` | STAGE 2: `libs8gl.dylib`'s EAGLContext replacement (port of the S9+ `eagl.c`, trimmed; see its header for what is a TODO). |
| `gl_osmesa_s8.c` | STAGE 2: `hook_gl.h` over Mesa OSMesa(ES), loaded with `dlopen` at run time; Panfrost on S8Mali or softpipe. |
| `eagl_slots.txt`, `mkeagl_s8.py`, `eagl_gen_s8.h` | the function table at Apple's offsets: slots (unchanged S9+ file), generator, generated header (committed; gl.xml of KhronosGroup/OpenGL-Registry commit 1cdd228e, 2026-11-23). |
| `stubs/`, `check-host.sh` | host syntax check with a Linux clang (no iOS SDK): `wsl bash userland/hook/check-host.sh`. |

Status: libs8gl compiles (host syntax check only); nothing of stage 2 has been built by CI or run on a phone.

## libs8gl.dylib (stage 2)

Build: `build-hook.sh` (also run by `.github/workflows/userland.yml`) -> `libs8gl.dylib`, install name `/usr/lib/libs8gl.dylib`, ad-hoc
signed with `ldid -S`. It does not link or include Mesa: Mesa is `dlopen`ed from `$S8GL_MESA`, `/usr/local/lib/s8mesa/libOSMesa.8.dylib`
(where `s8gl_test` looks), `/usr/lib/s8mesa/` or `/usr/lib/` at the first `initWithAPI:` (next to it `libglapi.0.dylib`; the Mesa artifact of
`mesa.yml`). Without Mesa `initWithAPI:` returns nil exactly like today and the log says why.

Load it into backboardd with `hook_inject.c` (it dlopens `/usr/lib/libs8gl.dylib` unless `gl=0`) or insert it directly; its constructor only
acts in `backboardd` (`S8GL_ANY=1` in the environment: any program, for a test) and replaces `initWithAPI:properties:` (and `initWithAPI:`,
`initWithAPI:sharegroup:`), `API`, `sharegroup`, `setParameter:to:`, `getMacroContextPrivate`, `texImageIOSurface:...`, `attachImage:...`,
`presentRenderbuffer:`/`renderbufferStorage:fromDrawable:` (log only), `dealloc` by name. If `libhook_log.dylib` is also loaded, whichever
installs last wins for each method (libs8gl never calls the originals, so hook_log's log then stops for those); load only one of them.

Milestone A (what to read on the first run): `/private/var/tmp/hook_log.txt`, lines `s8gl: ...`:
`libs8gl loaded`, `-[EAGLContext ...] replaced` (12 lines), `Mesa loaded from`, the driver decision (`the S8Mali service is there; the DRM table was
given to Mesa` or `no S8Mali service` or `software rendering asked for`), `GL_VENDOR ... GL_RENDERER` (`softpipe` or `Mali-G71 ... (Panfrost)`),
`an OpenGL ES 3 context for CoreAnimation`, then per call kind: `-setParameter`, `-getMacroContextPrivate ... table`, `first call of table entry 0x.. glX
(expected|NOT expected by QuartzCore's list)` (the list of GL functions CA really uses), `-texImageIOSurface:` / `-attachImage:` with surface id,
pixel format, size, `a new kind of texture`, shaders (the first two in full, any that do not compile with the compiler's message), a compressed
picture and whether the driver refused it (`ASTC`), `frame N: ...` lines, `-presentRenderbuffer: CALLED` if CA ever does that, and a summary every 200 frames.
`gltrace=A-B` (default 1-2) writes down every call of those frames. Force the CPU: boot-arg or environment `s8gl_sw=1` / `S8GL_SOFTWARE=1`.

Open (see the header of `eagl_s8.c`): the function-specific draws (vibrant colour matrix, blurs, groups; need CA's function number, whose structure
offsets are not verified for 20H392), zero-copy IOSurface textures (needs a Mesa patch 0006 = the S9+ `panfrost-darwin.patch` IOSurface hunks, and S8Mali
selector 0x7e, `s8mali_drm.h`), and the real-world tuning the S9+ file has (blit targets, host-presented frames).

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

## Stage-2 notes

* `libs8gl.dylib` = eagl.c equivalent (written, see above): constructor replaces the EAGLContext methods, keeps a table
  of `HOOK_GLTABLE_SIZE` bytes whose entry at slot S (eagl_slots.txt) calls the Mesa function of that
  name with `table[0]` = our context struct as first argument (`mkeagl_s8.py`, from the S9+ `mkeagl.py`).
* `hook_gl_*` over OSMesa ES profile (S9+ `gl_osmesa.c`) with Mesa 25.0.7 + Panfrost + S8Mali shim: `gl_osmesa_s8.c`.
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
