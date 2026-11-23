/*
 * gl_osmesa_s8.c: the OpenGL ES library behind eagl_s8.c is Mesa 25.0.7 as userland/mesa/ builds it: libOSMesa.8.dylib,
 * OSMesa with an OpenGL ES profile, gallium drivers panfrost (Mali-G71 through the S8Mali kernel service) and softpipe (the
 * CPU). This file implements hook_gl.h over it. It is loaded with dlopen() at run time (nothing is linked), so that
 * libs8gl.dylib builds without the Mesa artifact and so that backboardd without Mesa installed simply keeps CoreAnimation's
 * software renderer (hook_gl_create() returns NULL, with the reason in hook_gl_error()).
 *
 * Port of src/gl_osmesa.c of the S9+ port (MIT License, Copyright (c) 2025-2026 the HakiOS authors; full text in
 * userland/s8mali/s8mali_drm.h), reworked for dlopen, the S8Mali shim in this library (s8mali_drm.c, handed to Mesa through
 * the table of s8_drm_ops.h) and a CPU-only fallback.
 *
 * Which driver draws is decided once, at the first context:
 *   1. S8GL_SOFTWARE=1 in the environment or the boot-arg s8gl_sw=1: OSMESA_SOFTWARE=1 is set, Mesa uses softpipe
 *      (milestone A of the experiment: what does CoreAnimation draw, even with no GPU).
 *   2. otherwise the S8Mali service is waited for (S8GL_WAIT / boot-arg s8gl_wait seconds, default 3; the kernel brings it up
 *      some seconds after launchd) and, if it is there, the shim is given to Mesa (s8_drm_set_ops): Mesa's panfrost
 *      then finds the "panfrost" DRM device; if anything of that fails Mesa falls back to softpipe by itself (patch 0004).
 *   3. no service: no table is given; Mesa's stand-in fails every DRM call and softpipe draws.
 * The renderer string of the first context ("Mali-G71 ... (Panfrost)" or "softpipe") is logged.
 *
 * Library: $S8GL_MESA, else /usr/local/lib/s8mesa/libOSMesa.8.dylib (where s8gl_test looks), /usr/lib/s8mesa/..., /usr/lib/.
 */
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "hook_gl.h"
#include "../s8mali/s8mali_drm.h"
#include "../mesa/overlay/src/s8-drm/s8_drm_ops.h"

extern void s8gl_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* GL/osmesa.h of Mesa */
#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
#define GL_UNSIGNED_BYTE                0x1401
#define GL_TEXTURE_2D                   0x0de1
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01

typedef void *OSMesaContext;
static OSMesaContext (*p_CreateContextAttribs)(const int *attributes, OSMesaContext share);
static unsigned char (*p_MakeCurrent)(OSMesaContext context, void *buffer, unsigned type, int width, int height);
static void (*p_DestroyContext)(OSMesaContext context);
static void *(*p_GetProcAddress)(const char *name);

/*
 * OSMesa draws its "window" into memory handed to it. Nothing here draws into a window (the window server draws into
 * textures of its own, see eagl_s8.c): a few pixels will do.
 */
#define WINDOW  16

struct hook_gl {
	OSMesaContext context;
	unsigned char window[WINDOW * WINDOW * 4];
};

enum driver { DRIVER_UNKNOWN, DRIVER_SOFTWARE, DRIVER_PANFROST, DRIVER_OTHER };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *library;
static int load_tried, gpu_offered;
static enum driver driver;
static char error_text[400];

static void
fail(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(error_text, sizeof(error_text), format, ap);
	va_end(ap);
	s8gl_say("s8gl: %s\n", error_text);
}

const char *
hook_gl_error(void)
{
	return error_text;
}

const char *
hook_gl_library(void)
{
	return driver == DRIVER_PANFROST ? "Mesa OSMesa, Panfrost on the S8Mali service" :
	    driver == DRIVER_SOFTWARE ? "Mesa OSMesa, softpipe (CPU)" : "Mesa OSMesa";
}

