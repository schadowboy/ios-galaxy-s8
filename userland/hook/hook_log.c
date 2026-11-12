/*
 * hook_log.c: FIRST EXPERIMENT. A dylib loaded into backboardd (through an LC_LOAD_DYLIB command added
 * to the binary by tools/macho_add_dylib.py, or DYLD_INSERT_LIBRARIES) that only LOGS what
 * CoreAnimation asks of EAGLContext. It changes no behaviour: every wrapper calls the original
 * implementation through the saved IMP and returns its result.
 *
 * It answers, on the first boot, the questions the GPU port depends on:
 *   - is the dylib loaded in backboardd at all ("hook_log: loaded: backboardd pid N cache UUID ...")
 *   - does CA::WindowServer::EAGLServer::renderer() take the OpenGL ES road ("initWithAPI:" with
 *     API 3 = kEAGLRenderingAPIOpenGLES3), and does the stock OpenGLES give a context (nil = no GPU)
 *   - which of the methods the S9+ port replaces are really called, with which arguments
 *     (IOSurface ids, sizes, pixel formats, targets that a Mesa/Panfrost backend must take)
 *   - what -getMacroContextPrivate returns and where the table entries point (dladdr)
 *
 * Original work for the S8 port, MIT License. Method list and type encodings come from the S9+
 * port's userland/src/eagl.c (MIT, Copyright (c) 2025-2026 the HakiOS authors) and were checked
 * against the 20H392 cache (hook_addrs_20H392.h).
 *
 * Safety: the constructor returns immediately unless the process is backboardd; everything is
 * guarded (NULL checks, no abort/assert, no private API except dlsym'ed optional symbols); a log
 * file that cannot be opened is simply skipped. Rate limit per selector: the first 200 calls are
 * written, then every 1000th. A summary line is written every 10 seconds.
 * No ARC, no Objective-C syntax: plain C against the ObjC runtime.
 *
 * Output: /private/var/tmp/hook_log.txt (O_APPEND, one write per line) and stderr.
 *
 * Build: userland/hook/build-hook.sh (xcrun --sdk iphoneos clang -arch arm64 -dynamiclib
 *   -install_name /usr/lib/libhook_log.dylib -framework Foundation, then ldid -S).
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HOOK_LOG_FIRST 200
#define HOOK_LOG_EVERY 1000
#define HOOK_LOG_FILE "/private/var/tmp/hook_log.txt"

static int log_fd = -1;

static void
say(const char *format, ...)
{
	char line[640];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
		line[n - 1] = '\n';
	}
	if (log_fd >= 0) {
		(void)write(log_fd, line, (size_t)n); /* O_APPEND: one write = one line, no fsync needed */
	}
	(void)write(2, line, (size_t)n);
}

/* IOSurface is reached by name at run time so that this file links against nothing but libSystem/objc. */
static uint32_t (*p_IOSurfaceGetID)(void *);
static size_t (*p_IOSurfaceGetWidth)(void *);
static size_t (*p_IOSurfaceGetHeight)(void *);
static size_t (*p_IOSurfaceGetBytesPerRow)(void *);
static uint32_t (*p_IOSurfaceGetPixelFormat)(void *);
static size_t (*p_IOSurfaceGetPlaneCount)(void *);

static void
describe_surface(char *out, size_t size, void *surface)
{
	uint32_t pf;
	char fcc[5];

	if (surface == NULL || p_IOSurfaceGetID == NULL || p_IOSurfaceGetWidth == NULL ||
	    p_IOSurfaceGetHeight == NULL || p_IOSurfaceGetBytesPerRow == NULL ||
	    p_IOSurfaceGetPixelFormat == NULL || p_IOSurfaceGetPlaneCount == NULL) {
		snprintf(out, size, "surface %p", surface);
		return;
	}
	pf = __builtin_bswap32(p_IOSurfaceGetPixelFormat(surface));
	memcpy(fcc, &pf, 4);
	fcc[4] = 0;
	for (int i = 0; i < 4; i++) {
		if (fcc[i] < 32 || fcc[i] > 126) {
			fcc[i] = '.';
		}
	}
	snprintf(out, size, "IOSurface id %u '%s' %zux%zu bpr %zu planes %zu", p_IOSurfaceGetID(surface), fcc,
	    p_IOSurfaceGetWidth(surface), p_IOSurfaceGetHeight(surface), p_IOSurfaceGetBytesPerRow(surface),
	    p_IOSurfaceGetPlaneCount(surface));
}

