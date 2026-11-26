/*
 * eagl_s8.c: an OpenGL ES implementation behind Darwin's EAGLContext, inside the window server
 * (backboardd, iOS 16.7.16 / 20H392, Galaxy S8 port). Built into libs8gl.dylib together with
 * gl_osmesa_s8.c (the library behind it: Mesa OSMesa ES profile, Panfrost on the S8Mali kernel
 * service, softpipe when that is not there) and s8mali_drm.c (the DRM shim for Mesa).
 *
 * Port of src/eagl.c of the S9+ port (MIT License, Copyright (c) 2025-2026 the HakiOS authors; full
 * text in userland/s8mali/s8mali_drm.h). What was kept: the replacement of the EAGLContext
 * methods (by name), the function table at the offsets of eagl_slots.txt, the rewriting of
 * CoreAnimation's shaders, blue-first pixels, the textures that stand for IOSurfaces (copy path
 * and the zero-copy hook), the frame end, and the draws made with no program. What was NOT
 * ported (the S9+ file is 4800 lines of tuning for the Adreno/Mali-G72 and for CoreAnimation 16.5):
 *   TODO vibrant layers inside groups (group_draw_begin, eagl.c 3085-3360), the blur whose radius
 *        varies (vblur_*, 2512-2670, 2924-3085), the CA "function" specific programs (vibrant
 *        colour matrix, masking, eight times smaller, four-sample blur: plain_draw_begin 3416-3497).
 *        They need CA's function number, which the S9+ port reads from CA::OGL::GLESContext
 *        (ca_context+0x10 -> +0x10, hakios_gl_function_values() called from a hook of the vtable slot
 *        GLContext::function_uniform). The structure offsets are NOT verified for 20H392
 *        (hook_addrs_20H392.h, OFF_FUNCTION_UNIFORM), so none of it is guessed here.
 *   TODO mirror textures / GPU blit into the surface (blit_targets), host-presented frames, the
 *        census, glskip/glshow/glprof, the ASTC thumbnail. Diagnostics only.
 *
 * The OpenGL ES library is NOT linked: eagl_gen_s8.h declares every gl* function as a static
 * function pointer which eagl_resolve_functions() fills from hook_gl_proc() after the library
 * is loaded (a missing function is logged once, and the entry that needs it says so when called).
 *
 * The log is /private/var/tmp/hook_log.txt (O_APPEND, the same file hook_log.c writes) and stderr, lines
 * "s8gl: ...". Milestone A of the experiment: the first calls of every kind are written down with their
 * arguments (setParameter, getMacroContextPrivate and which table slot is called first, texImageIOSurface,
 * attachImage, presentRenderbuffer, shaders, formats, frames). Boot-args or environment (S8GL_<NAME> in
 * capitals, for a program started by hand):
 *   gl=0            nothing is replaced (hook_inject.c honours it too)
 *   gltrace=A-B     every call of frames A to B is written down (default 1-2; gltrace=0 none; lines are capped by gltrace_max=N, 20000)
 *   glframes=1      a line for every frame
 *   glerrors=1|2    glGetError after every call, for 600 frames (1) or always (2)
 *   glshare=1       ask the library to share an IOSurface's memory with a texture (hook_gl_surface_texture); default 0:
 *                   every IOSurface is copied (sharing needs a Mesa patch and kernel selector 0x7e that do not exist yet)
 *   glfetch=0       EXT_shader_framebuffer_fetch is not offered to CoreAnimation
 *   glprecision=0   the shaders' lowp/mediump are left as they are (else everything is highp)
 *   glplain=0|1|2   draws with no program: 0 left out, 1 (default) drawn when a texture is bound, 2 always
 *   glwait=0        a frame's end does not wait for the GPU
 *   glsurfaces=N, glsurfaces_mb=N   bound of the table of textures that stand for IOSurfaces (4096, 512 MB)
 *
 * No ARC, no Objective-C syntax: plain C against the ObjC runtime.
 *
 * Build: userland/hook/build-hook.sh (xcrun --sdk iphoneos clang -arch arm64 -dynamiclib -install_name
 * /usr/lib/libs8gl.dylib), see there. NOT BUILT OR RUN ON A DEVICE YET (compile-checked on the host against stub headers
 * only: userland/hook/stubs/).
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "hook_gl.h"

/* ---- CoreFoundation and IOSurface: a few functions, reached by name so that nothing but libSystem/objc/CF is linked ---- */

extern const void *CFRetain(const void *object);
extern void CFRelease(const void *object);
extern long CFGetRetainCount(const void *object);

typedef struct __IOSurface *IOSurfaceRef;
#define kIOSurfaceLockReadOnly 1u

static struct {
	uint32_t (*GetID)(IOSurfaceRef);
	size_t (*GetWidth)(IOSurfaceRef);
	size_t (*GetHeight)(IOSurfaceRef);
	size_t (*GetBytesPerRow)(IOSurfaceRef);
	size_t (*GetBytesPerElement)(IOSurfaceRef);
	uint32_t (*GetPixelFormat)(IOSurfaceRef);
	size_t (*GetPlaneCount)(IOSurfaceRef);
	void *(*GetBaseAddress)(IOSurfaceRef);
	void *(*GetBaseAddressOfPlane)(IOSurfaceRef, size_t);
	size_t (*GetBytesPerRowOfPlane)(IOSurfaceRef, size_t);
	uint32_t (*GetSeed)(IOSurfaceRef);
	size_t (*GetAllocSize)(IOSurfaceRef);
	int (*Lock)(IOSurfaceRef, uint32_t, uint32_t *);
	int (*Unlock)(IOSurfaceRef, uint32_t, uint32_t *);
} ios;
static int ios_ok;

#define IOSurfaceGetID(s)               ios.GetID(s)
#define IOSurfaceGetWidth(s)            ios.GetWidth(s)
#define IOSurfaceGetHeight(s)           ios.GetHeight(s)
#define IOSurfaceGetBytesPerRow(s)      ios.GetBytesPerRow(s)
#define IOSurfaceGetBytesPerElement(s)  ios.GetBytesPerElement(s)
#define IOSurfaceGetPixelFormat(s)      ios.GetPixelFormat(s)
#define IOSurfaceGetPlaneCount(s)       ios.GetPlaneCount(s)
#define IOSurfaceGetBaseAddress(s)      ios.GetBaseAddress(s)
#define IOSurfaceGetBaseAddressOfPlane(s, p) ios.GetBaseAddressOfPlane(s, p)
#define IOSurfaceGetBytesPerRowOfPlane(s, p) ios.GetBytesPerRowOfPlane(s, p)
#define IOSurfaceGetSeed(s)             ios.GetSeed(s)
#define IOSurfaceGetAllocSize(s)        ios.GetAllocSize(s)
#define IOSurfaceLock(s, o, q)          ios.Lock(s, o, q)
#define IOSurfaceUnlock(s, o, q)        ios.Unlock(s, o, q)

static int
ios_resolve(void)
{
	void *p[14];
	static const char *const names[14] = { "IOSurfaceGetID", "IOSurfaceGetWidth", "IOSurfaceGetHeight",
		"IOSurfaceGetBytesPerRow", "IOSurfaceGetBytesPerElement", "IOSurfaceGetPixelFormat", "IOSurfaceGetPlaneCount",
		"IOSurfaceGetBaseAddress", "IOSurfaceGetBaseAddressOfPlane", "IOSurfaceGetBytesPerRowOfPlane",
		"IOSurfaceGetSeed", "IOSurfaceGetAllocSize", "IOSurfaceLock", "IOSurfaceUnlock" };
	int ok = 1;

	for (int i = 0; i < 14; i++) {
		p[i] = dlsym(RTLD_DEFAULT, names[i]);
		ok &= p[i] != NULL;
	}
	if (ok) {
		memcpy(&ios, p, sizeof(p));       /* (the struct is those 14 pointers in that order) */
	}
	return ok;
}

/* ---- OpenGL ES: the types, the few names used here ---------------------------------------------------- */

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef GLDEBUGPROC GLDEBUGPROCKHR;

#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_UNPACK_ALIGNMENT             0x0cf5
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_BGRA                         0x80e1
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_TEXTURE                      0x1702
#define GL_TEXTURE0                     0x84c0
#define GL_TEXTURE_HEIGHT               0x1001
#define GL_VERTEX_ATTRIB_ARRAY_ENABLED  0x8622
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8cd0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8cd1
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER                  0x8d40
#define GL_READ_FRAMEBUFFER             0x8ca8

struct eagl;
static int eagl_tracing;                /* this frame's calls are written down (gltrace=A-B) */
static int frame_lines;                 /* glframes=1: a line for every frame */
static void eagl_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void eagl_trace_floats(const float *values, int count);
static void eagl_enter(struct eagl *e, unsigned offset);
static void eagl_leave(struct eagl *e, unsigned offset);
static long eagl_missing(void *context, unsigned offset);
static void *eagl_lookup(const char *name);
static void eagl_resolve_missing(const char *name);
#define EAGL_ENTER(context, offset)     eagl_enter(context, offset)
#define EAGL_LEAVE(context, offset)     eagl_leave(context, offset)

#define EAGL_GEN_DECLARATIONS
#include "eagl_gen_s8.h"
#undef EAGL_GEN_DECLARATIONS

/* ---- the log ---------------------------------------------------------------------------------------- */

int s8mali_log(const char *text, size_t n);     /* s8mali_drm.c */
size_t s8mali_stats(char *out, size_t size);    /* s8mali_drm.c: what the calls into the kernel service cost since the last call */
#define S8GL_LOG_FILE "/private/var/tmp/hook_log.txt"
static int log_fd = -2;
static int console_fd = -2;
static unsigned long trace_lines_left = 20000;  /* gltrace_max=N */