/* a boot-arg NAME=1, or the environment variable ENV */
static const char *
flag_value(const char *bootarg, const char *env)
{
	static char args[1024];
	static int read;
	const char *v;
	size_t n = strlen(bootarg);

	if (!read) {
		size_t size = sizeof(args) - 1;

		read = 1;
		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
			args[0] = 0;
		}
	}
	for (const char *a = args; (a = strstr(a, bootarg)) != NULL; a += n) {
		if ((a == args || a[-1] == ' ') && a[n] == '=') {
			return a + n + 1;
		}
	}
	v = getenv(env);
	return v != NULL && *v != 0 ? v : NULL;
}

/*
 * Mesa's GLSL built-in library alone takes tens of MB when the first shader is compiled, and a launchd daemon gets a small
 * fatal per-task memory limit (jetsam). The kernel's memorystatus_control() lets a root process set its own limit (command 7 =
 * SET_MEMLIMIT_PROPERTIES, MB, attribute 0 = soft; as s8gl_test does). Only when S8GL_MEMLIMIT_MB / boot-arg s8gl_memlimit_mb
 * is given: backboardd's own limit is not known (UNVERIFIED), and raising it is not needed to see the first frames.
 */
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);
struct memlimit_props {
	int32_t active;
	uint32_t active_attr;
	int32_t inactive;
	uint32_t inactive_attr;
};

static void
raise_memory_limit(void)
{
	const char *v = flag_value("s8gl_memlimit_mb", "S8GL_MEMLIMIT_MB");
	struct memlimit_props limit;
	int rc;

	if (v == NULL || atoi(v) <= 0) {
		return;
	}
	limit.active = limit.inactive = atoi(v);
	limit.active_attr = limit.inactive_attr = 0;
	rc = memorystatus_control(7, (int32_t)getpid(), 0, &limit, sizeof(limit));
	s8gl_say("s8gl: memorystatus_control(SET_MEMLIMIT_PROPERTIES %d MB soft) rc %d errno %d\n", limit.active, rc, rc ? errno : 0);
}

/* the shim's functions that s8mali_drm.h declares, wrapped for the two that differ in type */
static void *
shim_get_version(int fd)
{
	return drmGetVersion(fd);
}

static void
shim_free_version(void *version)
{
	drmFreeVersion(version);
}

static void
fill_ops(struct s8_drm_ops *o)
{
	memset(o, 0, sizeof(*o));
	o->size = sizeof(*o);
	o->OpenWithType = drmOpenWithType;
	o->Ioctl = drmIoctl;
	o->CommandWrite = drmCommandWrite;
	o->CommandWriteRead = drmCommandWriteRead;
	o->GetVersion = shim_get_version;
	o->FreeVersion = shim_free_version;
	o->GetCap = drmGetCap;
	o->CloseBufferHandle = drmCloseBufferHandle;
	o->PrimeHandleToFD = drmPrimeHandleToFD;
	o->PrimeFDToHandle = drmPrimeFDToHandle;
	o->SyncobjCreate = drmSyncobjCreate;
	o->SyncobjDestroy = drmSyncobjDestroy;
	o->SyncobjReset = drmSyncobjReset;
	o->SyncobjWait = drmSyncobjWait;
	o->SyncobjFDToHandle = drmSyncobjFDToHandle;
	o->SyncobjHandleToFD = drmSyncobjHandleToFD;
	o->SyncobjImportSyncFile = drmSyncobjImportSyncFile;
	o->SyncobjExportSyncFile = drmSyncobjExportSyncFile;
	o->SyncobjTimelineWait = drmSyncobjTimelineWait;
	o->SyncobjTransfer = drmSyncobjTransfer;
	o->Map = hakios_drm_map;
	o->Unmap = hakios_drm_unmap;
}