/* ---- the original implementations ------------------------------------------------------------- */

static id (*o_initAPI_props)(id, SEL, unsigned long, id);
static id (*o_initAPI)(id, SEL, unsigned long);
static id (*o_initAPI_sharegroup)(id, SEL, unsigned long, id);
static unsigned long (*o_API)(id, SEL);
static id (*o_sharegroup)(id, SEL);
static unsigned long (*o_setParameter)(id, SEL, unsigned, const int *);
static void *(*o_getMacro)(id, SEL);
static BOOL (*o_texImage)(id, SEL, void *, unsigned long, unsigned long, int, int, unsigned long, unsigned long,
    uint32_t, BOOL);
static BOOL (*o_attachImage)(id, SEL, unsigned long, void *, BOOL);
static void (*o_dealloc)(id, SEL);
static BOOL (*o_setCurrent)(id, SEL, id);
static id (*o_current)(id, SEL);
static BOOL (*o_present)(id, SEL, unsigned long);
static BOOL (*o_rbStorage)(id, SEL, unsigned long, id);

static unsigned n_initAPI_props, n_initAPI, n_initAPI_sg, n_API, n_sharegroup, n_setParameter, n_getMacro, n_tex,
    n_attach, n_dealloc, n_setCurrent, n_current, n_present, n_rb, n_setCurrent_changed;
static id last_current;

/* true for the first HOOK_LOG_FIRST calls, then for every HOOK_LOG_EVERY-th one */
static int
want_log(unsigned *counter)
{
	unsigned c = __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);

	return c < HOOK_LOG_FIRST || (c % HOOK_LOG_EVERY) == 0;
}

static id
h_initAPI_props(id self, SEL sel, unsigned long api, id props)
{
	id r = o_initAPI_props(self, sel, api, props);

	if (want_log(&n_initAPI_props)) {
		say("hook_log: -initWithAPI:%lu properties:%p -> %p  [thread %p]\n", api, (void *)props, (void *)r,
		    (void *)pthread_self());
	}
	return r;
}

static id
h_initAPI(id self, SEL sel, unsigned long api)
{
	id r = o_initAPI(self, sel, api);

	if (want_log(&n_initAPI)) {
		say("hook_log: -initWithAPI:%lu -> %p\n", api, (void *)r);
	}
	return r;
}

static id
h_initAPI_sharegroup(id self, SEL sel, unsigned long api, id group)
{
	id r = o_initAPI_sharegroup(self, sel, api, group);

	if (want_log(&n_initAPI_sg)) {
		say("hook_log: -initWithAPI:%lu sharegroup:%p -> %p\n", api, (void *)group, (void *)r);
	}
	return r;
}

static unsigned long
h_API(id self, SEL sel)
{
	unsigned long r = o_API(self, sel);

	if (want_log(&n_API)) {
		say("hook_log: -API (%p) = %lu\n", (void *)self, r);
	}
	return r;
}

static id
h_sharegroup(id self, SEL sel)
{
	id r = o_sharegroup(self, sel);

	if (want_log(&n_sharegroup)) {
		say("hook_log: -sharegroup (%p) = %p\n", (void *)self, (void *)r);
	}
	return r;
}

static unsigned long
h_setParameter(id self, SEL sel, unsigned parameter, const int *values)
{
	unsigned long r = o_setParameter(self, sel, parameter, values);

	if (want_log(&n_setParameter)) {
		say("hook_log: -setParameter:%#x to:%d (%p) = %lu\n", parameter, values ? values[0] : 0, (void *)self, r);
	}
	return r;
}

/* slots of eagl_slots.txt worth looking at: where do they point in the REAL OpenGLES/GLEngine? */
static void
look_at_table(void *table)
{
	static const unsigned slots[] = { 0x10, 0x30, 0x58, 0x210, 0x2d0, 0x2d8, 0x338, 0x1000, 0x2008, 0x2048 };
	uintptr_t *t = table;

	say("hook_log:   table %p: [0] = %#lx (the context passed as first argument)\n", table, (unsigned long)t[0]);
	for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		void *fn = (void *)t[slots[i] / 8];
		Dl_info info;

		memset(&info, 0, sizeof(info));
		if (fn != NULL && dladdr(fn, &info) && info.dli_sname) {
			const char *base = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;

			say("hook_log:   slot %#06x -> %p  %s (%s)\n", slots[i], fn, info.dli_sname,
			    base ? base + 1 : (info.dli_fname ? info.dli_fname : "?"));
		} else {
			say("hook_log:   slot %#06x -> %p  (no symbol)\n", slots[i], fn);
		}
	}
}