static void
log_write(const char *line, size_t n)
{
	if (log_fd == -2) {
		log_fd = open(S8GL_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	if (log_fd >= 0) {
		(void)write(log_fd, line, n);    /* O_APPEND: one write = one line, no fsync */
	}
	(void)write(2, line, n);
	/*
	 * Also on the console: the kernel's console ring is what tools/pull-kernlog.ps1 reads within seconds of a run, where the file
	 * above costs a 15 minute root reassembly. Summary lines only (not the per-call trace, not the extension lists), each cut at
	 * 240 bytes so that a frame line fits in one ring record.
	 */
	if (console_fd == -2) {
		console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (strncmp(line, "s8gl: gl:", 9) != 0 && strncmp(line, "s8gl:  ", 7) != 0) {
		char text[260];
		size_t m = n < 240 ? n : 240;

		memcpy(text, line, m);
		if (text[m - 1] != '\n') {
			text[m++] = '\n';
		}
		/* through the S8Mali service into the kernel log once that is open (the console is muted when the screen is up) */
		if (s8mali_log(text, m) != 0) {
			(void)write(console_fd, text, m);
		}
	}
}

/* exported to gl_osmesa_s8.c */
void s8gl_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
void
s8gl_say(const char *format, ...)
{
	char line[1100];
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
	log_write(line, (size_t)n);
}
#define say s8gl_say

static void
eagl_trace(const char *format, ...)
{
	char line[512] = "s8gl: gl: ";
	va_list ap;
	int n;

	if (trace_lines_left == 0) {
		return;
	}
	trace_lines_left--;
	va_start(ap, format);
	n = vsnprintf(line + 10, sizeof(line) - 10, format, ap) + 10;
	va_end(ap);
	log_write(line, n > (int)sizeof(line) - 1 ? sizeof(line) - 1 : (size_t)n);
	if (trace_lines_left == 0) {
		say("s8gl: gltrace_max reached, no more calls are written down\n");
	}
}

static void
eagl_trace_floats(const float *values, int count)
{
	char line[512] = "s8gl: gl:    ";
	int n = 13;

	for (int i = 0; i < count && i < 16 && n < 480; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %g", (double)values[i]);
	}
	line[n++] = '\n';
	log_write(line, (size_t)n);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* ---- boot-args and environment ----------------------------------------------------------------------- */

static char bootargs[1024];

/* "name=VALUE" of the boot-args, else the environment S8GL_NAME (name in capitals); NULL: not given. */
static const char *
arg_get(const char *name)
{
	size_t n = strlen(name);
	char env[64] = "S8GL_";
	const char *value;

	for (const char *a = bootargs; (a = strstr(a, name)) != NULL; a += n) {
		if ((a == bootargs || a[-1] == ' ') && a[n] == '=') {
			return a + n + 1;
		}
	}
	for (size_t i = 0; i < n && 5 + i + 1 < sizeof(env); i++) {
		env[5 + i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	}
	value = getenv(env);
	return value != NULL && *value != 0 ? value : NULL;
}

static long
arg_int(const char *name, long fallback)
{
	const char *v = arg_get(name);

	return v != NULL ? strtol(v, NULL, 10) : fallback;
}

/* ---- a context -------------------------------------------------------------------------------------- */

#define SLOTS           0x440           /* entries of the table: more than Darwin's has (checked below) */
#define NCONTEXTS       16

/* A texture that stands for an IOSurface. */
struct image {
	GLuint texture;
	IOSurfaceRef surface;
	bool target;                    /* drawn into (attachImage) rather than shown (texImageIOSurface) */
	bool invert, drawn;
	bool shared;                    /* the texture is the surface's memory: nothing to copy */
	bool wrapped;                   /* the surface is memory of somebody's with a surface made around it (no pixel format) */
	bool alpha_first;               /* its pixels are opacity, blue, green, red in memory: see my_attachImage() */
	bool painted;                   /* something has been drawn into it */
	unsigned looked;                /* ... the frame in which that memory was last looked at, */
	uint64_t sum;                   /* ... and what it added up to */
	unsigned unchanged;             /* ... and how many looks in a row found it the same (surface_changed()) */
	GLenum to, internal, format, type;
	uint32_t plane, seed;
	GLsizei width, height;
	unsigned used;                  /* the frame it was last bound or made in (image_evict()) */
	size_t bytes;                   /* its surface's size */
};

struct eagl {
	void *table[SLOTS];             /* [0] is the context itself: what every entry gets as its first argument */
	id object;                      /* the EAGLContext */
	struct hook_gl *gl;
	unsigned long api;
	struct image *images;           /* the textures that stand for IOSurfaces (image_new()), image_room of them made room for */
	unsigned image_count, image_room, image_most;
	int32_t *image_index;           /* ... and where each is, by texture name: open addressing (image_slot()) */
	unsigned index_room, index_taken;       /* (taken: entries and tombstones) */
	uint64_t image_bytes;           /* the sizes of their surfaces, together */
	struct image_counts {           /* what has happened to the table since the last line about it (image_report()) */
		unsigned made, deleted, released, respecified, evicted, untracked, refills;
		uint64_t refill_bytes;
	} image_counts;
	uint64_t image_said;            /* when that line was written (now_us()) */
	GLuint framebuffer;             /* the one bound */
	GLuint colour;                  /* ... and the texture that is its colour attachment (0: none, or no texture) */
	GLuint reader;                  /* a framebuffer of ours, to read a texture back */
	GLuint program;                 /* the one in use, as CoreAnimation has it */
	GLuint plain, lookup;           /* programs of ours for when that is none (plain_draw_begin()) */
	GLint plain_matrix, plain_textured, lookup_matrix;
	unsigned unit;                  /* the texture unit being spoken of */
	GLuint bound[8];                /* the 2D texture bound to each of the first units */
	const GLfloat *matrix;          /* where CoreAnimation keeps the matrix it gives its programs (read at draw time, as the S9+ port does) */
	struct {
		const void *pointer;
		GLsizei stride;
		GLint size;
		GLenum type;
	} arrays[8];                    /* the vertex arrays, as given */
	bool in_frame;
	/* a frame's worth */
	unsigned calls, draws, triangles, uploads, clears;
	uint64_t upload_bytes, began, attaching, flushing;
	/* everything so far */
	unsigned frames, shaders, programs, errors, shared, refused;
	uint64_t all_calls, all_draws, all_triangles, readback_us, wait_us, attach_us, flush_us, frame_us;
	unsigned most_calls, most_triangles;
	unsigned plain_drawn, plain_left_out, plain_said;
	const char *sources[256];       /* the text of the shaders being made, by name (modulo this) */
	bool fragment[256];             /* ... and which of the two kinds each is */
};

static unsigned trace_first = 1, trace_last = 2;        /* the frames whose calls are written down */
static __thread struct eagl *current;
static struct eagl *contexts[NCONTEXTS];
static uint32_t slot_calls[SLOTS];
static int check_errors;                /* glerrors=1 (first 600 frames) / 2 (always) */
static int keep_checking;
static int share_surfaces;              /* glcopy=1 -> 0; today 0 by default: hook_gl_surface_texture() cannot do it yet */
static int wait_for_frames = 1;         /* glwait=0: 0 */
static int fetch_offered = 1;           /* glfetch=0: 0 */
static int full_precision = 1;          /* glprecision=0: 0 */
static int plain_draws = 1;             /* glplain= */
static int resolved;                    /* eagl_resolve_functions() done */
static unsigned unresolved;

#define GL_SCISSOR_TEST                 0x0c11

static const char *eagl_slot_name(unsigned offset);
static int eagl_slot_expected(unsigned offset);

/* Look a function up by name, else without the vendor ending. */
static void *
eagl_lookup(const char *name)
{
	void *p = hook_gl_proc(name);
	size_t n = strlen(name);
	static const char *const endings[] = { "OES", "EXT", "APPLE", "ARB", "IMG" };

	if (p != NULL) {
		return p;
	}
	for (unsigned i = 0; i < sizeof(endings) / sizeof(endings[0]); i++) {
		size_t k = strlen(endings[i]);

		if (n > k + 2 && strcmp(name + n - k, endings[i]) == 0) {
			char plain[96];

			if (n - k < sizeof(plain)) {
				memcpy(plain, name, n - k);
				plain[n - k] = 0;
				if ((p = hook_gl_proc(plain)) != NULL) {
					return p;
				}
			}
		}
	}
	return NULL;
}

static void
eagl_resolve_missing(const char *name)
{
	static unsigned said;

	if (said++ < 400) {
		say("s8gl: the library has no %s\n", name);
	}
}

static void
eagl_enter(struct eagl *e, unsigned offset)
{
	if (current != e) {
		if (!hook_gl_make_current(e->gl)) {
			say("s8gl: the context cannot be made this thread's\n");
		}
		current = e;
	}
	if (slot_calls[offset / 8]++ == 0 && offset != 0) {
		/* the first call of every entry: this is the list of what CoreAnimation really uses */
		say("s8gl: first call of table entry %#x %s (%s by QuartzCore's list) [thread %p]\n", offset,
		    eagl_slot_name(offset), eagl_slot_expected(offset) ? "expected" : "NOT expected", (void *)pthread_self());
	}
	e->calls++;
}

static void
eagl_leave(struct eagl *e, unsigned offset)
{
	GLenum error;

	if (check_errors && offset != 0x338 && glGetError != NULL && (error = glGetError()) != 0) {
		if (e->errors++ < 60) {
			say("s8gl: %s: error %#x\n", eagl_slot_name(offset), error);
		}
	}
}

/* An entry the library has no function for: say so, once an entry. */
static long
eagl_missing(void *context, unsigned offset)
{
	struct eagl *e = context;

	eagl_enter(e, offset);
	if (slot_calls[offset / 8] == 1) {
		say("s8gl: CALLED AND NOT THERE: entry %#x, %s\n", offset, eagl_slot_name(offset));
	}
	return 0;
}

static struct eagl *
context_of(id object)
{
	for (unsigned i = 0; i < NCONTEXTS; i++) {
		if (contexts[i] != NULL && contexts[i]->object == object) {
			return contexts[i];
		}
	}
	return NULL;
}

/* ---- what CoreAnimation asks for: kept for the summary ------------------------------------------- */

struct format_use {
	GLenum internal, format, type;
	unsigned count, widest, tallest;
	const char *how;
};
static struct format_use formats[48];
static unsigned widest_texture, tallest_texture;

static void
note_format(const char *how, GLenum internal, GLenum format, GLenum type, GLsizei width, GLsizei height)
{
	struct format_use *use = NULL;

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].count == 0 || (formats[i].how == how && formats[i].internal == internal &&
		    formats[i].format == format && formats[i].type == type)) {
			use = &formats[i];
			break;
		}
	}
	if ((unsigned)width > widest_texture) {
		widest_texture = (unsigned)width;
	}
	if ((unsigned)height > tallest_texture) {
		tallest_texture = (unsigned)height;
	}
	if (use == NULL) {
		return;
	}
	if (use->count++ == 0) {
		use->how = how;
		use->internal = internal;
		use->format = format;
		use->type = type;
		say("s8gl: a new kind of texture: %s, internal format %#x, format %#x, type %#x, %d x %d\n", how, internal, format,
		    type, width, height);
	}
	if ((unsigned)width > use->widest) {
		use->widest = (unsigned)width;
	}
	if ((unsigned)height > use->tallest) {
		use->tallest = (unsigned)height;
	}
}

static void
summary(struct eagl *e)
{
	unsigned frames = e->frames ? e->frames : 1;

	say("s8gl: after %u frames: %llu calls, %llu draws and %llu triangles a frame on average; the most in one: "
	    "%u calls, %u triangles; %llu us a frame, of which %llu handing the commands on, %llu waiting for the picture "
	    "to be drawn and %llu copying it out, and before each %llu making textures of IOSurfaces; %u IOSurfaces were "
	    "the textures' memory, %u had to be copied; %u shaders, %u programs; biggest texture %u x %u; %u errors; "
	    "%u draws with no program drawn, %u left out\n",
	    e->frames, (unsigned long long)(e->all_calls / frames), (unsigned long long)(e->all_draws / frames),
	    (unsigned long long)(e->all_triangles / frames), e->most_calls, e->most_triangles,
	    (unsigned long long)(e->frame_us / frames), (unsigned long long)(e->flush_us / frames),
	    (unsigned long long)(e->wait_us / frames), (unsigned long long)(e->readback_us / frames),
	    (unsigned long long)(e->attach_us / frames), e->shared, e->refused, e->shaders, e->programs, widest_texture,
	    tallest_texture, e->errors, e->plain_drawn, e->plain_left_out);
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]) && formats[i].count; i++) {
		say("s8gl:   textures: %s %#x/%#x/%#x: %u times, up to %u x %u\n", formats[i].how, formats[i].internal,
		    formats[i].format, formats[i].type, formats[i].count, formats[i].widest, formats[i].tallest);
	}
	for (unsigned offset = 8; offset < SLOTS * 8; offset += 8) {
		if (slot_calls[offset / 8]) {
			say("s8gl:   %-28s %u\n", eagl_slot_name(offset), slot_calls[offset / 8]);
		}
	}
}

/* ---- IOSurfaces ----------------------------------------------------------------------------------- */

