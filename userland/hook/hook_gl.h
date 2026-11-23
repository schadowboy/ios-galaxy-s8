/*
 * hook_gl.h: what the EAGLContext replacement (eagl_s8.c, the equivalent of the S9+ port's
 * libhakios.gl.dylib / src/eagl.c) asks of the OpenGL ES library behind it, besides the gl*
 * functions themselves. The first four calls and the two after them are those of the S9+ port's
 * hakios.gl.h (MIT, Copyright (c) 2025-2026 the HakiOS authors; full text in
 * userland/s8mali/s8mali_drm.h); hook_gl_proc() and hook_gl_error() are ours (the library is
 * dlopen()ed at run time, so its gl* functions are found by name instead of linked).
 * The implementation is gl_osmesa_s8.c over a Mesa 25.0.7 OSMesa(ES profile)+Panfrost/softpipe
 * build (userland/mesa/, analysis/mesa-g71.md).
 *
 * The table the replaced -getMacroContextPrivate returns is an array of function pointers,
 * HOOK_GLTABLE_SIZE bytes at most, called as entry(table[0], args...) at the slot offsets of
 * eagl_slots.txt (unchanged for 20H392, see hook_addrs_20H392.h). It is generated from that file
 * and the Khronos gl.xml by mkeagl_s8.py into eagl_gen_s8.h (committed).
 */
#ifndef HOOK_GL_H
#define HOOK_GL_H

#include <stdint.h>

struct hook_gl;

struct hook_gl *hook_gl_create(int version);          /* a context of OpenGL ES 2 or 3; NULL: none to be had (see hook_gl_error) */
int hook_gl_make_current(struct hook_gl *gl);         /* the calling thread's (NULL: none); 0 on failure */
void hook_gl_destroy(struct hook_gl *gl);
const char *hook_gl_library(void);                    /* for the log */
const char *hook_gl_error(void);                      /* why the last hook_gl_create() / library load failed ("" if it did not) */

/* The address of a gl* function of the library (OSMesaGetProcAddress, then dlsym); NULL: it has none of that name.
 * Valid once hook_gl_create() has loaded the library; the functions work on the calling thread's current context. */
void *hook_gl_proc(const char *name);

/*
 * The texture bound to GL_TEXTURE_2D made of the IOSurface's own memory (zero copy): drawing into
 * the texture writes the surface, the surface's contents are what the texture shows. For Panfrost:
 * glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (void *)(uintptr_t)surface_id) with a Mesa patch that
 * asks the kernel driver for a buffer object of the surface's memory (selector 0x7e, see
 * s8mali_drm.h). 0: not done; the caller then fills the texture from the surface and reads it back
 * (copy path). NOT WORKING YET on purpose: eagl_s8.c asks only with the boot-arg/environment glshare=1,
 * and the library answers 0 unless it is Panfrost AND has glEGLImageTargetTexture2DOES (it then still
 * fails with a GL error until the Mesa patch and the kernel selector exist).
 */
int hook_gl_surface_texture(struct hook_gl *gl, uint32_t surface_id);

/* Block until the GPU has finished everything drawn so far (glFinish and the last job number of
 * the S8Mali shim), so that the kernel's framebuffer driver (AppleCLCD::swap copy) reads final
 * pixels. 0 on failure. */
int hook_gl_surface_wait(struct hook_gl *gl, uint32_t surface_id);

#endif
