/*
 * hook_inject.c: code of ours inside backboardd (iOS 16.7.16, 20H392, Galaxy S8 port).
 *
 * Port of the structure of the S9+ port's userland/src/inject.c (MIT, Copyright (c) 2025-2026 the
 * HakiOS authors; read-only reference, nothing executed). Differences:
 *   - addresses are 20H392's (hook_addrs_20H392.h), the cache is identified by UUID first, and
 *     every slot is checked to hold the expected function before it is touched;
 *   - ObjC methods are replaced BY NAME (class_replaceMethod), never by address; the only places
 *     that depend on the cache build are C++ vtable slots (virtual functions have no ObjC name);
 *   - the diagnostic hooks are the minimum needed to see the GPU road being taken
 *     (EAGLServer::renderer and ::render_update); the touch/ALS/containermanagerd parts of the
 *     S9+ file are not ported (we have our own input path).
 *
 * What it does, in order, once, from a constructor:
 *   1. returns unless the process is backboardd and the shared cache is the known build;
 *   2. hooks the two EAGLServer virtuals (log only, call through);
 *   3. if HOOK_GL_LIB exists and the GPU is there (boot-arg gl=1 forces it, gl=0 forbids it),
 *      dlopen()s it. THAT library (stage 2, eagl.c equivalent over Mesa) replaces the EAGLContext
 *      methods in its own constructor, as libhakios.gl.dylib does. Without it nothing changes and
 *      CoreAnimation keeps drawing with its software renderer.
 *
 * Insertion: DYLD_INSERT_LIBRARIES, see README.md (needs the kernel's AMFI stand-in to allow it).
 *
 * Build (later, by the userland workflow; not in CI now):
 *   xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 -dynamiclib \
 *     -install_name /usr/lib/libhook_inject.dylib -framework CoreFoundation -lobjc \
 *     -o hook_inject.dylib hook_inject.c && ldid -S hook_inject.dylib
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "hook_addrs_20H392.h"
#include "hook_gl.h"

#define HOOK_GL_LIB "/usr/lib/libhook_gl.dylib"      /* stage 2 output; absent now */

extern const void *_dyld_get_shared_cache_range(size_t *length);
extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);

static uintptr_t slide;
static int log_fd = -1;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	if (log_fd >= 0) {
		write(log_fd, line, (size_t)n);
	}
}

/*
 * Replace one entry of a virtual table (unslid address `slot`) that must now hold `expected`.
 * The table is in __DATA_CONST, shared by every process: get a private writable copy of its page.
 * Returns the original function or NULL (left alone).
 */
static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr;
	void *original;

	if (*entry != expected + slide) {
		say("hook: %s: found %#lx, expected %#lx; left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		say("hook: %s: cannot make the table writable (%d)\n", what, kr);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

/* ---- CA::WindowServer::EAGLServer ---------------------------------------------------------------- */

static void *(*orig_renderer)(void *server);
static uintptr_t (*orig_render_update)(void *server, void *update, unsigned *flags);
static unsigned calls_render;

/* Which renderer does the server end up with: non-NULL = Metal or OpenGL ES, NULL = software. */
static void *
my_renderer(void *server)
{
	static int said;
	void *renderer = orig_renderer(server);

	if (!said || (renderer == NULL) != (said == 2)) {
		said = renderer == NULL ? 2 : 1;
		say("hook: EAGLServer::renderer() -> %p (%s)\n", renderer, renderer ? "accelerated" : "software renderer");
	}
	return renderer;
}

static uintptr_t
my_render_update(void *server, void *update, unsigned *flags)
{
	uintptr_t r = orig_render_update(server, update, flags);

	if (calls_render++ < 6) {
		say("hook: render_update #%u -> %#lx (flags %#x)\n", calls_render, r, flags ? *flags : 0);
	}
	return r;
}

static int
cache_is_known(void)
{
	static const uint8_t expected[16] = HOOK_CACHE_UUID;
	uuid_t uuid;

	if (!_dyld_get_shared_cache_uuid(uuid)) {
		return 0;
	}
	return memcmp(uuid, expected, 16) == 0;
}

static int
bootarg_value(const char *name, int fallback)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	size_t n = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		for (const char *a = args; (a = strstr(a, name)) != NULL; a += n) {
			if ((a == args || a[-1] == ' ') && a[n] == '=') {
				return (int)strtol(a + n + 1, NULL, 10);
			}
		}
	}
	return fallback;
}

__attribute__((constructor))
static void
hook_init(void)
{
	const char *name = getprogname();
	const void *cache;
	size_t length = 0;
	int gl;

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	log_fd = open("/private/var/tmp/hook_inject.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
	say("hook: loaded in backboardd (pid %d)\n", getpid());
	cache = _dyld_get_shared_cache_range(&length);
	if (cache == NULL) {
		say("hook: no shared cache; nothing done\n");
		return;
	}
	slide = (uintptr_t)cache - HOOK_CACHE_BASE;
	if (!cache_is_known()) {
		say("hook: the shared cache is not 20H392's; C++ slots left alone (ObjC replacements by name still apply)\n");
	} else {
		orig_renderer = replace_virtual("renderer", VSLOT(VT_EAGL_SERVER, OFF_RENDERER), EXPECT_RENDERER,
		    my_renderer);
		orig_render_update = replace_virtual("render_update", VSLOT(VT_EAGL_SERVER, OFF_RENDER_UPDATE),
		    EXPECT_RENDER_UPDATE, my_render_update);
		say("hook: window server hooks %s\n", orig_renderer && orig_render_update ? "installed" : "INCOMPLETE");
	}
	gl = bootarg_value("gl", -1);
	if (gl == 0) {
		say("hook: gl=0: CoreAnimation keeps its software renderer\n");
		return;
	}
	if (access(HOOK_GL_LIB, R_OK) != 0) {
		say("hook: %s not installed (stage 2); CoreAnimation keeps its software renderer\n", HOOK_GL_LIB);
		return;
	}
	if (dlopen(HOOK_GL_LIB, RTLD_NOW) == NULL) {
		say("hook: %s: %s\n", HOOK_GL_LIB, dlerror());
	} else {
		say("hook: %s loaded; it replaces the EAGLContext methods\n", HOOK_GL_LIB);
	}
}