/*
 * The textures that stand for IOSurfaces (S9+ eagl.c 556-863 with its reasoning: CoreAnimation keeps a texture for
 * every layer's surface it has drawn recently, about a thousand in a minute of use). The table grows (doubling, from
 * IMAGES_FIRST up to IMAGES_MOST) and is found by texture name in a hash (image_slot(): glBindTexture looks every bind
 * up). What it holds alive is each surface (CFRetain). Past IMAGES_MOST entries or IMAGE_BYTES of surfaces it lets go
 * of one first (image_evict()): one whose surface nobody else holds any more, else the one bound least recently that is
 * not being drawn with or into. An entry goes when its texture is deleted, when the texture is given other pixels
 * (image_respecified()), and, every IMAGE_SWEEP_US, when nobody but the table holds its surface any more.
 */
#define IMAGES_FIRST    256
#define IMAGES_MOST     4096
#define IMAGE_BYTES     (512ull << 20)  /* TODO: the S8's RAM is smaller than the S9's 4 GB: check the real budget on the phone */
#define IMAGE_SWEEP_US  5000000ull
#define INDEX_EMPTY     (-1)
#define INDEX_GONE      (-2)

static uint64_t image_bytes_most = IMAGE_BYTES;        /* glsurfaces_mb=N */
static unsigned images_most = IMAGES_MOST;              /* glsurfaces=N (at least 16) */

/* Where texture `texture` is in the index, or where it would go (an empty entry; or -1: the index is full). */
static int
image_slot(const struct eagl *e, GLuint texture, bool *found)
{
	unsigned mask = e->index_room - 1, at = (texture * 2654435761u) & mask;
	int first_gone = -1;

	*found = false;
	if (e->index_room == 0) {
		return -1;
	}
	for (unsigned probes = 0; probes < e->index_room; probes++, at = (at + 1) & mask) {
		int32_t i = e->image_index[at];

		if (i == INDEX_EMPTY) {
			return first_gone >= 0 ? first_gone : (int)at;
		}
		if (i == INDEX_GONE) {
			if (first_gone < 0) {
				first_gone = (int)at;
			}
		} else if (e->images[i].texture == texture) {
			*found = true;
			return (int)at;
		}
	}
	return first_gone;
}

/* The index made again, `room` entries (a power of two, at least twice the table's), without tombstones. */
static bool
image_index_make(struct eagl *e, unsigned room)
{
	int32_t *index = malloc(room * sizeof(*index));
	bool found;

	if (index == NULL) {
		return false;
	}
	for (unsigned at = 0; at < room; at++) {
		index[at] = INDEX_EMPTY;
	}
	free(e->image_index);
	e->image_index = index;
	e->index_room = room;
	e->index_taken = e->image_count;
	for (unsigned i = 0; i < e->image_count; i++) {
		index[image_slot(e, e->images[i].texture, &found)] = (int32_t)i;
	}
	return true;
}

static struct image *
image_of(struct eagl *e, GLuint texture)
{
	bool found;
	int at;

	if (e->image_count == 0) {
		return NULL;
	}
	at = image_slot(e, texture, &found);
	return found ? &e->images[e->image_index[at]] : NULL;
}

/* Let go of an entry: its texture stays CoreAnimation's, with what it holds. */
static void
image_remove(struct eagl *e, struct image *image)
{
	unsigned i = (unsigned)(image - e->images), last = e->image_count - 1;
	bool found;
	int at;

	e->image_bytes -= image->bytes;
	CFRelease(image->surface);
	at = image_slot(e, image->texture, &found);
	e->image_index[at] = INDEX_GONE;
	if (i != last) {
		e->images[i] = e->images[last];
		at = image_slot(e, e->images[i].texture, &found);
		e->image_index[at] = (int32_t)i;
	}
	e->image_count--;
}

static void
image_forget(struct eagl *e, GLuint texture)
{
	struct image *image = image_of(e, texture);

	if (image != NULL) {
		image_remove(e, image);
		e->image_counts.deleted++;
	}
}

/* Nobody but the table holds the surface: whoever showed it has let it go. */
static bool
image_orphaned(const struct image *image)
{
	return CFGetRetainCount(image->surface) <= 1;
}

/* An entry that may be let go of now: not the picture being drawn into, nor bound, nor used this frame. */
static bool
image_idle(const struct eagl *e, const struct image *image)
{
	if (image->texture == e->colour || image->used == e->frames + 1) {
		return false;
	}
	for (unsigned unit = 0; unit < sizeof(e->bound) / sizeof(e->bound[0]); unit++) {
		if (e->bound[unit] == image->texture) {
			return false;
		}
	}
	return true;
}

/*
 * Make room by letting one entry go: one whose surface only the table holds, else the least recently used one that is
 * shown (not drawn into), else a picture drawn into that has not been used for 60 frames. False: none may go.
 */
static bool
image_evict(struct eagl *e)
{
	struct image *best = NULL;
	unsigned best_rank = 3;

	for (unsigned i = 0; i < e->image_count; i++) {
		struct image *image = &e->images[i];
		unsigned rank;

		if (!image_idle(e, image)) {
			continue;
		}
		rank = image_orphaned(image) ? 0 : !image->target ? 1 : e->frames - image->used >= 60 ? 2 : 3;
		if (rank < best_rank || (rank == best_rank && best != NULL && image->used < best->used)) {
			best = image;
			best_rank = rank;
		}
	}
	if (best == NULL || best_rank == 3) {
		return false;
	}
	if (best_rank == 0) {
		e->image_counts.released++;
	} else {
		e->image_counts.evicted++;
	}
	image_remove(e, best);
	return true;
}

/* Room for one more entry, of `bytes` more of surfaces: grown, or made by letting another go. */
static bool
image_room(struct eagl *e, size_t bytes)
{
	while (e->image_count != 0 && (e->image_count >= images_most || e->image_bytes + bytes > image_bytes_most)) {
		if (!image_evict(e)) {
			return false;
		}
	}
	if (e->image_count == e->image_room) {
		unsigned room = e->image_room == 0 ? IMAGES_FIRST : 2 * e->image_room;
		struct image *images;

		if (room > images_most) {
			room = images_most;
		}
		if (room <= e->image_room || (images = realloc(e->images, room * sizeof(*images))) == NULL) {
			return false;
		}
		e->images = images;
		e->image_room = room;
	}
	/* the index at most half taken, tombstones included */
	if (2 * (e->index_taken + 1) > e->index_room) {
		unsigned room = 2 * IMAGES_FIRST;

		while (room < 4 * e->image_room) {
			room *= 2;
		}
		if (!image_index_make(e, room)) {
			return false;
		}
	}
	return true;
}

static struct image *
image_new(struct eagl *e, GLuint texture, IOSurfaceRef surface)
{
	struct image *image = image_of(e, texture);
	size_t bytes = IOSurfaceGetAllocSize(surface);
	bool found;
	int at;

	if (image != NULL) {
		image_remove(e, image);         /* the same texture, another surface (or the same one again): made anew */
	}
	if (!image_room(e, bytes)) {
		say("s8gl: more textures with IOSurfaces than there is room for (%u, %llu MB of surfaces), "
		    "and none can be let go of: texture %u is copied once\n", e->image_count,
		    (unsigned long long)(e->image_bytes >> 20), texture);
		return NULL;
	}
	at = image_slot(e, texture, &found);
	if (e->image_index[at] == INDEX_EMPTY) {
		e->index_taken++;
	}
	e->image_index[at] = (int32_t)e->image_count;
	image = &e->images[e->image_count++];
	e->image_counts.made++;
	if (e->image_count > e->image_most) {
		e->image_most = e->image_count;
	}
	memset(image, 0, sizeof(*image));
	image->texture = texture;
	image->surface = (IOSurfaceRef)CFRetain(surface);
	image->bytes = bytes;
	image->used = e->frames + 1;
	e->image_bytes += bytes;
	return image;
}

/* The texture bound to `target` on this unit is given other pixels: it no longer stands for its surface. */
static void
image_respecified(struct eagl *e, GLenum target, GLint level)
{
	struct image *image;

	if (target == GL_TEXTURE_2D && level == 0 && e->unit < sizeof(e->bound) / sizeof(e->bound[0]) &&
	    e->bound[e->unit] != 0 && (image = image_of(e, e->bound[e->unit])) != NULL) {
		image_remove(e, image);
		e->image_counts.respecified++;
	}
}

/*
 * Every IMAGE_SWEEP_US, at a frame's end: the entries whose surface only the table holds let go of, and a line of what
 * the table has done since the last one, when it has done anything.
 */
static void
image_report(struct eagl *e)
{
	uint64_t now = now_us(), span;
	struct image_counts *c = &e->image_counts;

	if (now - e->image_said < IMAGE_SWEEP_US) {
		return;
	}
	for (unsigned i = 0; i < e->image_count;) {
		if (image_orphaned(&e->images[i]) && image_idle(e, &e->images[i])) {
			image_remove(e, &e->images[i]);         /* (the last one is moved here: look at i again) */
			c->released++;
		} else {
			i++;
		}
	}
	span = e->image_said == 0 ? IMAGE_SWEEP_US : now - e->image_said;
	e->image_said = now;
	if (c->made == 0 && c->deleted == 0 && c->released == 0 && c->respecified == 0 && c->evicted == 0 &&
	    c->untracked == 0 && c->refills == 0) {
		return;
	}
	say("s8gl: IOSurface textures: %u in use (at most %u, room for %u of %u), %llu MB of surfaces held (of %llu); "
	    "in %llu s: %u new, %u deleted, %u let go (surface released), %u given other pixels, %u evicted, "
	    "%u copied once (no room); %u refilled (%llu KB)\n", e->image_count, e->image_most, e->image_room, images_most,
	    (unsigned long long)(e->image_bytes >> 20), (unsigned long long)(image_bytes_most >> 20),
	    (unsigned long long)((span + 500000) / 1000000), c->made, c->deleted, c->released, c->respecified, c->evicted,
	    c->untracked, c->refills, (unsigned long long)(c->refill_bytes >> 10));
	memset(c, 0, sizeof(*c));
}

/* Apple's OpenGL ES takes blue-first pixels into a texture called GL_RGBA; everybody else's calls the texture so too. */
static GLenum
internal_format(GLenum internal, GLenum format)
{
	return format == GL_BGRA && internal == GL_RGBA ? GL_BGRA : internal;
}

static unsigned
bytes_a_pixel(GLenum format, GLenum type)
{
	unsigned parts = format == GL_RGBA || format == GL_BGRA ? 4 : format == 0x1907 ? 3 :        /* RGB */
	    format == 0x8227 || format == 0x190a ? 2 : 1;                                           /* RG, luminance + alpha */

	switch (type) {
	case GL_UNSIGNED_BYTE:
		return parts;
	case 0x140b: case 0x8d61: case 0x1403:          /* half floats, 16 bits */
		return 2 * parts;
	case 0x1406:                                    /* floats */
		return 4 * parts;
	case 0x8368:                                    /* 2:10:10:10 in a word */
		return 4;
	default:
		return 2;                               /* the packed 16-bit kinds */
	}
}

static const char *
fourcc(uint32_t format)
{
	static __thread char text[4][5];
	static __thread unsigned which;
	char *t = text[which++ % 4];

	for (int i = 0; i < 4; i++) {
		unsigned char c = (unsigned char)(format >> (24 - 8 * i));

		t[i] = c >= 32 && c < 127 ? (char)c : '.';
	}
	t[4] = 0;
	return t;
}

/*
 * Make a texture, which is bound, of its IOSurface's own memory, if that is what is asked for (the whole of a surface of
 * blue-first pixels, the same way up) and the library can do it (hook_gl_surface_texture(): today it cannot).
 */
