/*
 * ca_function_s8.h: what libs8gl knows of CoreAnimation's "functions" (ca_function_s8.c).
 * CA's GLES renderer (CA::OGL::GLESContext) draws about half of its 90 effect functions (masking, colour
 * matrices, blurs) with no GL program at all; which function a program-less draw is, and the numbers CA gave it
 * (GLContext::function_uniform), are only known to CA, hence the hook.
 */
#ifndef CA_FUNCTION_S8_H
#define CA_FUNCTION_S8_H

#include <stddef.h>
#include <stdint.h>

#define CA_FUNCTIONS 90
#define CA_FUNCTION_UNKNOWN 255u

/* Installs the hook of GLContext::function_uniform if (and only if) every address is verified; logs why not otherwise. */
void s8gl_ca_install(void);
/* 1 once the hook is in place. */
int s8gl_ca_hooked(void);
/* CA's current function number (0..89), or CA_FUNCTION_UNKNOWN (no hook, no call yet, or the read was refused). */
unsigned s8gl_ca_function(void);
/* Three-letter name of a function ("mua", ...), "?" when out of range. */
const char *s8gl_ca_name(unsigned function);
/* The numbers CA last gave that function: how many (floats), 0 = none given; values copied to `out` (up to 20). */
unsigned s8gl_ca_values(unsigned function, float out[20]);

#endif