/* Load Mesa once, and decide between the GPU and the CPU. 1: loaded. */
static int
load_library(void)
{
	static const char *const defaults[] = { "/usr/local/lib/s8mesa/libOSMesa.8.dylib", "/usr/lib/s8mesa/libOSMesa.8.dylib",
		"/usr/lib/libOSMesa.8.dylib" };
	const char *path = getenv("S8GL_MESA"), *v;
	int software;

	if (library != NULL) {
		return 1;
	}
	if (load_tried) {
		return 0;
	}
	load_tried = 1;
	raise_memory_limit();
	software = (v = flag_value("s8gl_sw", "S8GL_SOFTWARE")) != NULL && atoi(v) == 1;
	if (path != NULL) {
		library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	}
	for (unsigned i = 0; library == NULL && i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		path = defaults[i];
		library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	}
	if (library == NULL) {
		fail("no Mesa: %s (looked at $S8GL_MESA and /usr/local/lib/s8mesa/, /usr/lib/s8mesa/, /usr/lib/)", dlerror());
		return 0;
	}
	*(void **)&p_CreateContextAttribs = dlsym(library, "OSMesaCreateContextAttribs");
	*(void **)&p_MakeCurrent = dlsym(library, "OSMesaMakeCurrent");
	*(void **)&p_DestroyContext = dlsym(library, "OSMesaDestroyContext");
	*(void **)&p_GetProcAddress = dlsym(library, "OSMesaGetProcAddress");
	if (!p_CreateContextAttribs || !p_MakeCurrent || !p_DestroyContext || !p_GetProcAddress) {
		fail("%s has not the OSMesa entry points (%p %p %p %p)", path, *(void **)&p_CreateContextAttribs,
		    *(void **)&p_MakeCurrent, *(void **)&p_DestroyContext, *(void **)&p_GetProcAddress);
		dlclose(library);
		library = NULL;
		return 0;
	}
	s8gl_say("s8gl: Mesa loaded from %s\n", path);
	if (software) {
		setenv("OSMESA_SOFTWARE", "1", 1);
		s8gl_say("s8gl: software rendering asked for (s8gl_sw=1): OSMESA_SOFTWARE=1, softpipe\n");
	} else {
		int seconds = (v = flag_value("s8gl_wait", "S8GL_WAIT")) != NULL ? atoi(v) : 3;

		if (s8mali_wait_service(seconds < 0 ? 0 : seconds)) {
			int (*set_ops)(const struct s8_drm_ops *) = (int (*)(const struct s8_drm_ops *))dlsym(library, "s8_drm_set_ops");
			struct s8_drm_ops table;

			if (set_ops == NULL) {
				s8gl_say("s8gl: this Mesa has no s8_drm_set_ops (the userland/mesa overlay is not in it): softpipe only\n");
			} else {
				fill_ops(&table);
				gpu_offered = set_ops(&table) == 0;
				s8gl_say("s8gl: the S8Mali service is there; the DRM table %s Mesa\n", gpu_offered ? "was given to" : "was REFUSED by");
			}
		} else {
			s8gl_say("s8gl: no S8Mali service (waited %d s): Mesa has no GPU, softpipe draws\n", seconds);
		}
	}
	return 1;
}

void *
hook_gl_proc(const char *name)
{
	void *p = NULL;

	if (library == NULL) {
		return NULL;
	}
	p = p_GetProcAddress(name);
	if (p == NULL) {
		p = dlsym(library, name);
	}
	return p;
}

/* Which driver draws, told by the strings of the (current) context. */
static enum driver
which_driver(void)
{
	const unsigned char *(*get_string)(unsigned) = (const unsigned char *(*)(unsigned))hook_gl_proc("glGetString");
	const char *vendor, *renderer;

	if (get_string == NULL) {
		return DRIVER_OTHER;
	}
	vendor = (const char *)get_string(GL_VENDOR);
	renderer = (const char *)get_string(GL_RENDERER);
	s8gl_say("s8gl: GL_VENDOR \"%s\" GL_RENDERER \"%s\"\n", vendor ? vendor : "(none)", renderer ? renderer : "(none)");
	if (renderer == NULL || strstr(renderer, "softpipe") != NULL || strstr(renderer, "llvmpipe") != NULL) {
		return DRIVER_SOFTWARE;
	}
	if (strstr(renderer, "anfrost") != NULL) {
		return DRIVER_PANFROST;
	}
	return DRIVER_OTHER;
}