static bool
surface_shared(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;

	if (!share_surfaces || image->invert || image->alpha_first || image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE ||
	    image->plane != 0 || (size_t)image->width != IOSurfaceGetWidth(surface) ||
	    (size_t)image->height != IOSurfaceGetHeight(surface) ||
	    !hook_gl_surface_texture(e->gl, IOSurfaceGetID(surface))) {
		/* why a surface is copied rather than its memory used: the first ones of a run, for the copies to be made fewer */
		if (e->refused < 20) {
			say("s8gl: surface %u ('%s' %zu x %zu) copied, not shared: %s%s%s%s%s%s\n", IOSurfaceGetID(surface),
			    fourcc(IOSurfaceGetPixelFormat(surface)), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
			    !share_surfaces ? "sharing off; " : "", image->invert ? "turned over; " : "",
			    image->alpha_first ? "alpha first; " : "",
			    image->format != GL_BGRA || image->type != GL_UNSIGNED_BYTE ? "not BGRA bytes; " : "",
			    image->plane != 0 ? "a plane; " : "",
			    (size_t)image->width != IOSurfaceGetWidth(surface) || (size_t)image->height != IOSurfaceGetHeight(surface) ?
			    "a part of it; " : "(the library refused or sharing is off)");
		}
		e->refused++;
		return false;
	}
	image->shared = true;
	e->shared++;
	return true;
}

/*
 * A surface that is shown goes on being drawn into after the texture has been made of it, and not everybody who draws says
 * so: often no seed moves. A GPU that has the memory itself shows what is there. A copy has to notice: once a frame, when
 * the texture is about to be drawn with, the memory is added up, and copied again if the sum has changed. One that has been
 * the same STILL_LOOKS looks in a row is looked at every STILL_EVERY frames only (S9+ eagl.c 954-1010).
 */
#define STILL_LOOKS     120
#define STILL_EVERY     8
static bool
surface_changed(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t size = IOSurfaceGetBytesPerRow(surface) * (size_t)(image->height - 1) +
	    (size_t)image->width * bytes_a_pixel(image->format, image->type);
	const uint8_t *base = IOSurfaceGetBaseAddress(surface);
	uint64_t sum = size, word;

	if (image->looked == e->frames + 1 || base == NULL || IOSurfaceGetPlaneCount(surface) > 0) {
		return false;
	}
	if (image->unchanged >= STILL_LOOKS && (e->frames + IOSurfaceGetID(surface)) % STILL_EVERY != 0) {
		return false;
	}
	image->looked = e->frames + 1;
	for (size_t at = 0; at + 8 <= size; at += 8) {
		memcpy(&word, base + at, 8);
		sum = (sum ^ word) * 0x9e3779b97f4a7c15ull;
		sum ^= sum >> 29;
	}
	if (sum == image->sum) {
		image->unchanged += image->unchanged < STILL_LOOKS;
		return false;
	}
	image->sum = sum;
	image->unchanged = 0;
	return true;
}

/* Fill a texture, which is bound, from its IOSurface, unless it is the surface. */
static void
surface_to_texture(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	size_t row = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, image->plane) : IOSurfaceGetBytesPerRow(surface);
	unsigned pixel = bytes_a_pixel(image->format, image->type);
	GLint row_length = 0, alignment = 4;
	uint8_t *base, *flipped = NULL;
	bool alpha_first = image->alpha_first && !planar && pixel == 4;
	uint32_t seed = 0;

	if (image->shared) {
		return;
	}
	/*
	 * The seed is the one the lock sees, before the pixels are read: taken after the copy, a write between the two (a
	 * label's text drawn into its layer while it was being copied) moved it unseen, and the texture kept the old pixels.
	 */
	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &seed) != 0) {
		say("s8gl: an IOSurface cannot be locked\n");
		return;
	}
	base = planar ? IOSurfaceGetBaseAddressOfPlane(surface, image->plane) : IOSurfaceGetBaseAddress(surface);
	if (base == NULL) {
		say("s8gl: surface %u has no base address\n", IOSurfaceGetID(surface));
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		return;
	}
	if ((image->invert || alpha_first) && (flipped = calloc(row, (size_t)image->height)) != NULL) {
		for (GLsizei y = 0; y < image->height; y++) {
			const uint8_t *from = base + row * (size_t)(image->invert ? image->height - 1 - y : y);
			uint8_t *to = flipped + row * (size_t)y;
			size_t used = (size_t)image->width * pixel;     /* (the last row may end where its pixels do) */

			if (!alpha_first) {
				memcpy(to, from, used < row ? used : row);
				continue;
			}
			/* opacity, blue, green, red in memory: into the order OpenGL ES was told */
			for (size_t at = 0; at + 4 <= used && at + 4 <= row; at += 4) {
				to[at] = from[at + (image->format == GL_BGRA ? 1 : 3)];
				to[at + 1] = from[at + 2];
				to[at + 2] = from[at + (image->format == GL_BGRA ? 3 : 1)];
				to[at + 3] = from[at];
			}
		}
		base = flipped;
	}
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(row / pixel));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(image->to, 0, (GLint)internal_format(image->internal, image->format), image->width, image->height, 0,
	    image->format, image->type, base);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	free(flipped);
	image->seed = seed;
	e->uploads++;
	e->upload_bytes += row * (size_t)image->height;
}

/* What has been drawn into a texture, into its IOSurface, unless it is there (frame_end() waits for it). */
static void
target_to_surface(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t row = IOSurfaceGetBytesPerRow(surface);
	uint8_t *base;

	image->drawn = false;
	if (image->shared) {
		/* whoever looks at the surface's seed is to see that it has changed */
		if (IOSurfaceLock(surface, 0, NULL) == 0) {
			IOSurfaceUnlock(surface, 0, NULL);
		}
		return;
	}
	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
	if (IOSurfaceLock(surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(surface);
		glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(row / 4));
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		if (!image->invert) {
			glReadPixels(0, 0, image->width, image->height, GL_BGRA, GL_UNSIGNED_BYTE, base);
		} else {
			for (GLsizei y = 0; y < image->height; y++) {
				glReadPixels(0, y, image->width, 1, GL_BGRA, GL_UNSIGNED_BYTE, base + row * (size_t)(image->height - 1 - y));
			}
		}
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
}

/* Before the first drawing of a frame: the textures whose IOSurfaces somebody has written to since. */
static void
frame_begin(struct eagl *e)
{
	GLint bound = 0;
	bool rebound = false;

	if (e->in_frame) {
		return;
	}
	e->in_frame = true;
	e->began = now_us();
	eagl_tracing = e->frames + 1 >= trace_first && e->frames + 1 <= trace_last;
	if (eagl_tracing) {
		eagl_trace("---- frame %u (framebuffer %u)\n", e->frames + 1, e->framebuffer);
	}
	for (unsigned i = 0; i < e->image_count; i++) {
		struct image *image = &e->images[i];

		if (image->target || image->shared || IOSurfaceGetSeed(image->surface) == image->seed) {
			continue;
		}
		if (!rebound) {
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			rebound = true;
		}
		glBindTexture(image->to, image->texture);
		surface_to_texture(e, image);
		e->image_counts.refills++;
		e->image_counts.refill_bytes += IOSurfaceGetBytesPerRow(image->surface) * (size_t)image->height;
	}
	if (rebound) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	}
}

/*
 * glFlush, glFinish: what was drawn must be where the display takes it from. `finished`: the library has been waited for
 * already.
 */
static void
frame_end(struct eagl *e, bool finished)
{
	static unsigned unfollowed;
	uint64_t before, waited, after;
	unsigned copied = 0, in_place = 0;

	before = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn && e->images[i].shared) {
			in_place++;
		}
	}
	if (in_place != 0 && !finished && wait_for_frames) {
		/* the display's driver reads the surface as soon as it is told of it: the GPU's jobs must be done */
		for (unsigned i = 0; i < e->image_count; i++) {
			if (e->images[i].target && e->images[i].drawn && e->images[i].shared &&
			    !hook_gl_surface_wait(e->gl, IOSurfaceGetID(e->images[i].surface))) {
				glFinish();
				break;
			}
		}
	}
	waited = now_us();
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn) {
			copied += !e->images[i].shared;
			target_to_surface(e, &e->images[i]);
		}
	}
	if (!e->in_frame) {
		return;
	}
	after = now_us();
	e->in_frame = false;
	e->frames++;
	if (frame_lines) {
		say("s8gl: f %u at %llu.%03llu s: %u draws, %u clears, %u GL calls; %u in place, %u copied out\n", e->frames,
		    (unsigned long long)(after / 1000000), (unsigned long long)(after / 1000 % 1000), e->draws, e->clears,
		    e->calls, in_place, copied);
	}
	if (in_place == 0 && copied == 0 && e->draws != 0 && unfollowed < 8) {
		/* (whatever it was drawn into is shown as it is, drawn or not: this is to be seen in the log) */
		unfollowed++;
		say("s8gl: frame %u: %u draws, and none into an IOSurface that is known here (framebuffer %u, texture %u)\n",
		    e->frames, e->draws, e->framebuffer, e->colour);
	}
	e->all_calls += e->calls;
	e->all_draws += e->draws;
	e->all_triangles += e->triangles;
	e->wait_us += waited - before;
	e->readback_us += after - waited;
	e->attach_us += e->attaching;
	e->flush_us += e->flushing;
	e->frame_us += after - e->began;
	if (e->calls > e->most_calls) {
		e->most_calls = e->calls;
	}
	if (e->triangles > e->most_triangles) {
		e->most_triangles = e->triangles;
	}
	if (e->frames <= 40 || e->frames % 20 == 0) {
		char kernel_calls[400];

		(void)s8mali_stats(kernel_calls, sizeof(kernel_calls));
		say("s8gl: kernel calls this frame: %s\n", kernel_calls[0] != 0 ? kernel_calls : "none");
		say("s8gl: frame %u: %u GL calls, %u draws, %u triangles, %u clears; %u textures refilled from IOSurfaces "
		    "(%llu KB); %u picture%s copied out, %u drawn in place; %llu us (%llu handing on, %llu waiting, %llu "
		    "copying out), %llu before it for the IOSurfaces\n", e->frames, e->calls, e->draws, e->triangles, e->clears,
		    e->uploads, (unsigned long long)(e->upload_bytes >> 10), copied, copied == 1 ? "" : "s", in_place,
		    (unsigned long long)(after - e->began), (unsigned long long)e->flushing,
		    (unsigned long long)(waited - before), (unsigned long long)(after - waited),
		    (unsigned long long)e->attaching);
	}
	if (e->frames == 30 || e->frames % 200 == 0) {
		summary(e);
	}
	image_report(e);
	/* each check is a round trip: they stop after the first 600 frames of any context, errors or not */
	if (e->frames == 600 && check_errors && !keep_checking) {
		check_errors = 0;
		say("s8gl: after 600 frames, %u errors; no longer asked for after every call\n", e->errors);
	}
	e->calls = e->draws = e->triangles = e->uploads = e->clears = 0;
	e->upload_bytes = e->attaching = e->flushing = 0;
}

static unsigned
triangles_of(GLenum mode, GLsizei count)
{
	return mode == GL_TRIANGLES ? (unsigned)count / 3 : (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) &&
	       count > 2 ? (unsigned)count - 2 : 0;
}

/* Something is about to be drawn into the framebuffer that is bound. */
static void
drawing(struct eagl *e)
{
	struct image *image;

	frame_begin(e);
	if (e->framebuffer != 0 && e->colour != 0 && (image = image_of(e, e->colour)) != NULL && image->target) {
		image->drawn = image->painted = true;
	}
}

/* ---- draws with no program ---------------------------------------------------------------------------- */

