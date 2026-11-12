/*
 * hook_gl.h: what the EAGLContext replacement (stage 2, the equivalent of the S9+ port's
 * libhakios.gl.dylib / src/eagl.c) asks of the OpenGL ES library behind it, besides the gl*
 * functions themselves. Same four calls as the S9+ port's hakios.gl.h (MIT, Copyright (c)
 * 2025-2026 the HakiOS authors); the implementation for us is a gl_osmesa.c equivalent over a
 * Mesa 25.0.7 OSMesa(ES profile)+Panfrost build (userland/mesa/, analysis/mesa-g71.md).
 *
 * TODO (stage 2, depends on the Mesa build and on the kernel S8Mali user client):
 *   - eagl.c equivalent: the table the replaced -getMacroContextPrivate returns. It is an array of
 *     function pointers, HOOK_GLTABLE_SIZE bytes, called as entry(table[0], args...) at the slot
 *     offsets of eagl_slots.txt (unchanged for 20H392, see hook_addrs_20H392.h). It is generated
 *     from that file, the Khronos gl.xml and the list of gl* functions the Mesa library exports
 *     (mkeagl.py of the S9+ port does exactly that; MIT, can be reused as is).
 *   - the surface/texture methods (-texImageIOSurface..., -attachImage...) which call
 *     hook_gl_surface_texture() so that a texture IS an IOSurface's memory (kernel selector that
 *     maps the SweetSurface into the GPU address space).
 */
#ifndef HOOK_GL_H
#define HOOK_GL_H

#include <stdint.h>

struct hook_gl;

struct hook_gl *hook_gl_create(int version);          /* a context of OpenGL ES 2 or 3; NULL: none to be had */
int hook_gl_make_current(struct hook_gl *gl);         /* the calling thread's (NULL: none); 0 on failure */
void hook_gl_destroy(struct hook_gl *gl);
const char *hook_gl_library(void);                    /* for the log */

/*
 * The texture bound to GL_TEXTURE_2D made of the IOSurface's own memory (zero copy): drawing into
 * the texture writes the surface, the surface's contents are what the texture shows. For Panfrost:
 * glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (void *)(uintptr_t)surface_id) with a Mesa patch that
 * asks the kernel driver for a buffer object of the surface's memory. 0: not done; the caller
 * then fills the texture from the surface and reads it back (copy path).
 */
int hook_gl_surface_texture(struct hook_gl *gl, uint32_t surface_id);

/* Block until the GPU has finished everything drawn into the surface so far, so that the kernel's
 * framebuffer driver (AppleCLCD::swap copy) reads final pixels. 0 on failure. */
int hook_gl_surface_wait(struct hook_gl *gl, uint32_t surface_id);

#endif
