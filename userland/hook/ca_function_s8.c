/*
 * ca_function_s8.c: CoreAnimation's current effect "function" and the numbers it is given (iOS 16.7.16, 20H392).
 *
 * Port of the minimum of the S9+ port's userland/src/inject.c my_function_uniform() (MIT, Copyright (c) 2025-2026 the
 * HakiOS authors; read-only reference): CA::OGL::GLContext::function_uniform(uint index, ulong components, ulong count,
 * float const *values) is called by CA with the numbers of the function it is about to draw (a colour matrix, a blur's
 * sample places); the function number is a byte at (*(ca_context + 0x10)) + 0x10. LOGGING / CENSUS ONLY: the original is
 * always called, nothing else changes.
 *
 * Safety, in the order it is checked (all of it in s8gl_ca_install(), nothing is written if one step fails):
 *   1. the process links a shared cache whose UUID is 20H392's (HOOK_CACHE_UUID), else nothing;
 *   2. the slot of GLESContext::function_supported (a second slot of the same vtable) holds its EXPECT_ value, which
 *      shows that the vtable address VT_GLES_CONTEXT + slide is the vtable and the slide is right;
 *   3. the slot function_uniform (OFF_FUNCTION_UNIFORM) holds EXPECT_FUNCTION_UNIFORM + slide;
 *   only then the page is made privately writable and the slot replaced.
 * The structure offsets (+0x10 -> +0x10) are the S9+ port's for 20F66 and are NOT verified for 20H392: the pointers are
 * read with vm_read_overwrite() (no fault possible) for the first calls and for ever when one of those reads fails, the
 * result is accepted only when it is below CA_FUNCTIONS, and the first calls log the chain so that the log shows whether
 * the offsets hold ("s8gl: ca: function_uniform call N: context ... -> ... -> byte ...").
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ca_function_s8.h"

void s8gl_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
#define say s8gl_say

static const char ca_names[] =
	"clr img ida cim cid mua m1a lrp lrc lra add spl s1a cma bds bco bim c33 c3b c4b c4p c3a c3x c4x bia grl ds4 ds6 "
	"ds8 ups db2 db4 nb7 n11 n15 n19 n23 n27 vrd vrb sb3 axl rad rac con vcm vdk vlt map mpc crv hue clm hom opa met "
	"srl ega sdr alt p3c edr hrm lut ovr ovl lub aup alm stl pc0 pc1 prc psl ps0 ps1 psm psr pcr pbl peo bib asg a75 "
	"a50 atc ipc apl 3dl cst ";

const char *
s8gl_ca_name(unsigned function)
{
	static char names[CA_FUNCTIONS][4];
	static int made;

	if (function >= CA_FUNCTIONS) {
		return "?";
	}
	if (!made) {
		for (unsigned i = 0; i < CA_FUNCTIONS; i++) {
			memcpy(names[i], ca_names + 4 * i, 3);
			names[i][3] = 0;
		}
		made = 1;
	}
	return names[function];
}

static float function_values[CA_FUNCTIONS][20];
static uint8_t function_given[CA_FUNCTIONS];
static volatile unsigned current_function = CA_FUNCTION_UNKNOWN;
static int hooked;

int
s8gl_ca_hooked(void)
{
	return hooked;
}

unsigned
s8gl_ca_function(void)
{
	return current_function;
}

unsigned
s8gl_ca_values(unsigned function, float out[20])
{
	if (function >= CA_FUNCTIONS || function_given[function] == 0) {
		return 0;
	}
	memcpy(out, function_values[function], sizeof(function_values[function]));
	return function_given[function];
}

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <stdbool.h>
#include <uuid/uuid.h>

#include "hook_addrs_20H392.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);

static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static unsigned calls;
static int safe_reads = 1;                      /* until 200 calls have all been read without a refused pointer */

static int
safe_read(uintptr_t address, void *out, size_t size)
{
	vm_size_t done = 0;

	if (address < 0x1000 || (address & 7) != 0) {
		return 0;
	}
	return vm_read_overwrite(mach_task_self(), (vm_address_t)address, size, (vm_address_t)out, &done) == KERN_SUCCESS &&
	    done == size;
}