/*
 * CoreAnimation's OpenGL ES renderer draws some of its ninety "functions" with no program at all: glUseProgram(0), then
 * the draw. In OpenGL ES that draws nothing; what goes that way is missing from the picture (the S9+ port: the lock
 * screen's wallpaper and clock). Here such a draw is made with a program of ours, the plainest there could be:
 * CoreAnimation's own vertex arrays (position 0, colour 1, texture coordinates 2, the latter in the texture's pixels), the
 * matrix it gives every program of its own, and the colour times the texture on the first unit; one with a one-row texture
 * on the second unit is a lookup table (the home indicator). Function-specific programs are NOT ported: see the TODO at
 * the top of this file.
 */
static const char plain_vertex[] =
    "#version 300 es\n"
    "uniform highp mat4 vertex_matrix;\n"
    "in highp vec4 vertex_position;\n"
    "in lowp vec4 vertex_color;\n"
    "in highp vec2 vertex_texcoord0;\n"
    "out lowp vec4 color;\n"
    "out highp vec2 texcoord;\n"
    "void main() {\n"
    "  gl_Position = vertex_matrix * vertex_position;\n"
    "  color = vertex_color;\n"
    "  texcoord = vertex_texcoord0;\n"
    "}\n";
static const char plain_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp float textured;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "void main() {\n"
    "  result = textured > 0.5 ? color * texture(texture0, texcoord / vec2(textureSize(texture0, 0))) : color;\n"
    "}\n";
static const char table_fragment[] =
    "#version 300 es\n"
    "uniform lowp sampler2D texture0;\n"
    "uniform lowp sampler2D table;\n"
    "in lowp vec4 color;\n"
    "in highp vec2 texcoord;\n"
    "out lowp vec4 result;\n"
    "const mediump float white = .284, amount = 1., overlay = .31;\n"
    "void main() {\n"
    "  mediump vec4 behind = texture(texture0, texcoord / vec2(textureSize(texture0, 0)));\n"
    "  mediump vec4 entry = texture(table, vec2(dot(behind.rgb, vec3(.2125, .7154, .0721)), .5));\n"
    "  mediump vec4 bar = clamp(entry + vec4(vec3(white), 1.), 0., 1.) * amount;\n"
    "  mediump vec4 under = behind * (1. - bar.a) + bar;\n"
    "  mediump vec4 over = under * overlay;\n"
    "  mediump vec3 dark = 2. * under.rgb * over.rgb;\n"
    "  mediump vec3 light = over.a * under.a - 2. * (under.a - under.rgb) * (over.a - over.rgb);\n"
    "  mediump vec3 both = mix(dark, light, step(under.a * .5, under.rgb));\n"
    "  mediump float opacity = over.a + under.a * (1. - over.a);\n"
    "  result = vec4(clamp(both + over.rgb * (1. - under.a) + under.rgb * (1. - over.a), 0., opacity), opacity);\n"
    "}\n";

/* One of our programs, made: CoreAnimation's own vertex arrays go in at the places it has them. */
static GLuint
plain_program(const char *fragment, const char *what)
{
	GLuint program = glCreateProgram(), shader;
	const GLchar *text;
	GLint good = 0;

	shader = glCreateShader(GL_VERTEX_SHADER);
	text = plain_vertex;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	shader = glCreateShader(GL_FRAGMENT_SHADER);
	text = fragment;
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glAttachShader(program, shader);
	glBindAttribLocation(program, 0, "vertex_position");
	glBindAttribLocation(program, 1, "vertex_color");
	glBindAttribLocation(program, 2, "vertex_texcoord0");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	say("s8gl: a program of ours for the draws CoreAnimation makes with none (%s): %s\n", what, good ? "made" : "NOT MADE");
	return program;
}

static bool
plain_draw_begin(struct eagl *e, GLint enabled[4])
{
	static const GLfloat identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	GLint rows = 0;
	GLuint program;

	if (e->program != 0) {
		return false;
	}
	if (!plain_draws || (plain_draws == 1 && e->bound[0] == 0) || (e->colour != 0 && e->colour == e->bound[0])) {
		if (e->plain_left_out++ < 8) {
			say("s8gl: a draw with no program is left out (glplain %d, texture %u on the first unit, framebuffer %u)\n",
			    plain_draws, e->bound[0], e->framebuffer);
		}
		return false;
	}
	if (e->bound[0] != 0 && e->bound[1] != 0) {
		glActiveTexture(GL_TEXTURE0 + 1);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &rows);
		glActiveTexture(GL_TEXTURE0 + e->unit);
	}
	if (rows == 1) {
		if (e->lookup == 0) {
			e->lookup = plain_program(table_fragment, "through a table");
			e->lookup_matrix = glGetUniformLocation(e->lookup, "vertex_matrix");
			glUseProgram(e->lookup);
			glUniform1i(glGetUniformLocation(e->lookup, "table"), 1);
		}
		program = e->lookup;
	} else {
		if (e->plain == 0) {
			e->plain = plain_program(plain_fragment, "as they are");
			e->plain_matrix = glGetUniformLocation(e->plain, "vertex_matrix");
			e->plain_textured = glGetUniformLocation(e->plain, "textured");
		}
		program = e->plain;
	}
	glUseProgram(program);
	glUniformMatrix4fv(program == e->plain ? e->plain_matrix : e->lookup_matrix, 1, 0, e->matrix != NULL ? e->matrix : identity);
	if (program == e->plain) {
		glUniform1f(e->plain_textured, e->bound[0] != 0 ? 1 : 0);
	}
	for (GLuint i = 0; i < 3; i++) {
		glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled[i]);
		if (!enabled[i]) {
			glEnableVertexAttribArray(i);
		}
	}
	e->plain_drawn++;
	if (eagl_tracing || e->plain_said < 4) {
		const GLfloat *position = e->arrays[0].pointer, *where = e->arrays[2].pointer;

		e->plain_said++;
		say("s8gl: a draw with no program, into framebuffer %u: texture %u on the first unit%s; arrays %d %d %d; first "
		    "corner %g %g, texture coordinates %g %g\n", e->framebuffer, e->bound[0], rows == 1 ? ", a table on the second" : "",
		    enabled[0], enabled[1], enabled[2], position ? (double)position[0] : 0, position ? (double)position[1] : 0,
		    where ? (double)where[0] : 0, where ? (double)where[1] : 0);
	}
	return true;
}

static void
plain_draw_end(struct eagl *e, const GLint enabled[4])
{
	(void)e;
	for (GLuint i = 0; i < 4; i++) {
		if (!enabled[i]) {
			glDisableVertexAttribArray(i);
		}
	}
	glUseProgram(0);
}

/* ---- the entries written by hand ---------------------------------------------------------------- */

static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
	struct eagl *e = context;
	struct image *image;

	EAGL_ENTER(e, 0x30);
	if (eagl_tracing) {
		eagl_trace("glBindTexture(%#x, %u)\n", target, texture);
	}
	glBindTexture(target, texture);
	if (target == GL_TEXTURE_2D && e->unit < 8) {
		e->bound[e->unit] = texture;
	}
	if (texture != 0 && e->image_count != 0 && (image = image_of(e, texture)) != NULL) {
		image->used = e->frames + 1;
		if (!image->shared && (!image->target ? IOSurfaceGetSeed(image->surface) != image->seed ||
		    surface_changed(e, image) : image->wrapped && !image->painted && surface_changed(e, image))) {
			surface_to_texture(e, image);
			e->image_counts.refills++;
			e->image_counts.refill_bytes += IOSurfaceGetBytesPerRow(image->surface) * (size_t)image->height;
		}
	}
	EAGL_LEAVE(e, 0x30);
}

static void
my_glClear(void *context, GLbitfield mask)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x58);
	drawing(e);
	e->clears++;
	if (eagl_tracing) {
		eagl_trace("glClear(%#x)\n", mask);
	}
	glClear(mask);
	EAGL_LEAVE(e, 0x58);
}

static void
my_glUseProgram(void *context, GLuint program)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x12c8);
	if (eagl_tracing) {
		eagl_trace("glUseProgram(%u)\n", program);
	}
	e->program = program;
	glUseProgram(program);
	EAGL_LEAVE(e, 0x12c8);
}

static void
my_glActiveTexture(void *context, GLenum unit)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0xab8);
	if (eagl_tracing) {
		eagl_trace("glActiveTexture(%#x)\n", unit);
	}
	e->unit = unit - GL_TEXTURE0;
	glActiveTexture(unit);
	EAGL_LEAVE(e, 0xab8);
}

static void
my_glUniformMatrix4fv(void *context, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1368);
	if (eagl_tracing) {
		eagl_trace("glUniformMatrix4fv(%d, %d, %d, %p)\n", location, count, transpose, (const void *)value);
		eagl_trace_floats(value, 16);
	}
	e->matrix = value;
	glUniformMatrix4fv(location, count, transpose, value);
	EAGL_LEAVE(e, 0x1368);
}

static void
my_glVertexAttribPointer(void *context, GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
    const void *pointer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1000);
	if (eagl_tracing) {
		eagl_trace("glVertexAttribPointer(%u, %d, %#x, %d, %d, %p)\n", index, size, type, normalized, stride, pointer);
	}
	if (index < 8) {
		e->arrays[index].pointer = pointer;
		e->arrays[index].stride = stride;
		e->arrays[index].size = size;
		e->arrays[index].type = type;
	}
	glVertexAttribPointer(index, size, type, normalized, stride, pointer);
	EAGL_LEAVE(e, 0x1000);
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x210);
	drawing(e);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawArrays(%#x, %d, %d) program %u framebuffer %u\n", mode, first, count, e->program, e->framebuffer);
	}
	plain = plain_draw_begin(e, enabled);
	glDrawArrays(mode, first, count);
	if (plain) {
		plain_draw_end(e, enabled);
	}
	EAGL_LEAVE(e, 0x210);
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	struct eagl *e = context;
	GLint enabled[4] = { 1, 1, 1, 1 };
	bool plain;

	EAGL_ENTER(e, 0x220);
	drawing(e);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawElements(%#x, %d, %#x, %p) program %u framebuffer %u\n", mode, count, type, indices, e->program,
		    e->framebuffer);
	}
	plain = plain_draw_begin(e, enabled);
	glDrawElements(mode, count, type, indices);
	if (plain) {
		plain_draw_end(e, enabled);
	}
	EAGL_LEAVE(e, 0x220);
}

static void
my_glFinish(void *context)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2d0);
	if (eagl_tracing) {
		eagl_trace("glFinish()\n");
	}
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0x2d0);
}

static void
my_glFlush(void *context)
{
	struct eagl *e = context;
	uint64_t began;

	EAGL_ENTER(e, 0x2d8);
	if (eagl_tracing) {
		eagl_trace("glFlush()\n");
	}
	began = now_us();
	glFlush();
	e->flushing += now_us() - began;
	frame_end(e, false);
	EAGL_LEAVE(e, 0x2d8);
}

/* APPLE_fence: wait for one object to be done with. Everything is done with after glFinish. */
static void
my_glFinishObjectAPPLE(void *context, GLenum object, GLint name)
{
	struct eagl *e = context;

	(void)object;
	(void)name;
	EAGL_ENTER(e, 0xeb8);
	glFinish();
	frame_end(e, true);
	EAGL_LEAVE(e, 0xeb8);
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1e0);
	for (GLsizei i = 0; i < n; i++) {
		image_forget(e, textures[i]);
		if (textures[i] == e->colour) {
			e->colour = 0;          /* (a texture that is deleted leaves the framebuffer bound) */
		}
		for (unsigned unit = 0; unit < 8; unit++) {
			if (e->bound[unit] == textures[i]) {
				e->bound[unit] = 0;
			}
		}
	}
	glDeleteTextures(n, textures);
	EAGL_LEAVE(e, 0x1e0);
}