static void *
h_getMacro(id self, SEL sel)
{
	void *r = o_getMacro(self, sel);
	unsigned c = __atomic_fetch_add(&n_getMacro, 1, __ATOMIC_RELAXED);

	if (c < 3) {
		say("hook_log: -getMacroContextPrivate (%p) = %p\n", (void *)self, r);
		if (r != NULL) {
			look_at_table(r);
		}
	}
	return r;
}

static BOOL
h_texImage(id self, SEL sel, void *surface, unsigned long target, unsigned long internal, int width, int height,
    unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
	BOOL r = o_texImage(self, sel, surface, target, internal, width, height, format, type, plane, invert);

	if (want_log(&n_tex)) {
		char s[200];

		describe_surface(s, sizeof(s), surface);
		say("hook_log: -texImageIOSurface: %s target %#lx internal %#lx %dx%d format %#lx type %#lx plane %u invert %d = %d\n",
		    s, target, internal, width, height, format, type, plane, (int)invert, (int)r);
	}
	return r;
}

static BOOL
h_attachImage(id self, SEL sel, unsigned long target, void *surface, BOOL inverted)
{
	BOOL r = o_attachImage(self, sel, target, surface, inverted);

	if (want_log(&n_attach)) {
		char s[200];

		describe_surface(s, sizeof(s), surface);
		say("hook_log: -attachImage: target %#lx toCoreSurface: %s inverted %d = %d\n", target, s, (int)inverted,
		    (int)r);
	}
	return r;
}

static void
h_dealloc(id self, SEL sel)
{
	if (want_log(&n_dealloc)) {
		say("hook_log: -dealloc (%p)\n", (void *)self);
	}
	o_dealloc(self, sel);
}

static BOOL
h_setCurrent(id cls, SEL sel, id context)
{
	BOOL r = o_setCurrent(cls, sel, context);

	if (context != last_current) {
		__atomic_fetch_add(&n_setCurrent_changed, 1, __ATOMIC_RELAXED);
	}
	if (want_log(&n_setCurrent)) {
		say("hook_log: +setCurrentContext:%p = %d  [thread %p]\n", (void *)context, (int)r, (void *)pthread_self());
	}
	last_current = context;
	return r;
}

static id
h_current(id cls, SEL sel)
{
	id r = o_current(cls, sel);

	__atomic_fetch_add(&n_current, 1, __ATOMIC_RELAXED);
	return r;
}

static BOOL
h_present(id self, SEL sel, unsigned long rb)
{
	BOOL r = o_present(self, sel, rb);

	if (want_log(&n_present)) {
		say("hook_log: -presentRenderbuffer:%lu = %d (CoreAnimation is NOT expected to call this)\n", rb, (int)r);
	}
	return r;
}

static BOOL
h_rbStorage(id self, SEL sel, unsigned long rb, id drawable)
{
	BOOL r = o_rbStorage(self, sel, rb, drawable);

	if (want_log(&n_rb)) {
		say("hook_log: -renderbufferStorage:%lu fromDrawable:%p = %d\n", rb, (void *)drawable, (int)r);
	}
	return r;
}

/* ---- installation -------------------------------------------------------------------------------- */

static void
hook(Class cls, int meta, const char *name, void *ours, void **original)
{
	SEL sel = sel_registerName(name);
	Method m = meta ? class_getClassMethod(cls, sel) : class_getInstanceMethod(cls, sel);
	Class target = meta ? object_getClass((id)cls) : cls;
	IMP orig;

	if (m == NULL || target == Nil) {
		say("hook_log: EAGLContext has no %c%s\n", meta ? '+' : '-', name);
		return;
	}
	orig = method_getImplementation(m);
	if (orig == NULL) {
		say("hook_log: %c%s has no implementation, not hooked\n", meta ? '+' : '-', name);
		return;
	}
	*original = (void *)orig; /* stored before the replacement is visible */
	/* class_replaceMethod adds to this class when the method is inherited, replaces otherwise */
	class_replaceMethod(target, sel, (IMP)ours, method_getTypeEncoding(m));
	say("hook_log: %c[EAGLContext %s] %s (orig %p)\n", meta ? '+' : '-', name, method_getTypeEncoding(m) ?: "?",
	    (void *)orig);
}

