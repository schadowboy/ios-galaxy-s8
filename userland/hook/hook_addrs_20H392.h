/*
 * hook_addrs_20H392.h: addresses inside the dyld shared cache of iOS 16.7.16 (20H392), iPhone X
 * (dyld_shared_cache_arm64 from SystemOS cryptex 087-86593-047.dmg).
 *
 * Derived for the Galaxy S8 port from the structure documented by the HakiOS authors'
 * userland/src/inject.c (MIT, Copyright (c) 2025-2026 the HakiOS authors), whose 20F66 (16.5)
 * addresses are listed in the OLD column of each comment.
 *
 * All values are UNSLID (cache as built, base 0x180000000). The runtime address is value + slide,
 * slide = (uintptr_t)_dyld_get_shared_cache_range() - HOOK_CACHE_BASE.
 *
 * How each value was found (tools: C:\github\ios-s8\tools\ca-*.sh|py, static, phone untouched):
 *   S   = "ipsw dyld symaddr --image <Image>": the cache's local symbol file names the symbol
 *   V   = vtable slot read from the cache (ca-vt.py) and resolved by symbol: the slot content is
 *         the function named in the comment (evidence = same name as the S9+ role of that slot)
 *   M   = ObjC method list of the class read from the cache (ca-meth.py)
 * Anything the code touches by absolute address is verified at run time against EXPECT_* values
 * (the slot must still hold what is listed here) and left alone otherwise.
 *
 * Machine-independent facts (selectors, vtable slot offsets) are separate from the address
 * facts so that a different cache build only needs the address lines changed.
 */
#ifndef HOOK_ADDRS_20H392_H
#define HOOK_ADDRS_20H392_H

#define HOOK_CACHE_BASE         0x180000000UL

/* Cache identity: the main file's UUID (ipsw dyld info). The hook refuses unknown caches. */
#define HOOK_CACHE_UUID \
	{ 0x8F, 0x97, 0x04, 0x52, 0xC9, 0xE3, 0x3E, 0x88, 0xB1, 0x14, 0xFE, 0xB1, 0x86, 0x4B, 0xC9, 0xE0 }

/* ---- C++ vtables of CA::WindowServer / CA::OGL (QuartzCore) --------------------------------------
 * A vtable's slot N (byte offset `off` after the two-word header) is at VSLOT(vt, off) = vt + 0x10 + off.
 *                                                  OLD 20F66        NEW 20H392   how
 */
#define VT_APPLE_CLCD_DISPLAY   0x1d5b38788UL   /* 0x1d5b44520  vtable for CA::WindowServer::AppleCLCDDisplay  S */
#define VT_EAGL_SERVER          0x1d5b39ed0UL   /* 0x1d5b45c58  vtable for CA::WindowServer::EAGLServer         S */
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* 0x1d5b3b540  vtable for CA::OGL::GLESContext                 S */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))

/* slot offsets and the function each holds in 20H392 (EXPECT = value found in the slot; V)
 * NOTE: begin_update/finish_update moved by +8 (a new virtual, IOMFBDisplay::work_interval, sits at 0x170).
 *       slot          OLD off  NEW off  OLD fn        NEW fn (EXPECT)    symbol */
#define OFF_IS_READY            0x0b0           /* 0xb0   */
#define EXPECT_IS_READY         0x1882f9318UL   /* 0x1882f0908  CA::WindowServer::IOMFBDisplay::is_ready() */
#define OFF_IGNORE_UPDATE_P     0x138
#define EXPECT_IGNORE_UPDATE_P  0x1882e9684UL   /* 0x1882e0c90  IOMFBDisplay::ignore_update_p(Render::Update const*) */
#define OFF_BEGIN_UPDATE        0x178           /* OLD 0x170 */
#define EXPECT_BEGIN_UPDATE     0x1882e6798UL   /* 0x1882ddda4  IOMFBDisplay::begin_update(Render::Update*) */
#define OFF_FINISH_UPDATE       0x180           /* OLD 0x178 */
#define EXPECT_FINISH_UPDATE    0x1882d0f88UL   /* 0x1882c8568  IOMFBDisplay::finish_update(Render::Update*, unsigned) */
#define OFF_RENDERER            0x1a0           /* EAGLServer */
#define EXPECT_RENDERER         0x1882eaf3cUL   /* 0x1882e2548  EAGLServer::renderer() */
#define OFF_RENDER_UPDATE       0x178           /* EAGLServer */
#define EXPECT_RENDER_UPDATE    0x1883601dcUL   /* 0x18835770c  EAGLServer::render_update(Render::Update*, unsigned&) */
#define OFF_FUNCTION_SUPPORTED  0x378           /* GLESContext (inherited: CA::OGL::GLContext) */
#define EXPECT_FUNCTION_SUPPORTED 0x1884b2164UL /* 0x1884a9308  GLContext::function_supported() const */
#define OFF_FUNCTION_UNIFORM    0x388
#define EXPECT_FUNCTION_UNIFORM 0x1884b21acUL   /* 0x1884a9350  GLContext::function_uniform(uint, ulong, ulong, float const*) */
#define OFF_COMPILE_SHADER      0x060
#define EXPECT_COMPILE_SHADER   0x1884053fcUL   /* 0x1883fc808  CA::OGL::Context::compile_shader(char const*) */