static const GLubyte *
my_glGetString(void *context, GLenum name)
{
	struct eagl *e = context;
	const GLubyte *string;
	static unsigned said;

	EAGL_ENTER(e, 0x3b0);
	/* 0x8a1e is no name of OpenGL ES's; Darwin's library answers it (the constructor of CA::OGL::GLESContext asks) */
	string = name == 0x8a1e ? (const GLubyte *)"" : glGetString(name);
	if (name == GL_EXTENSIONS && string != NULL && !fetch_offered) {
		static char without[8192];      /* (the list less that extension: see glfetch=0) */
		size_t used = 0;

		for (const char *word = (const char *)string; *word != 0;) {
			size_t length = strcspn(word, " ");

			if (strncmp(word, "GL_EXT_shader_framebuffer_fetch", 31) != 0 && used + length + 2 < sizeof(without)) {
				memcpy(without + used, word, length);
				used += length;
				without[used++] = ' ';
			}
			word += length + (word[length] == ' ');
		}
		without[used] = 0;
		string = (const GLubyte *)without;
	}
	if (said++ < 60) {
		/* the list of extensions is longer than a line of the log: in pieces */
		if (name == GL_EXTENSIONS && string != NULL) {
			size_t length = strlen((const char *)string);

			say("s8gl: asked for string %#x, %zu characters:\n", name, length);
			for (size_t at = 0; at < length; at += 700) {
				say("s8gl:  %.700s\n", (const char *)string + at);
			}
		} else {
			say("s8gl: asked for string %#x: %.900s\n", name, string ? (const char *)string : "(none)");
		}
	}
	EAGL_LEAVE(e, 0x3b0);
	return string;
}

static void
my_glGetIntegerv(void *context, GLenum name, GLint *values)
{
	struct eagl *e = context;
	static unsigned said;

	EAGL_ENTER(e, 0x348);
	glGetIntegerv(name, values);
	if (said < 60) {
		said++;
		say("s8gl: asked for number %#x: %d\n", name, values[0]);
	}
	EAGL_LEAVE(e, 0x348);
}

static void
my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x970);
	if (level == 0) {
		note_format(pixels ? "filled" : "empty", (GLenum)internal, format, type, width, height);
	}
	if (eagl_tracing) {
		eagl_trace("glTexImage2D(%#x, %d, %#x, %d, %d, %d, %#x, %#x, %p)\n", target, level, internal, width, height, border,
		    format, type, pixels);
	}
	image_respecified(e, target, level);
	glTexImage2D(target, level, (GLint)internal_format((GLenum)internal, format), width, height, border, format, type, pixels);
	EAGL_LEAVE(e, 0x970);
}

static void
my_glCompressedTexImage2D(void *context, GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
    GLint border, GLsizei size, const void *data)
{
	struct eagl *e = context;
	static unsigned told;

	EAGL_ENTER(e, 0xbe8);
	if (level == 0) {
		note_format("compressed", internal, 0, 0, width, height);
	}
	image_respecified(e, target, level);
	if (told < 40) {
		(void)glGetError();             /* so that what is left behind is this call's own */
	}
	glCompressedTexImage2D(target, level, internal, width, height, border, size, data);
	/*
	 * A format the driver has not got is refused here, and then the texture stays undefined (magenta, with some). The
	 * first of these are written down with what the call left behind (the errors of every call are only asked for over
	 * the first 600 frames). CoreAnimation sends ASTC 4x4 (0x93b0) for its pictures; whether Mesa's Panfrost takes it
	 * is one of the things this log is to tell.
	 */
	if (told < 40) {
		GLenum error = glGetError();

		told++;
		say("s8gl: a compressed picture into texture %u: internal format %#x, %d x %d, level %d, %d bytes%s\n",
		    e->bound[e->unit < 8 ? e->unit : 0], internal, width, height, level, size,
		    error == 0 ? "" : error == 0x500 ? " -- refused, no such format" : " -- refused");
	}
	EAGL_LEAVE(e, 0xbe8);
}

static void
my_glBindFramebuffer(void *context, GLenum target, GLuint framebuffer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1508);
	if (eagl_tracing) {
		eagl_trace("glBindFramebuffer(%#x, %u)\n", target, framebuffer);
	}
	glBindFramebuffer(target, framebuffer);
	if (target == GL_READ_FRAMEBUFFER) {
		EAGL_LEAVE(e, 0x1508);
		return;                 /* (not what is drawn into) */
	}
	/*
	 * What is drawn into from here on is asked of the library, not kept by the framebuffer's name: CoreAnimation makes a
	 * framebuffer for every frame and the names do not come back, so there is no end of them.
	 */
	e->framebuffer = framebuffer;
	e->colour = 0;
	if (framebuffer != 0) {
		GLint kind = 0, name = 0;

		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
		    &kind);
		if (kind == GL_TEXTURE) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
			e->colour = (GLuint)name;
		}
	}
	EAGL_LEAVE(e, 0x1508);
}

static void
my_glFramebufferTexture2D(void *context, GLenum target, GLenum attachment, GLenum to, GLuint texture, GLint level)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1530);
	if (attachment == GL_COLOR_ATTACHMENT0 && target != GL_READ_FRAMEBUFFER) {
		e->colour = texture;
	}
	if (eagl_tracing) {
		eagl_trace("glFramebufferTexture2D(%#x, %#x, %#x, %u, %d)\n", target, attachment, to, texture, level);
	}
	glFramebufferTexture2D(target, attachment, to, texture, level);
	EAGL_LEAVE(e, 0x1530);
}

static void
my_glDeleteFramebuffers(void *context, GLsizei n, const GLuint *framebuffers)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1510);
	for (GLsizei i = 0; i < n; i++) {
		if (framebuffers[i] == e->framebuffer) {
			e->framebuffer = e->colour = 0;
		}
	}
	glDeleteFramebuffers(n, framebuffers);
	EAGL_LEAVE(e, 0x1510);
}

/* A private entry of Darwin's library (CA::OGL::GLESContext::set_surface calls it); nobody else has it. */
static void
my_glFramebufferParameteriAPPLE(void *context, GLenum target, GLenum name, GLint value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2008);
	if (slot_calls[0x2008 / 8] <= 4) {
		say("s8gl: glFramebufferParameteriAPPLE(%#x, %#x, %d): nothing done\n", target, name, value);
	}
}

/* EXT_debug_marker: names for a debugger's list of calls. */
static void
my_glInsertEventMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1988);
}

static void
my_glPushGroupMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1990);
}

static void
my_glPopGroupMarkerEXT(void *context)
{
	EAGL_ENTER(context, 0x1998);
}

/*
 * CoreAnimation's shaders are in the first version of the shading language, with one thing of Apple's own in them:
 * sampler2DRect and texture2DRect(), a texture read at coordinates counted in its pixels. The language has no such thing,
 * and in its first version no way to make one, since a shader cannot ask how big a texture is. In version 3.00 it can.
 * So every shader is handed on as a 3.00 one: the words of the first version that 3.00 dropped are defined back, and
 * texture2DRect() divides by textureSize(). (All of a program's shaders must be of one version, so all are treated alike.)
 * With glprecision=1 (default) "lowp" and "mediump" are defined away (S9+: on an Adreno mediump is a 16-bit float and
 * CoreAnimation's gradient tables came out in bands; on Mali-G71 mediump is also fp16; the same default).
 */
static const char all_highp[] =
    "#define lowp highp\n"
    "#define mediump highp\n";

static const char fragment_start[] =
    "#define varying in\n"
    "#define texture2D texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define textureCube texture\n"
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture(s, (c) / vec2(textureSize(s, 0)))\n"
    "highp vec4 s8gl_data[1];\n"
    "#define gl_FragData s8gl_data\n"
    "#define gl_FragColor s8gl_data[0]\n";
/*
 * What a fragment shader of the first version writes to (gl_FragData), and with EXT_shader_framebuffer_fetch (which
 * CoreAnimation uses where the library offers it, reading gl_LastFragData) are in 3.00 one variable of the shader's own,
 * declared "out" or "inout". The shader's main() is given another name and called from one that reads that variable
 * before and writes it after.
 */
static const char fragment_plain[] =
    "layout(location = 0) out highp vec4 s8gl_colour;\n"
    "#define main s8gl_main\n"
    "#line 1\n";
static const char fragment_plain_end[] =
    "\n#undef main\n"
    "void main() { s8gl_main(); s8gl_colour = s8gl_data[0]; }\n";
static const char fragment_fetch[] =
    "layout(location = 0) inout highp vec4 s8gl_colour;\n"
    "highp vec4 s8gl_last[1];\n"
    "#define gl_LastFragData s8gl_last\n"
    "#define main s8gl_main\n"
    "#line 1\n";
static const char fragment_fetch_end[] =
    "\n#undef main\n"
    "void main() { s8gl_last[0] = s8gl_colour; s8gl_main(); s8gl_colour = s8gl_data[0]; }\n";
static const char vertex_start[] =
    "#define attribute in\n"
    "#define varying out\n"
    "#define texture2D texture\n"
    "#define texture2DLod textureLod\n"
    "#line 1\n";

static GLuint
my_glCreateShader(void *context, GLenum type)
{
	struct eagl *e = context;
	GLuint shader;

	EAGL_ENTER(e, 0x1298);
	shader = glCreateShader(type);
	e->fragment[shader % 256] = type == GL_FRAGMENT_SHADER;
	EAGL_LEAVE(e, 0x1298);
	return shader;
}

/* CoreAnimation does not ask whether its shaders compiled. Here it is asked, and what the compiler said is written down. */
static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct eagl *e = context;
	bool fragment = e->fragment[shader % 256], fetch;
	const GLchar *parts[6] = { "#version 300 es\n", fragment ? fragment_start : vertex_start, "", NULL, "",
		                   full_precision ? all_highp : "" };
	size_t size = 1;
	char *text, *line, *head;

	EAGL_ENTER(e, 0x12a0);
	for (GLsizei i = 0; i < count; i++) {
		size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	if ((text = calloc(1, 2 * size + 64)) == NULL) {
		glShaderSource(shader, count, strings, lengths);
		EAGL_LEAVE(e, 0x12a0);
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		strncat(text, strings[i], lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));
	}
	/* "#extension" lines must stay ahead of everything that is not the preprocessor's: they move to the top */
	head = text + size;
	strcpy(head, parts[0]);
	for (line = text; (line = strstr(line, "#extension")) != NULL; line += 2) {
		size_t length = strcspn(line, "\n");

		strncat(head, line, length);
		strcat(head, "\n");
		line[0] = line[1] = '/';
	}
	parts[0] = head;
	parts[3] = text;
	if (fragment) {
		fetch = strstr(text, "gl_LastFragData") != NULL;
		parts[2] = fetch ? fragment_fetch : fragment_plain;
		parts[4] = fetch ? fragment_fetch_end : fragment_plain_end;
	}
	free((void *)e->sources[shader % 256]);
	e->sources[shader % 256] = text;
	glShaderSource(shader, 6, (const GLchar *const[6]){ parts[0], parts[5], parts[1], parts[2], parts[3], parts[4] }, NULL);
	EAGL_LEAVE(e, 0x12a0);
}