/* The function number of a context as the S9+ port reads it: *(*(uint8_t **)(context + 0x10) + 0x10). CA_FUNCTION_UNKNOWN: refused. */
static unsigned
function_of(void *context, uintptr_t *first, uintptr_t *second, int *byte_out)
{
	uintptr_t a = 0, b = 0;
	uint8_t byte = 0xff;

	*first = *second = 0;
	*byte_out = -1;
	if (safe_reads) {
		if (!safe_read((uintptr_t)context + 0x10, &a, sizeof(a))) {
			return CA_FUNCTION_UNKNOWN;
		}
		*first = a;
		if (!safe_read(a + 0x10, &byte, 1)) {
			return CA_FUNCTION_UNKNOWN;
		}
	} else {
		a = *(uintptr_t *)((char *)context + 0x10);
		*first = a;
		byte = *(uint8_t *)(a + 0x10);
	}
	b = a + 0x10;
	*second = b;
	*byte_out = byte;
	return byte < CA_FUNCTIONS ? byte : CA_FUNCTION_UNKNOWN;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	static uint8_t said[CA_FUNCTIONS], distinct[CA_FUNCTIONS];
	uintptr_t first, second;
	int byte;
	unsigned function = function_of(context, &first, &second, &byte);
	unsigned n = calls++;

	if (n < 12 || (function == CA_FUNCTION_UNKNOWN && n < 64)) {
		say("s8gl: ca: function_uniform call %u: context %p -> %#lx -> byte %d => function %u (index %u, %zu of %zu)\n", n + 1, context,
		    (unsigned long)first, byte, function, index, count, components);
	}
	if (n == 200 && safe_reads && function != CA_FUNCTION_UNKNOWN) {
		safe_reads = 0;                 /* the offsets hold: no more system calls for the reads */
		say("s8gl: ca: the structure offsets held for 200 calls; plain reads from now on\n");
	}
	current_function = function;
	if (function != CA_FUNCTION_UNKNOWN && values != NULL) {
		size_t total = components * count;

		if (index == 0 && total <= 20) {
			int changed = memcmp(function_values[function], values, total * sizeof(float)) != 0 ||
			    function_given[function] != total;

			if (!said[function] || (changed && distinct[function] < 3)) {
				char text[400];
				size_t used = 0;

				said[function]++;
				distinct[function] += changed;
				for (size_t i = 0; i < total && used < sizeof(text) - 16; i++) {
					used += (size_t)snprintf(text + used, sizeof(text) - used, " %.6g", (double)values[i]);
				}
				say("s8gl: ca: function %u (\"%s\") is given %zu of %zu numbers:%s\n", function, s8gl_ca_name(function), count,
				    components, total != 0 ? text : " none");
			}
			memcpy(function_values[function], values, total * sizeof(float));
			function_given[function] = (uint8_t)total;
		} else if (!said[function]) {
			said[function] = 1;
			say("s8gl: ca: function %u (\"%s\") is given %zu of %zu numbers at index %u: not kept\n", function,
			    s8gl_ca_name(function), count, components, index);
		}
	}
	orig_function_uniform(context, index, components, count, values);
}

void
s8gl_ca_install(void)
{
	static const uint8_t expected[16] = HOOK_CACHE_UUID;
	uuid_t uuid;
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	uintptr_t slide, *entry, *other;
	vm_address_t page;
	kern_return_t kr;

	if (cache == NULL) {
		say("s8gl: ca: no shared cache; function_uniform NOT hooked\n");
		return;
	}
	if (!_dyld_get_shared_cache_uuid(uuid) || memcmp(uuid, expected, 16) != 0) {
		say("s8gl: ca: the shared cache is not 20H392's (UUID mismatch); function_uniform NOT hooked, no function numbers\n");
		return;
	}
	slide = (uintptr_t)cache - HOOK_CACHE_BASE;
	other = (uintptr_t *)(VSLOT(VT_GLES_CONTEXT, OFF_FUNCTION_SUPPORTED) + slide);
	entry = (uintptr_t *)(VSLOT(VT_GLES_CONTEXT, OFF_FUNCTION_UNIFORM) + slide);
	if (*other != EXPECT_FUNCTION_SUPPORTED + slide) {
		say("s8gl: ca: vtable check: the function_supported slot holds %#lx, expected %#lx; function_uniform NOT hooked\n",
		    (unsigned long)(*other - slide), (unsigned long)EXPECT_FUNCTION_SUPPORTED);
		return;
	}
	if (*entry != EXPECT_FUNCTION_UNIFORM + slide) {
		say("s8gl: ca: the function_uniform slot holds %#lx, expected %#lx; NOT hooked\n", (unsigned long)(*entry - slide),
		    (unsigned long)EXPECT_FUNCTION_UNIFORM);
		return;
	}
	page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("s8gl: ca: cannot make the vtable page writable (%d); function_uniform NOT hooked\n", kr);
		return;
	}
	orig_function_uniform = (void *)*entry;
	*entry = (uintptr_t)my_function_uniform;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	hooked = 1;
	say("s8gl: ca: GLContext::function_uniform hooked (slide %#lx, slot %#lx verified, original %#lx)\n", (unsigned long)slide,
	    (unsigned long)(VSLOT(VT_GLES_CONTEXT, OFF_FUNCTION_UNIFORM)), (unsigned long)EXPECT_FUNCTION_UNIFORM);
}
#else
void
s8gl_ca_install(void)
{
	say("s8gl: ca: not an Apple build; function_uniform NOT hooked\n");
}
#endif
