# CoreAnimation GLES hook: S9+ mechanism and the port to our 20H392 cache

Legend: [E] = evidence read in a file/cache (path or tool given), [G] = guess / inference, to be verified on device.
Static analysis only. Phone untouched, root images untouched (root image mounted read-only, cryptex via apfs-fuse, both unmounted afterwards).
S9+ files: `ref\s9plus\core\userland\src\{inject.c,eagl.c,eagl_slots.txt,gl_osmesa.c,hakios.gl.h}` (MIT, "the HakiOS authors"), `ref\s9plus\core\xnu\{hakios.patch,files\bsd\kern\kern_minimac.c}`.
Our cache: SystemOS cryptex `087-86593-047.dmg`, `dyld_shared_cache_arm64` (+42 subcaches), iOS 16.7, UUID 8F970452-C9E3-3E88-B114-FEB1864BC9E0, shared region 0x180000000.. [E ipsw dyld info].
Helpers written for this (all in `tools\`): `ca-mount.sh`, `ca-vt.py` (vtable slots -> symbols), `ca-meth.py` (ObjC method lists), `ca-slots.py` (GL stub table offsets), `ca-dis.py`, `ca-q*.sh`.

## 1. How the S9+ mechanism works

### 1.1 Injection [E]
* NOT launchd `EnvironmentVariables`. `inject.c` header: "launchd starts the jobs ... with DYLD_INSERT_LIBRARIES"; the real mechanism is in the kernel patch (`xnu/hakios.patch` ~line 720): sysctl `kern.exec_inject` = `"LIBRARY NAME NAME ...; LIBRARY NAME ..."`; `exec_extract_strings()` adds `DYLD_INSERT_LIBRARIES=<lib>[:<lib>]` to the environment of every exec whose file name matches one of the NAMEs. Comment: "launchd refuses to pass DYLD_ variables from a job description, which is why the kernel adds this one". Their `init` writes it from `etc/inject.conf` before launchd runs (init.c:1585-1619). build.sh INJECT list: `/redwood/lib/libhakios.dylib backboardd containermanagerd containermanagerd_system`, plus SpringBoard/mediaserverd/etc. libs.
* dyld must honour it: their AMFI stand-in (`kern_minimac.c`) answers AMFI policy syscall `0x5a` (check dyld policy; arg = {flags in, ptr flags out}) with output flags 0xff = allow everything (env vars, @paths, insert, interpose), and `0x60` (boot-arg state) with 0x86.
* Two dylibs: `libhakios.dylib` (inject.c, constructor `hakios_init`, acts only if `getprogname()=="backboardd"` or containermanagerd) which `dlopen`s `/redwood/lib/libhakios.gl.dylib` (eagl.c, its own constructor `install()`) when a GPU is present (service `HakiosGPU`/`HakiosMali`) or boot-arg `gl=1`.
* **Our kernel has neither piece** [E: grep of all `upstream\xnu\patches`]: no `exec_inject`; the AMFI stand-in is named "SWEET AMFI" and registers no `mpo_policy_syscall` (only the "Sandbox" policy has one, 0050/0064). So DYLD_INSERT_LIBRARIES into backboardd will not work yet [G on how dyld4 treats the failing amfi call; likely ignored for a platform binary]. Needs one of: (a) port exec_inject + AMFI 0x5a/0x60 (small, APSL), (b) add LC_LOAD_DYLIB to backboardd in the root image (our AMFI is permissive; no kernel change). Not done here.

### 1.2 What is patched: two different techniques [E]
**(a) EAGLContext (eagl.c `install()`, constructor of libhakios.gl).** ObjC runtime, by name: `class_getInstanceMethod(EAGLContext, sel)` then `method_setImplementation` (or `class_addMethod` if the method is inherited). No cache addresses. 8 methods in the code (header says "six things", then lists 6; the table has 8): `initWithAPI:properties:`, `API`, `sharegroup`, `setParameter:to:`, `getMacroContextPrivate`, `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:`, `attachImage:toCoreSurface:invertedRender:`, `dealloc`. The replaced `initWithAPI:` never calls Apple's init: it creates the Mesa/OSMesa context (`hakios_gl_create`) and keeps its state in a side struct found by `context_of(self)`. `+setCurrentContext:` is not replaced (see 3.4).
**(b) C++ vtables of CA::WindowServer (inject.c `window_server_fixes()`).** Binary: `replace_virtual(what, slot, expected, replacement)` writes into vtable slots (unslid addresses of 20F66 + slide; slide = `_dyld_get_shared_cache_range() - 0x180000000`), after `vm_protect(... VM_PROT_COPY)` for a private writable page, and only if the slot holds the expected function. Slots: AppleCLCDDisplay `is_ready, ignore_update_p, begin_update, finish_update` (diagnostics only), EAGLServer `renderer` (stop retrying a failing accelerated renderer), `render_update` (log), GLESContext `function_supported`, `function_uniform`, `compile_shader` (census / experiments with CA's "functions"/shaders). **None of these is needed to get GPU compositing**; the actual GPU switch is item (a). Also: `-[CAWindowServer _detectDisplays]` (ObjC, but identity checked by address) empties Metal's static `_deviceArray` so that `MTLCreateSystemDefaultDevice()` returns nil and CA falls back from Metal to GLES (only needed when a Metal device exists, boot-arg `metal=1`), and `-[BLControl waitForALSArrival]`, `+[MCMUserIdentityCache personasAreSupported]` (not related to GPU).

### 1.3 Which GL calls, through which table [E]
QuartzCore's `CA::OGL::GLESContext` does not call libGL stubs. In `EAGLServer::renderer()` it does `[[EAGLContext alloc] initWithAPI:3 properties:...]` (Metal first via `CAMetalContextCreate`/`new_metal_context`, GLES when the device is nil) and calls `-getMacroContextPrivate` (CGLMacro-style): the result is a **table of function pointers, entry(table[0], args...)**; offsets = the "slots" in `eagl_slots.txt` (414 slots, 0x10..0x2048, '*' = seen called by QuartzCore). The table is `priv + 0x20` where `priv = [EAGLContext _private]` (ivar at +8) of Darwin's own OpenGLES; Apple's gl* stubs do `x8=TSD[0xf0]; x0=[x8+0x20]; br [x8+0x20+slot]`. eagl.c builds its own table (generated by `mkeagl.py` from slots + Khronos gl.xml + exported Mesa functions: each entry counts and forwards to the Mesa `gl*`, ~20 hand-written `my_gl*` for shader rewriting etc.). Table[0] = pointer to eagl.c's `struct eagl`.
Extra on top of plain forwarding (all in eagl.c): `my_glShaderSource` (rewrites shader text: `sampler2DRect`, pixel coordinates, `gl_LastFragData` -> framebuffer fetch), BGRA as GL_RGBA with blue-first, draws made with no program (`plain_draw_begin`, CA's 90 "functions", about half have no GLES shader), `vblur` handling, EXT_shader_framebuffer_fetch passthrough, fences.

### 1.4 Surfaces [E]
* `-texImageIOSurface:...` (CA shows an IOSurface: layer contents, backing store): texture bound to `target` becomes the surface's content. `-attachImage:toCoreSurface:invertedRender:` (the texture bound to the target IS the surface; it becomes a framebuffer colour attachment: what is drawn is the screen or an offscreen layer).
* Zero copy when the driver can: `hakios_gl_surface_texture(gl, IOSurfaceGetID(surface))` -> `glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (void*)(uintptr_t)surface_id)` (gl_osmesa.c; Mesa patched so the surface ID is the EGLImage and the kernel driver makes a BO of the surface's memory: DRM shim requests 0x7c..0x7f). Otherwise copy: fill texture from `IOSurfaceGetBaseAddress` when `IOSurfaceGetSeed` changed, `glReadPixels` back into it at `glFlush`/`glFinish`. Only 4-bytes/element, single-plane surfaces are shared (`attachImage` requires `bytesPerElement==4 && planes==0`).
* Frame end: `frame_end()` waits for the GPU (or, with `HakiosWaitsForGPU` set on the IOMobileFramebuffer service, the display driver waits instead; boot-arg `glwait`).

### 1.5 Frame to the display (S9+ vs ours) [E for S9+ via gpu-plan.md/README, E for ours via patches]
S9+: MiniIOMobileFramebuffer (their IOMFB stand-in) waits for the surface's GPU job (`HakiosMaliDrawn()`, `maliSeq` in the IOSurface), then the CPU copies/scales into one of three panel-size buffers; `hakios.exynos_dpu.c` flips DECON IDMA base (or VGF0 DPP scaler, 1125x2436 -> 1440x2960). The GPU never writes the panel.
Ours: `AppleCLCD::swap(rec)` in `0046-sweet-iosurface-iomfb.patch` (later 0109/0124 for S8 DECON). The IOMFB swap record carries per layer: surface id (`rec+0x54+4*layer`), source rect (`rec+0x64+16*layer`), dest rect (`rec+0xc4+16*layer`), layer mask `rec+0x104`, swap id `rec+0x50`; the kernel does `sweet_surface_copy(id)` and a CPU `memcpy` of rows into the boot framebuffer (4 bytes/pixel, no format conversion, no scaling; XRGB8888/BGRA little-endian equals 'BGRA'). Log line: `SWEET FB: swap N layer L surface ID WxH fmt 0x.. bpr .. a sx,sy w x h b dx,dy w x h`.
**So the new renderer hands the framebuffer driver nothing new: the same IOSurface id (CA's display surface, 'BGRA', bpr = width*4, what CA hands to IOMFB swap), but with the GPU's pixels already in that surface's memory and visible to the CPU.** Requirements: (1) the SweetSurface memory must be mappable into the Mali address space (kernel selector like S9+ 0x7c) and write-combined/uncached or cache-maintained for the CPU read in `AppleCLCD::swap`; (2) GPU job completion before the swap (userland `glFinish` equivalent = wait for job number, or a fence field in SweetSurface the swap waits on, as S9+ `maliSeq`); (3) CA_FORCE_TIMER (boot-arg `ca_vbl=1`) because iPhone X CA uses CADisplayTimer without VBL.

## 2. Derivation for 20H392 (old S9+ 20F66 -> new)

Method: `ipsw dyld symaddr` on the cache (local symbols included), vtable slots read from the cache and resolved ([E] ca-vt.py), ObjC method lists ([E] ca-meth.py), disassembly with capstone ([E] ca-dis.py/ca-slots.py).

### 2.1 Vtables and slots (C++; the only build-specific part)
| item | 20F66 (S9+) | 20H392 (ours) | evidence |
|---|---|---|---|
| vtable CA::WindowServer::AppleCLCDDisplay | 0x1d5b44520 | **0x1d5b38788** | symbol `__ZTVN2CA12WindowServer16AppleCLCDDisplayE` |
| vtable CA::WindowServer::EAGLServer | 0x1d5b45c58 | **0x1d5b39ed0** | `__ZTVN2CA12WindowServer10EAGLServerE` |
| vtable CA::OGL::GLESContext | 0x1d5b3b540 | **0x1d5b2f6e8** | `__ZTVN2CA3OGL11GLESContextE` |
| AppleCLCD slot 0xb0 is_ready | fn 0x1882f0908 | slot 0xb0 = 0x1882f9318 `IOMFBDisplay::is_ready()` | slot read, name matches |
| slot 0x138 ignore_update_p | 0x1882e0c90 | slot 0x138 = 0x1882e9684 `IOMFBDisplay::ignore_update_p` | idem |
| begin_update | slot 0x170, 0x1882ddda4 | **slot 0x178** = 0x1882e6798 `IOMFBDisplay::begin_update` | **moved +8**: new virtual `IOMFBDisplay::work_interval()` at 0x170 |
| finish_update | slot 0x178, 0x1882c8568 | **slot 0x180** = 0x1882d0f88 `IOMFBDisplay::finish_update` | moved +8 |
| EAGLServer slot 0x1a0 renderer | 0x1882e2548 | 0x1882eaf3c `EAGLServer::renderer()` | same slot |
| EAGLServer slot 0x178 render_update | 0x18835770c | 0x1883601dc `EAGLServer::render_update(Update*, unsigned&)` | same slot |
| GLESContext slot 0x378 function_supported | 0x1884a9308 | 0x1884b2164 `GLContext::function_supported() const` | same slot |
| slot 0x388 function_uniform | 0x1884a9350 | 0x1884b21ac `GLContext::function_uniform(uint,ulong,ulong,float const*)` | same slot |
| slot 0x060 compile_shader | 0x1883fc808 | 0x1884053fc `Context::compile_shader(char const*)` | same slot |
(slot address = vtable + 0x10 + offset; old->new function deltas are about +0x89f4..+0x8ed0, not constant.)

### 2.2 Other addresses quoted by inject.c
| item | old | new | evidence |
|---|---|---|---|
| `-[CAWindowServer _detectDisplays]` | 0x1884da264 | 0x1884e3150 | symbol (ObjC: use by name) |
| `EAGLServer::EAGLServer(IOMFBDisplay*, CFString*)` | 0x1883d1544 | 0x1883da11c | symbol |
| `_CAMetalContextCreate` | 0x18837b718 | 0x18838421c | symbol |
| `CA::OGL::new_metal_context` | 0x18847efcc | 0x188487e28 | symbol |
| Metal `_deviceArray` (static NSMutableArray) | 0x1e1efe9a0 | 0x1e1d8a970 | symbol `__ZL12_deviceArray` (Metal) |
| `__MTLCreateSystemDefaultDevice_block_invoke` | 0x181a3442c | 0x181a37050 | symbol |
| `-[BLControl waitForALSArrival]` | 0x1af043734 | 0x1af0a672c | symbol (not GPU related) |
| `+[MCMUserIdentityCache personasAreSupported]` | 0x1c3ea6298 | 0x1c4c699f8 | symbol (ContainerManagerCommon) |
| **CA_SHADER_TABLE** (shader source per CA function) | 0x1d5b54790 | **NOT DERIVED** | old address in 20H392 lies inside a C++ exception table, so certainly different; only for the optional "sb3" experiment |
| `DISPLAY_FLAGS +0x9f2`, `+0x233`, `+0x220`, `update+0xf0` (struct offsets, diagnostics) | | **not verified** | unneeded |

### 2.3 EAGLContext (OpenGLES.framework) [E ca-meth.py]
All 8 selectors exist as methods of `EAGLContext` itself in 20H392, with the type encodings the S9+ code assumes:
`initWithAPI:properties:` 0x1d3f51270 `@32@0:8Q16@24`; `API` 0x1d3f51880 `Q16@0:8`; `sharegroup` 0x1d3f5188c; `setParameter:to:` 0x1d3f52064 `Q28@0:8I16^i20`; `getMacroContextPrivate` 0x1d3f5211c (`ldr x8,[x0,#8]; add x0,x8,#0x20; ret`); `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:` 0x1d3f51efc `B72@0:8^{__IOSurface=}16Q24Q32I40I44Q48Q56I64B68`; `attachImage:toCoreSurface:invertedRender:` 0x1d3f51c60 `B36@0:8Q16^{__IOSurface=}24B32`; `dealloc` 0x1d3f51714. Class list at 0x1d3f52db8 (25 instance methods), class methods at 0x1d3f52d98 (`setCurrentContext:`, `currentContext`). Every one of the six selectors CA calls has an `objc_msgSend$` stub in QuartzCore (attachImage, getMacroContextPrivate, initWithAPI:properties:, setCurrentContext:, setParameter:to:, texImageIOSurface...invert:) [E qc.sym], i.e. 16.7.16's QuartzCore still uses the same interface. No address is needed for any of these.

### 2.4 The GL dispatch table slots
`eagl_slots.txt` offsets are **identical** in 20H392: ca-slots.py disassembled the OpenGLES stub of every function in the file (457 names). 443 matched exactly (`x8=TSD[0xf0]`, `ldr xN,[x8,#slot+0x20]`, `br xN`; table = private+0x20 = what `getMacroContextPrivate` returns), 0 contradicted, 14 have a different stub shape (extra args, `blr`, x9/x10 temp) which the script could not parse; 7 of those checked by hand and all match (glBlitFramebuffer 0x17a0, glCreateShader 0x1298, glCreateProgram 0x12b0, glTexImage2D 0x970, glTexSubImage2D 0x9a0, glCopyTexSubImage2D 0x1c8, glTexImage3D 0xdb8; glCopyTexImage2D 0x1b8 also). The last 7 (glCompressedTexImage3D/2D, glCompressedTexSubImage3D/2D, glTexSubImage3D, glCopyTexSubImage3D, glCopyTexImage2D's sibling shapes) were not checked individually; the neighbouring slots all fit, so they are expected to match. `eagl_slots.txt` can be reused unchanged, and `mkeagl.py` as is. The table object is `priv + 0x20`, `priv` = `[self+8]`.

### 2.5 Not derived / caveats
* CA_SHADER_TABLE (above). Struct-field offsets used by the S9+ diagnostics.
* Whether 16.7.16's `EAGLServer::renderer()` still falls to GLES when `MTLCreateSystemDefaultDevice()==nil` [G, S9+ comment says it does for 16.5; strings/stubs for both paths exist: `new_metal_context`, `GLESContext::GLESContext(void*)` 0x18840960c].
* Whether the 20H392 `EAGLServer` constructor still calls `renderer()` by direct branch (irrelevant to us unless Metal is present).

## 3. Port (new files, `upstream\userland\hook\`)
* `hook_addrs_20H392.h` the table above with how each was found. `hook_inject.c` backboardd-only constructor: UUID check, `replace_virtual` for `EAGLServer::renderer`/`render_update` (log only), dlopen of the stage-2 GL library (`/usr/lib/libhook_gl.dylib`, absent now). `hook_gl.h` interface for stage 2 (create/make_current/surface_texture/wait). `hook_log.c` experiment, `README.md`.
* Deviation from the S9+ code: the two cache-build-dependent things (vtable slots, `_detectDisplays` identity check) are limited to what is needed; ObjC replacements use `class_replaceMethod` by name; no hard-coded slide: `_dyld_get_shared_cache_range - 0x180000000` as S9+ (cache UUID checked, so a different build disables C++ slots instead of corrupting memory).
* 3.4 Why leaving `+setCurrentContext:` alone works [E from disassembly of 0x1d3f517ec]: `x20 = TSD[0xf0]; x0 = self ? [self+8] : 0; if (x0 == x20) return YES`. With a replaced init the `_private` ivar is NULL and nothing ever sets TSD[0xf0], so it compares NULL==NULL and returns YES. The Mesa context was made current by our own `initWithAPI:` (OSMesaMakeCurrent on the calling thread); [G] CA renders on the thread that created the context.

## 4. Report

**High confidence (static evidence):** all 8 EAGLContext selectors and types exist; the dispatch table slots and layout are unchanged (443/457 verified by script, 7 by hand, 0 contradictions); the three vtable addresses and nine slot contents with their names; the +8 shift of begin/finish_update; every plain function address in 2.2 by symbol; the mechanism (name-based ObjC replacement + table via getMacroContextPrivate); how frames reach our display (same IOSurface id, `AppleCLCD::swap` CPU copy).

**Needs on-device verification:** (1) that DYLD_INSERT_LIBRARIES reaches backboardd (kernel work missing, see 1.1); (2) that EAGLServer::renderer() in 16.7.16 reaches `[EAGLContext initWithAPI:3]` on our system (no Metal device expected, since there is no AGX accelerator service: [G]); (3) the live table content (hook_log dumps `dladdr` of slots 0x58, 0x2008 ...); (4) CA's thread/context usage with the unreplaced `setCurrentContext:`; (5) the CA_SHADER_TABLE address (optional); (6) GPU/kernel side: IOSurface -> Mali VA mapping, cache maintenance, job wait before `AppleCLCD::swap`.

**Not derived:** CA_SHADER_TABLE; diagnostic struct offsets; anything GPU-side (stage 1/2 outputs).

**Minimal first experiment:** build `userland\hook\hook_log.c` (build line in its header, via the userland workflow, later), put it in the root image as a new file, get it inserted into backboardd by whichever of the two injection options is chosen, boot, read `/private/var/tmp/hook_log.txt` (and stderr/console). It changes nothing (all originals called). It verifies: dylib loaded in backboardd; whether/when `initWithAPI:` is called with API 3 (or 2) and whether the stock OpenGLES returns nil (the gap to fill); the six selectors CA really uses and with which arguments (IOSurface id, size, pixel format, target, plane, invert); `setCurrentContext:` call count and thread; and `getMacroContextPrivate` table pointers resolved with dladdr (confirms slot layout).
**First visible result:** no picture change at all; text in the log. The first visible *picture* change comes only in stage 2: with `libhook_gl.dylib` over Mesa softpipe (CPU) as a first backend (S9+ boot arg `gl=1` equivalent), CA's GLES renderer draws SpringBoard through our context; then with the S8Mali kernel driver and Panfrost, the same frames at GPU speed.