static void
say_long(const char *what, const char *text)
{
	say("s8gl: %s:\n", what);
	for (size_t at = 0, length = strlen(text); at < length; at += 900) {
		char piece[901];
		size_t n = length - at < 900 ? length - at : 900;

		memcpy(piece, text + at, n);
		piece[n] = 0;
		say("%s", piece);
	}
	say("\n");
}

static void
my_glCompileShader(void *context, GLuint shader)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12a8);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	e->shaders++;
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
		say("s8gl: A SHADER DOES NOT COMPILE (shader %u): %s\n", shader, said);
		if (e->sources[shader % 256] != NULL) {
			say_long("its text", e->sources[shader % 256]);
		}
	} else if (e->shaders <= 2 && e->sources[shader % 256] != NULL) {
		say("s8gl: shader %u:\n", shader);
		say_long("the first shaders, for the record", e->sources[shader % 256]);
	}
	EAGL_LEAVE(e, 0x12a8);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12c0);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	e->programs++;
	if (!good) {
		glGetProgramInfoLog(program, sizeof(said), NULL, said);
		say("s8gl: A PROGRAM DOES NOT LINK (program %u): %s\n", program, said);
	}
	EAGL_LEAVE(e, 0x12c0);
}

/* Darwin's library has one entry for deleting a program and deleting a shader. */
static void
my_glDeleteProgram(void *context, GLuint name)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1280);
	if (glIsProgram(name)) {
		glDeleteProgram(name);
	} else {
		glDeleteShader(name);
	}
	EAGL_LEAVE(e, 0x1280);
}

#define EAGL_GEN_TABLE
#include "eagl_gen_s8.h"
#undef EAGL_GEN_TABLE

_Static_assert(EAGL_TABLE_SIZE <= SLOTS * 8, "the table has grown");
_Static_assert(EAGL_TABLE_SIZE <= 0x2200, "the table is far bigger than Darwin's (HOOK_GLTABLE_SIZE 0x2050)");

/* ---- EAGLContext ---------------------------------------------------------------------------------- */

static void
describe_context(unsigned long api)
{
	say("s8gl: an OpenGL ES %lu context for CoreAnimation, by %s: %s; %s; %s; shaders %s\n", api, hook_gl_library(),
	    glGetString ? (const char *)glGetString(GL_VERSION) : "?", glGetString ? (const char *)glGetString(GL_VENDOR) : "?",
	    glGetString ? (const char *)glGetString(GL_RENDERER) : "?",
	    glGetString ? (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION) : "?");
}

static id
my_initWithAPI(id self, SEL selector, unsigned long api, id properties)
{
	struct eagl *e;
	unsigned place;
	static unsigned calls;

	(void)selector;
	calls++;
	if (calls <= 20 || calls % 100 == 0) {
		say("s8gl: -initWithAPI:%lu properties:%p (call %u) [thread %p]\n", api, (void *)properties, calls,
		    (void *)pthread_self());
	}
	for (place = 0; place < NCONTEXTS && contexts[place] != NULL; place++) {
	}
	if (api < 2 || api > 3 || place == NCONTEXTS || !ios_ok || (e = calloc(1, sizeof(*e))) == NULL) {
		say("s8gl: no context of API %lu (%s)\n", api,
		    !ios_ok ? "IOSurface functions missing" : place == NCONTEXTS ? "too many contexts" : "bad API or memory");
		return nil;
	}
	if ((e->gl = hook_gl_create((int)api)) == NULL) {
		say("s8gl: %s gives no OpenGL ES %lu context: %s\n", hook_gl_library(), api, hook_gl_error());
		free(e);
		return nil;
	}
	if (!resolved) {
		resolved = 1;
		unresolved = eagl_resolve_functions();
		say("s8gl: %u gl functions the table or the hand-written entries use are not in the library (listed above)\n",
		    unresolved);
	}
	if (!hook_gl_make_current(e->gl)) {
		say("s8gl: %s cannot make its OpenGL ES %lu context current: %s\n", hook_gl_library(), api, hook_gl_error());
		hook_gl_destroy(e->gl);
		free(e);
		return nil;
	}
	current = e;
	e->object = self;
	e->api = api;
	e->table[0] = e;
	eagl_fill_table(e->table);
	contexts[place] = e;
	describe_context(api);
	return self;
}

static id
my_initWithAPI_only(id self, SEL selector, unsigned long api)
{
	return my_initWithAPI(self, selector, api, nil);
}

static id
my_initWithAPI_sharegroup(id self, SEL selector, unsigned long api, id group)
{
	(void)group;
	return my_initWithAPI(self, selector, api, nil);
}

static unsigned long
my_API(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->api : 0;
}

/*
 * -[EAGLContext sharegroup]. CoreAnimation never asks for it, but CoreImage does (S9+: a crash in Darwin's accessor, which
 * reads ivars my_initWithAPI never filled in). Nothing here shares a context with anybody, so the honest answer is none.
 */
static id
my_sharegroup(id self, SEL selector)
{
	static int said;

	(void)self;
	(void)selector;
	if (!said) {
		said = 1;
		say("s8gl: -sharegroup asked for: none (nothing here shares a context)\n");
	}
	return nil;
}

static unsigned long
my_setParameter(id self, SEL selector, unsigned parameter, const int32_t *values)
{
	static unsigned said;

	(void)self;
	(void)selector;
	if (said++ < 60) {
		say("s8gl: -setParameter:%#x to:%d: noted\n", parameter, values ? values[0] : 0);
	}
	return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
	struct eagl *e = context_of(self);
	static unsigned said;

	(void)selector;
	if (said++ < 5) {
		say("s8gl: -getMacroContextPrivate (%p) = table %p [thread %p]\n", (void *)self, e ? (void *)e->table : NULL,
		    (void *)pthread_self());
	}
	return e ? e->table : NULL;
}