struct hook_gl *
hook_gl_create(int version)
{
	const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, version, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	struct hook_gl *gl;

	error_text[0] = 0;
	pthread_mutex_lock(&lock);
	if (!load_library()) {
		pthread_mutex_unlock(&lock);
		if (error_text[0] == 0) {
			fail("the library did not load (earlier attempt)");
		}
		return NULL;
	}
	pthread_mutex_unlock(&lock);
	if ((gl = calloc(1, sizeof(*gl))) == NULL) {
		fail("out of memory");
		return NULL;
	}
	if ((gl->context = p_CreateContextAttribs(attributes, NULL)) == NULL) {
		fail("OSMesaCreateContextAttribs(ES %d.0) failed", version);
		free(gl);
		return NULL;
	}
	return gl;
}

int
hook_gl_make_current(struct hook_gl *gl)
{
	unsigned char ok;

	if (library == NULL) {
		return 0;
	}
	if (gl == NULL) {
		return p_MakeCurrent(NULL, NULL, 0, 0, 0);
	}
	ok = p_MakeCurrent(gl->context, gl->window, GL_UNSIGNED_BYTE, WINDOW, WINDOW);
	if (ok && driver == DRIVER_UNKNOWN) {
		driver = which_driver();
		s8gl_say("s8gl: the driver is %s\n", hook_gl_library());
	}
	return ok;
}

void
hook_gl_destroy(struct hook_gl *gl)
{
	if (gl != NULL) {
		p_DestroyContext(gl->context);
		free(gl);
	}
}

/*
 * OES_EGL_image, with the IOSurface's ID as the image: the kernel's GPU driver would make a buffer object of the surface's
 * memory. NOT DONE: it needs (1) the S9+ hunks of panfrost-darwin.patch (pan_kmod/pan_bo/osmesa target: drmPrimeFDToHandle
 * with the surface ID, whole-BO import) ported to Mesa 25.0.7 as userland/mesa/patches/0006, and (2) the kernel selector 0x7e in
 * S8Mali (s8mali_drm.h, struct s8mali_surface). Until then the call below fails with a GL error (or the entry point does not
 * exist) and the caller copies. Only the Panfrost driver is asked; eagl_s8.c asks at all only with glshare=1.
 */
int
hook_gl_surface_texture(struct hook_gl *gl, uint32_t surface_id)
{
	static void (*image_target)(unsigned target, void *image);
	static unsigned (*get_error)(void);
	static int asked;

	(void)gl;
	if (!asked) {
		asked = 1;
		image_target = (void (*)(unsigned, void *))hook_gl_proc("glEGLImageTargetTexture2DOES");
		get_error = (unsigned (*)(void))hook_gl_proc("glGetError");
		s8gl_say("s8gl: surface sharing: glEGLImageTargetTexture2DOES %s, driver %s\n", image_target ? "found" : "NOT in the library",
		    hook_gl_library());
	}
	if (image_target == NULL || get_error == NULL || driver != DRIVER_PANFROST) {
		return 0;
	}
	while (get_error() != 0) {
	}
	image_target(GL_TEXTURE_2D, (void *)(uintptr_t)surface_id);
	return get_error() == 0;
}

/*
 * Wait for the GPU. glFinish makes Mesa's panfrost wait for its last job (drmSyncobjWait on the shim's job numbers); the
 * S8Mali kernel driver runs jobs synchronously inside SUBMIT today (s8mali-abi.md), so the explicit wait for the last job
 * number is a no-op that stays correct when the kernel gets a queue.
 */
int
hook_gl_surface_wait(struct hook_gl *gl, uint32_t surface_id)
{
	static void (*finish)(void);

	(void)gl;
	(void)surface_id;
	if (finish == NULL) {
		finish = (void (*)(void))hook_gl_proc("glFinish");
	}
	if (finish == NULL) {
		return 0;
	}
	finish();
	if (driver == DRIVER_PANFROST) {
		uint64_t seq = s8mali_last_seq();

		if (seq != 0 && s8mali_wait_seq(seq, INT64_MAX) != 0) {
			return 0;
		}
	}
	return 1;
}