static void
summary(void)
{
	say("hook_log: calls so far: initAPI:properties %u, initAPI %u, initAPI:sharegroup %u, API %u, sharegroup %u, "
	    "setParameter %u, getMacro %u, texImageIOSurface %u, attachImage %u, dealloc %u, setCurrentContext %u "
	    "(%u changes), currentContext %u, present %u, renderbufferStorage %u\n",
	    n_initAPI_props, n_initAPI, n_initAPI_sg, n_API, n_sharegroup, n_setParameter, n_getMacro, n_tex, n_attach,
	    n_dealloc, n_setCurrent, n_setCurrent_changed, n_current, n_present, n_rb);
}

static int installed;

/* returns 1 when EAGLContext was found and hooked (once) */
static int
try_install(void)
{
	Class cls;

	if (installed) {
		return 1;
	}
	cls = objc_getClass("EAGLContext");
	if (cls == Nil) {
		return 0;
	}
	installed = 1;
	hook(cls, 0, "initWithAPI:properties:", (void *)h_initAPI_props, (void **)&o_initAPI_props);
	hook(cls, 0, "initWithAPI:", (void *)h_initAPI, (void **)&o_initAPI);
	hook(cls, 0, "initWithAPI:sharegroup:", (void *)h_initAPI_sharegroup, (void **)&o_initAPI_sharegroup);
	hook(cls, 0, "API", (void *)h_API, (void **)&o_API);
	hook(cls, 0, "sharegroup", (void *)h_sharegroup, (void **)&o_sharegroup);
	hook(cls, 0, "setParameter:to:", (void *)h_setParameter, (void **)&o_setParameter);
	hook(cls, 0, "getMacroContextPrivate", (void *)h_getMacro, (void **)&o_getMacro);
	hook(cls, 0, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", (void *)h_texImage,
	    (void **)&o_texImage);
	hook(cls, 0, "attachImage:toCoreSurface:invertedRender:", (void *)h_attachImage, (void **)&o_attachImage);
	hook(cls, 0, "dealloc", (void *)h_dealloc, (void **)&o_dealloc);
	hook(cls, 0, "presentRenderbuffer:", (void *)h_present, (void **)&o_present);
	hook(cls, 0, "renderbufferStorage:fromDrawable:", (void *)h_rbStorage, (void **)&o_rbStorage);
	hook(cls, 1, "setCurrentContext:", (void *)h_setCurrent, (void **)&o_setCurrent);
	hook(cls, 1, "currentContext", (void *)h_current, (void **)&o_current);
	return 1;
}

static dispatch_source_t timer;
static unsigned timer_ticks;

static void
timer_tick(void)
{
	timer_ticks++;
	if (!installed) {
		if (try_install()) {
			say("hook_log: EAGLContext appeared late (tick %u), hooked\n", timer_ticks);
		} else if (timer_ticks == 3 || timer_ticks == 30) {
			say("hook_log: still no EAGLContext class after %u ticks\n", timer_ticks);
		}
	}
	summary();
}

__attribute__((constructor))
static void
hook_log_init(void)
{
	const char *name = getprogname();
	unsigned char uuid[16];
	char uuids[40] = "unknown";
	bool (*get_uuid)(unsigned char *);

	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	log_fd = open(HOOK_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	get_uuid = (bool (*)(unsigned char *))dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_uuid");
	if (get_uuid != NULL && get_uuid(uuid)) {
		snprintf(uuids, sizeof(uuids),
		    "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1], uuid[2],
		    uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12],
		    uuid[13], uuid[14], uuid[15]);
	}
	say("hook_log: loaded: %s pid %d dyld shared cache UUID %s (log fd %d)\n", name, getpid(), uuids, log_fd);
	p_IOSurfaceGetID = (uint32_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetID");
	p_IOSurfaceGetWidth = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth");
	p_IOSurfaceGetHeight = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight");
	p_IOSurfaceGetBytesPerRow = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	p_IOSurfaceGetPixelFormat = (uint32_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	p_IOSurfaceGetPlaneCount = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount");
	if (!try_install()) {
		say("hook_log: backboardd has no EAGLContext class yet; will retry from the 10 s timer\n");
	}
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
	if (timer != NULL) {
		dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 10 * NSEC_PER_SEC,
		    NSEC_PER_SEC);
		dispatch_source_set_event_handler(timer, ^{ timer_tick(); });
		dispatch_resume(timer);
	}
}