/* ---- plain functions the S9+ source quotes (informational; all reached by NAME in this port) ------- */
#define ADDR_EAGLSERVER_CTOR    0x1883da11cUL   /* 0x1883d1544  EAGLServer::EAGLServer(IOMFBDisplay*, __CFString const*)     S */
#define ADDR_EAGLSERVER_RENDERER 0x1882eaf3cUL  /* 0x1882e2548  (direct call from the ctor, bypasses the vtable)             S */
#define ADDR_CAMETALCONTEXTCREATE 0x18838421cUL /* 0x18837b718  _CAMetalContextCreate                                        S */
#define ADDR_NEW_METAL_CONTEXT  0x188487e28UL   /* 0x18847efcc  CA::OGL::new_metal_context(...)                              S */
#define ADDR_DETECT_DISPLAYS    0x1884e3150UL   /* 0x1884da264  -[CAWindowServer _detectDisplays]                            S (ObjC: replace by name) */
#define ADDR_MTL_DEVICE_ARRAY   0x1e1d8a970UL   /* 0x1e1efe9a0  Metal  static NSMutableArray *_deviceArray (__ZL12_deviceArray) S */
#define ADDR_MTL_DEFAULT_BLOCK  0x181a37050UL   /* 0x181a3442c  ___MTLCreateSystemDefaultDevice_block_invoke                S */
#define ADDR_BLCONTROL_WAITALS  0x1af0a672cUL   /* 0x1af043734  -[BLControl waitForALSArrival] (CoreBrightness)              S (ObjC: by name) */
#define ADDR_MCM_PERSONAS       0x1c4c699f8UL   /* 0x1c3ea6298  +[MCMUserIdentityCache personasAreSupported]                 S (ObjC: by name) */
/* NOT derived: CA_SHADER_TABLE (old 0x1d5b54790, per-function shader source table; only for the
 * optional "sb3" experiment). The first 8 bytes at that address in 20H392 are inside a C++ exception
 * table, so the old address is certainly wrong here; finding the new one needs a disassembly of
 * CA::OGL::GLContext::create_shader. Not needed for the first GPU frame. */

/* ---- EAGLContext (OpenGLES.framework; reached by selector, addresses are for the record) ---------
 * ObjC class_ro method list at 0x1d3f52db8 (instance) / 0x1d3f52d98 (class); all eight selectors the
 * S9+ port replaces exist, with these implementations and type encodings (M). */
#define EAGL_IMP_initWithAPI_properties     0x1d3f51270UL   /* @32@0:8Q16@24 */
#define EAGL_IMP_API                        0x1d3f51880UL   /* Q16@0:8 */
#define EAGL_IMP_sharegroup                 0x1d3f5188cUL   /* @16@0:8 */
#define EAGL_IMP_setParameter_to            0x1d3f52064UL   /* Q28@0:8I16^i20 */
#define EAGL_IMP_getMacroContextPrivate     0x1d3f5211cUL   /* ^{EAGLMacroContext=}16@0:8 ; body: ldr x8,[x0,#8]; add x0,x8,#0x20 */
#define EAGL_IMP_texImageIOSurface          0x1d3f51efcUL   /* B72@0:8^{__IOSurface=}16Q24Q32I40I44Q48Q56I64B68 (the invert: variant) */
#define EAGL_IMP_attachImage                0x1d3f51c60UL   /* B36@0:8Q16^{__IOSurface=}24B32 */
#define EAGL_IMP_dealloc                    0x1d3f51714UL   /* v16@0:8 */
#define EAGL_IMP_setCurrentContext          0x1d3f517ecUL   /* +[EAGLContext setCurrentContext:] B24@0:8@16 (NOT replaced; see README) */

/* ---- the GL dispatch table behind -getMacroContextPrivate -----------------------------------------
 * Layout in 20H392 is IDENTICAL to 20F66 (src/eagl_slots.txt of the S9+ port): 443 of the 457 listed
 * functions were checked by script against the stubs in OpenGLES.framework (the stub does
 *   x8 = TSD[0xf0] (tpidrro_el0); x0 = [x8+0x20]; fn = [x8 + 0x20 + slot]; br fn
 * i.e. the table is at private+0x20, table[0] is the context argument, entry at table+slot), and 7 of
 * the remaining 14 (blr/x9 shaped stubs) by hand (glBlitFramebuffer 0x17a0, glCreateShader 0x1298,
 * glCreateProgram 0x12b0, glTexImage2D 0x970, glTexSubImage2D 0x9a0, glCopyTexSubImage2D 0x1c8,
 * glTexImage3D 0xdb8). No mismatch was found. So eagl_slots.txt is used unchanged.
 */
#define HOOK_GLTABLE_SLOTS_FILE "eagl_slots.txt"   /* copy from ref/s9plus/core/userland/src, unchanged */
#define HOOK_GLTABLE_SIZE       0x2050              /* highest slot 0x2048 + 8 */

#endif