/* (the types are the method's own: the last three arrive on the stack, where sizes matter) */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface, unsigned long target, unsigned long internal,
    GLsizei width, GLsizei height, unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
	static unsigned shown;
	struct eagl *e = context_of(self);
	struct image *image;
	GLint texture = 0;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		say("s8gl: -texImageIOSurface: no context (%p) or surface (%p)\n", (void *)e, (void *)surface);
		return NO;
	}
	eagl_enter(e, 0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	if (target != GL_TEXTURE_2D || texture == 0) {
		say("s8gl: -texImageIOSurface: target %#lx, texture %d: not done\n", target, texture);
		return NO;
	}
	if ((image = image_new(e, (GLuint)texture, surface)) == NULL) {
		/*
		 * No room in the table even after letting go of what could be (image_room()): the surface is copied into the
		 * texture once, as it is now, and not followed after. A texture left as it was made is incomplete and is read as
		 * opaque black.
		 */
		struct image once = {
			.texture = (GLuint)texture, .surface = surface, .to = (GLenum)target, .internal = (GLenum)internal,
			.format = (GLenum)format, .type = (GLenum)type, .plane = plane, .invert = invert, .width = width,
			.height = height, .alpha_first = false,
		};

		surface_to_texture(e, &once);
		e->image_counts.untracked++;
		return YES;
	}
	image->to = (GLenum)target;
	image->internal = (GLenum)internal;
	image->format = (GLenum)format;
	image->type = (GLenum)type;
	image->plane = plane;
	image->invert = invert;
	image->width = width;
	image->height = height;
	note_format(invert ? "IOSurface shown, inverted" : "IOSurface shown", image->internal, image->format, image->type, width,
	    height);
	if (shown++ < 40) {
		say("s8gl: -texImageIOSurface: surface %u ('%s' %zu x %zu, %zu planes, %zu bytes a row) as texture %d: %d x %d, "
		    "internal %#lx format %#lx type %#lx plane %u%s\n", IOSurfaceGetID(surface),
		    fourcc(IOSurfaceGetPixelFormat(surface)), IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
		    IOSurfaceGetPlaneCount(surface), IOSurfaceGetBytesPerRow(surface), texture, width, height, internal, format,
		    type, plane, invert ? ", inverted" : "");
	}
	image->wrapped = IOSurfaceGetPixelFormat(surface) == 0;
	began = now_us();
	if (!surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	return YES;
}

/* (the "image" is the target, GL_TEXTURE_2D, and means the texture bound to it) */
static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface, BOOL inverted)
{
	struct eagl *e = context_of(self);
	static unsigned attached;
	struct image *image;
	GLint texture = 0;
	uint32_t pixels;
	uint64_t began;

	(void)selector;
	if (e == NULL || surface == NULL) {
		say("s8gl: -attachImage: no context (%p) or surface (%p)\n", (void *)e, (void *)surface);
		return NO;
	}
	eagl_enter(e, 0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	pixels = IOSurfaceGetPixelFormat(surface);
	if (target != GL_TEXTURE_2D || texture == 0 || (pixels != 0x42475241 /* 'BGRA' */ && pixels != 0) ||
	    IOSurfaceGetBytesPerElement(surface) != 4 || IOSurfaceGetPlaneCount(surface) != 0) {
		say("s8gl: -attachImage: target %#lx, texture %d, surface %u of '%s' (%zu bytes an element, %zu planes): not done\n",
		    target, texture, IOSurfaceGetID(surface), fourcc(pixels), IOSurfaceGetBytesPerElement(surface),
		    IOSurfaceGetPlaneCount(surface));
		return NO;
	}
	if ((image = image_new(e, (GLuint)texture, surface)) == NULL) {
		/* (no room, as above: what the surface holds now, once, and nothing drawn reaches it) */
		struct image once = {
			.texture = (GLuint)texture, .surface = surface, .to = GL_TEXTURE_2D, .internal = GL_BGRA, .format = GL_BGRA,
			.type = GL_UNSIGNED_BYTE, .invert = inverted, .alpha_first = pixels == 0,
			.width = (GLsizei)IOSurfaceGetWidth(surface), .height = (GLsizei)IOSurfaceGetHeight(surface),
		};

		surface_to_texture(e, &once);
		e->image_counts.untracked++;
		say("s8gl: -attachImage: texture %d: no room to follow its IOSurface; copied once\n", texture);
		return YES;
	}
	image->target = true;
	image->to = GL_TEXTURE_2D;
	image->invert = inverted;
	image->width = (GLsizei)IOSurfaceGetWidth(surface);
	image->height = (GLsizei)IOSurfaceGetHeight(surface);
	image->internal = image->format = GL_BGRA;
	image->type = GL_UNSIGNED_BYTE;
	/*
	 * A surface with no pixel format is memory CoreAnimation has made a surface around: a picture to be shown, as an
	 * application has drawn it (S9+: opacity first, then blue, green, red; the one sign with colours in it comes out orange
	 * if red is taken to follow the opacity). Darwin's library has the order from the surface; here it is taken from there
	 * being no pixel format. UNVERIFIED on 20H392 / our IOSurface stand-in: the log line below says how often it happens.
	 */
	image->wrapped = image->alpha_first = pixels == 0;
	note_format("IOSurface attached", GL_BGRA, GL_BGRA, GL_UNSIGNED_BYTE, image->width, image->height);
	/* what is attached is as often read as drawn into: a texture that is not the surface's memory starts with what the surface holds */
	began = now_us();
	if (!surface_shared(e, image)) {
		surface_changed(e, image);              /* (what it adds up to now) */
		surface_to_texture(e, image);
	}
	e->attaching += now_us() - began;
	if (attached++ < 40) {
		say("s8gl: -attachImage: texture %d is IOSurface %u ('%s' %d x %d, %zu bytes a row)%s%s\n", texture,
		    IOSurfaceGetID(surface), fourcc(pixels), image->width, image->height, IOSurfaceGetBytesPerRow(surface),
		    inverted ? ", drawn inverted" : "", image->shared ? ": its memory" : ": a copy");
	}
	return YES;
}

/* CoreAnimation is NOT expected to call these two (it draws offscreen into IOSurfaces): write down that it does. */
static BOOL
my_presentRenderbuffer(id self, SEL selector, unsigned long renderbuffer)
{
	static unsigned said;

	(void)self;
	(void)selector;
	if (said++ < 40) {
		say("s8gl: -presentRenderbuffer:%lu CALLED (CoreAnimation is not expected to): nothing done\n", renderbuffer);
	}
	return NO;
}

static BOOL
my_renderbufferStorage(id self, SEL selector, unsigned long renderbuffer, id drawable)
{
	static unsigned said;

	(void)self;
	(void)selector;
	if (said++ < 40) {
		say("s8gl: -renderbufferStorage:%lu fromDrawable:%p CALLED: nothing done\n", renderbuffer, (void *)drawable);
	}
	return NO;
}

static void
my_dealloc(id self, SEL selector)
{
	struct objc_super super = { self, class_getSuperclass(objc_getClass("EAGLContext")) };
	struct eagl *e = context_of(self);

	if (e != NULL) {
		say("s8gl: the context goes (%u frames)\n", e->frames);
		summary(e);
		for (unsigned i = 0; i < NCONTEXTS; i++) {
			if (contexts[i] == e) {
				contexts[i] = NULL;
			}
		}
		if (current == e) {
			hook_gl_make_current(NULL);
			current = NULL;
		}
		hook_gl_destroy(e->gl);
		for (unsigned i = 0; i < e->image_count; i++) {
			CFRelease(e->images[i].surface);        /* (the textures went with the library's context) */
		}
		free(e->images);
		free(e->image_index);
		free(e);
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, selector);
}

/* Crash report for the log: no crash reporter runs on this system, so a fault in the GL layer or in Mesa would leave nothing but a restarted
 * backboardd. SA_RESETHAND: after the report the same fault happens again and kills the process as before. */
#ifdef __APPLE__
static void
s8gl_fault(int sig, siginfo_t *si, void *uctx)
{
	ucontext_t *uc = uctx;
	uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr, sp = uc->uc_mcontext->__ss.__sp;
	uint64_t *fp = (uint64_t *)uc->uc_mcontext->__ss.__fp;
	Dl_info di;

	say("s8gl: FAULT signal %d code %d address %p pc %#llx lr %#llx sp %#llx\n", sig, si->si_code, si->si_addr,
	    (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)sp);
	for (int k = 0; k < 2; k++) {
		uint64_t a = k == 0 ? pc : lr;

		if (dladdr((void *)a, &di) && di.dli_sname != NULL) {
			say("s8gl:   %s %s+%#llx (image %s)\n", k == 0 ? "pc" : "lr", di.dli_sname, (unsigned long long)(a - (uint64_t)di.dli_saddr),
			    di.dli_fname ? di.dli_fname : "?");
		} else if (dladdr((void *)a, &di)) {
			say("s8gl:   %s image %s +%#llx\n", k == 0 ? "pc" : "lr", di.dli_fname ? di.dli_fname : "?", (unsigned long long)(a - (uint64_t)di.dli_fbase));
		}
	}
	/* registers x0..x28, and for each that points at readable memory the first 0x80 bytes behind it (the IOSurface client whose state
	 * pointer, client+0x70, is NULL is somewhere in here) */
	for (int r = 0; r < 29; r++) {
		uint64_t v = uc->uc_mcontext->__ss.__x[r];
		uint64_t words[16];
		vm_size_t got = 0;

		if (v > 0x100000000ULL && v < 0x800000000ULL && vm_read_overwrite(mach_task_self(), (vm_address_t)v, sizeof(words),
		    (vm_address_t)words, &got) == KERN_SUCCESS && got == sizeof(words)) {
			say("s8gl:   x%d %#llx: %llx %llx %llx %llx %llx %llx %llx %llx | %llx %llx %llx %llx %llx %llx %llx %llx\n", r, (unsigned long long)v,
			    (unsigned long long)words[0], (unsigned long long)words[1], (unsigned long long)words[2], (unsigned long long)words[3],
			    (unsigned long long)words[4], (unsigned long long)words[5], (unsigned long long)words[6], (unsigned long long)words[7],
			    (unsigned long long)words[8], (unsigned long long)words[9], (unsigned long long)words[10], (unsigned long long)words[11],
			    (unsigned long long)words[12], (unsigned long long)words[13], (unsigned long long)words[14], (unsigned long long)words[15]);
			if (r == 19) {          /* the IOSurfaceRef of the fault: its client is the pointer at +8; dump that and what it points at */
				for (int k = 0; k < 6; k++) {
					uint64_t w[24];
					vm_size_t g = 0;

					if (words[k] > 0x100000000ULL && words[k] < 0x800000000ULL && vm_read_overwrite(mach_task_self(),
					    (vm_address_t)words[k], sizeof(w), (vm_address_t)w, &g) == KERN_SUCCESS && g == sizeof(w)) {
						say("s8gl:     x19[%d] %#llx: %llx %llx %llx %llx %llx %llx %llx %llx | %llx %llx %llx %llx %llx %llx %llx %llx | "
						    "%llx %llx %llx %llx %llx %llx %llx %llx\n", k, (unsigned long long)words[k],
						    (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
						    (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7],
						    (unsigned long long)w[8], (unsigned long long)w[9], (unsigned long long)w[10], (unsigned long long)w[11],
						    (unsigned long long)w[12], (unsigned long long)w[13], (unsigned long long)w[14], (unsigned long long)w[15],
						    (unsigned long long)w[16], (unsigned long long)w[17], (unsigned long long)w[18], (unsigned long long)w[19],
						    (unsigned long long)w[20], (unsigned long long)w[21], (unsigned long long)w[22], (unsigned long long)w[23]);
					}
				}
			}
		} else {
			say("s8gl:   x%d %#llx\n", r, (unsigned long long)v);
		}
	}
	for (int n = 0; n < 14 && fp != NULL && ((uintptr_t)fp & 7) == 0; n++) {
		uint64_t ret = fp[1];

		if (dladdr((void *)ret, &di) && di.dli_sname != NULL) {
			say("s8gl:   #%d %#llx %s+%#llx (%s)\n", n, (unsigned long long)ret, di.dli_sname, (unsigned long long)(ret - (uint64_t)di.dli_saddr),
			    di.dli_fname ? di.dli_fname : "?");
		} else {
			say("s8gl:   #%d %#llx\n", n, (unsigned long long)ret);
		}
		fp = (uint64_t *)fp[0];
	}
}

static void
s8gl_fault_install(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = s8gl_fault;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
}
#else
static void
s8gl_fault_install(void)
{
}
#endif

static int installed;

__attribute__((constructor)) static void
s8gl_install(void)
{
	static const struct {
		const char *name;
		IMP ours;
		int optional;                   /* a method this port only logs / wraps; its absence is not worth a line */
	} methods[] = {
		{ "initWithAPI:properties:", (IMP)my_initWithAPI, 0 },
		{ "initWithAPI:", (IMP)my_initWithAPI_only, 1 },
		{ "initWithAPI:sharegroup:", (IMP)my_initWithAPI_sharegroup, 1 },
		{ "API", (IMP)my_API, 0 },
		{ "sharegroup", (IMP)my_sharegroup, 0 },
		{ "setParameter:to:", (IMP)my_setParameter, 0 },
		{ "getMacroContextPrivate", (IMP)my_getMacroContextPrivate, 0 },
		{ "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", (IMP)my_texImageIOSurface, 0 },
		{ "attachImage:toCoreSurface:invertedRender:", (IMP)my_attachImage, 0 },
		{ "presentRenderbuffer:", (IMP)my_presentRenderbuffer, 1 },
		{ "renderbufferStorage:fromDrawable:", (IMP)my_renderbufferStorage, 1 },
		{ "dealloc", (IMP)my_dealloc, 0 },
	};
	const char *name = getprogname();
	Class class;
	unsigned replaced = 0;
	size_t size = sizeof(bootargs) - 1;
	const char *argument;

	if (installed) {
		return;
	}
	if (name == NULL || (strcmp(name, "backboardd") != 0 && getenv("S8GL_ANY") == NULL)) {
		return;                         /* only the window server (S8GL_ANY=1: any program that has an EAGLContext, for tests) */
	}
	installed = 1;
	if (sysctlbyname("kern.bootargs", bootargs, &size, NULL, 0) != 0) {
		bootargs[0] = 0;
	}
	say("s8gl: libs8gl loaded in %s pid %d (boot-args: %s)\n", name, getpid(), bootargs);
	s8gl_fault_install();
	if (arg_int("gl", -1) == 0) {
		say("s8gl: gl=0: nothing is replaced\n");
		return;
	}
	if ((argument = arg_get("gltrace")) != NULL) {
		trace_first = trace_last = (unsigned)strtoul(argument, (char **)&argument, 10);
		if (*argument == '-') {
			trace_last = (unsigned)strtoul(argument + 1, NULL, 10);
		}
		if (trace_first == 0) {
			trace_first = 1;
			trace_last = 0;         /* none */
		}
	}
	trace_lines_left = (unsigned long)arg_int("gltrace_max", 20000);
	frame_lines = arg_int("glframes", 0) == 1;
	keep_checking = arg_int("glerrors", 0) == 2;
	check_errors = keep_checking || arg_int("glerrors", 0) == 1;
	/* sharing needs the Mesa patch + kernel selector 0x7e (hook_gl.h): opt-in only, glshare=1 */
	share_surfaces = arg_int("glshare", 0) == 1 && arg_int("glcopy", 0) != 1;
	fetch_offered = arg_int("glfetch", 1) != 0;
	full_precision = arg_int("glprecision", 1) != 0;
	plain_draws = (int)arg_int("glplain", 1);
	wait_for_frames = arg_int("glwait", 1) != 0;
	if ((argument = arg_get("glsurfaces")) != NULL) {
		images_most = (unsigned)strtoul(argument, NULL, 10);
		images_most = images_most < 16 ? 16 : images_most;
	}
	if ((argument = arg_get("glsurfaces_mb")) != NULL) {
		image_bytes_most = strtoull(argument, NULL, 10) << 20;
		image_bytes_most = image_bytes_most < (16ull << 20) ? 16ull << 20 : image_bytes_most;
	}
	ios_ok = ios_resolve();
	say("s8gl: IOSurface functions %s; gltrace frames %u-%u, glframes %d, glerrors %d, share %d, glplain %d, glwait %d\n",
	    ios_ok ? "found" : "MISSING (the replaced initWithAPI will return nil)", trace_first, trace_last, frame_lines,
	    check_errors, share_surfaces, plain_draws, wait_for_frames);
	class = objc_getClass("EAGLContext");
	if (class == Nil) {
		say("s8gl: this program has no EAGLContext (yet): nothing to do\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		SEL selector = sel_registerName(methods[i].name);
		Method method = class_getInstanceMethod(class, selector);

		if (method == NULL) {
			if (!methods[i].optional) {
				say("s8gl: EAGLContext has no -%s\n", methods[i].name);
			}
			continue;
		}
		say("s8gl: -[EAGLContext %s] (%s) replaced\n", methods[i].name, method_getTypeEncoding(method));
		/* class_replaceMethod adds to this class when the method is inherited, replaces otherwise */
		class_replaceMethod(class, selector, methods[i].ours, method_getTypeEncoding(method));
		replaced++;
	}
	say("s8gl: %u of %zu methods of EAGLContext are ours; OpenGL ES by %s\n", replaced, sizeof(methods) / sizeof(methods[0]),
	    hook_gl_library());
}
