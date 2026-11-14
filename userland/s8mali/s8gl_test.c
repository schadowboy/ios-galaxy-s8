/*
 * s8gl_test: one coloured triangle through the OpenGL ES library of Mesa (libOSMesa.8.dylib, panfrost on the Mali-G71
 * when the S8Mali service is there, softpipe on the CPU when it is not), read back and checked pixel by pixel. It says
 * WHICH driver drew (GL_RENDERER), how long a frame took and what the pixels are, so the picture need not be seen.
 * Structure after gl_test.c of the HakiOS Galaxy S9+ project (Copyright (c) 2025-2026 the HakiOS authors, MIT License,
 * full text in s8mali_drm.h). No panel output: this proves the GPU renders; it does not touch the window server.
 *
 *   s8gl_test [WIDTH HEIGHT]     default 256 x 256
 *
 * Library: $S8GL_LIB, else /usr/local/lib/s8mesa/libOSMesa.8.dylib. Log: /var/mobile/s8gl_test.log (else /tmp).
 * "S8GL-TEST: <what> PASS|FAIL" lines; the last line is "S8GL-TEST: all passed" or "S8GL-TEST: N failures".
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_ES_PROFILE               0x38
#define GL_TRIANGLES                    0x0004
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_2D                   0x0de1
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER                  0x8d40

#include "s8mali_drm.h"
#include "../mesa/overlay/src/s8-drm/s8_drm_ops.h"

extern int s8mali_wait_service(int seconds); /* s8mali_drm.c */

/* the shim's DRM calls that s8mali_drm.h does not declare (s8mali_drm.c) */
extern int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
extern int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
extern int drmGetCap(int fd, uint64_t capability, uint64_t *value);
extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
extern int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
extern int drmSyncobjDestroy(int fd, uint32_t handle);
extern int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
extern int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
extern int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
extern int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
extern int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
extern int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
extern int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags);

/* the DRM table handed to Mesa: this program's own shim (s8mali_drm.c), called by Mesa's stand-in */
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

typedef void *OSMesaContext;
static OSMesaContext (*pCreateContextAttribs)(const int *attributes, OSMesaContext share);
static unsigned char (*pMakeCurrent)(OSMesaContext context, void *buffer, unsigned type, int width, int height);
static void *(*pGetProcAddress)(const char *name);

static const unsigned char *(*glGetString)(unsigned);
static unsigned (*glGetError)(void);
static void (*glGenTextures)(int, unsigned *);
static void (*glBindTexture)(unsigned, unsigned);
static void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*glTexParameteri)(unsigned, unsigned, int);
static void (*glGenFramebuffers)(int, unsigned *);
static void (*glBindFramebuffer)(unsigned, unsigned);
static void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
static unsigned (*glCheckFramebufferStatus)(unsigned);
static void (*glViewport)(int, int, int, int);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(unsigned);
static unsigned (*glCreateShader)(unsigned);
static void (*glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*glCompileShader)(unsigned);
static void (*glGetShaderiv)(unsigned, unsigned, int *);
static unsigned (*glCreateProgram)(void);
static void (*glAttachShader)(unsigned, unsigned);
static void (*glBindAttribLocation)(unsigned, unsigned, const char *);
static void (*glLinkProgram)(unsigned);
static void (*glGetProgramiv)(unsigned, unsigned, int *);
static void (*glUseProgram)(unsigned);
static void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (*glEnableVertexAttribArray)(unsigned);
static void (*glDrawArrays)(unsigned, int, int);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *);

static FILE *logf_;
static int failures;

static void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (logf_ != NULL) {
		va_start(ap, fmt);
		vfprintf(logf_, fmt, ap);
		va_end(ap);
		fflush(logf_);
		fsync(fileno(logf_));
	}
}

static void
verdict(const char *what, int ok)
{
	say("S8GL-TEST: %s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok) {
		failures++;
	}
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

#define LOAD(name)                                                              \
	do {                                                                    \
		*(void **)&name = pGetProcAddress(#name);                       \
		if (name == NULL) {                                             \
			say("S8GL-TEST: no function %s\n", #name);              \
			return 0;                                               \
		}                                                               \
	} while (0)

static int
load_gl(void)
{
	LOAD(glGetString);
	LOAD(glGetError);
	LOAD(glGenTextures);
	LOAD(glBindTexture);
	LOAD(glTexImage2D);
	LOAD(glTexParameteri);
	LOAD(glGenFramebuffers);
	LOAD(glBindFramebuffer);
	LOAD(glFramebufferTexture2D);
	LOAD(glCheckFramebufferStatus);
	LOAD(glViewport);
	LOAD(glClearColor);
	LOAD(glClear);
	LOAD(glCreateShader);
	LOAD(glShaderSource);
	LOAD(glCompileShader);
	LOAD(glGetShaderiv);
	LOAD(glCreateProgram);
	LOAD(glAttachShader);
	LOAD(glBindAttribLocation);
	LOAD(glLinkProgram);
	LOAD(glGetProgramiv);
	LOAD(glUseProgram);
	LOAD(glVertexAttribPointer);
	LOAD(glEnableVertexAttribArray);
	LOAD(glDrawArrays);
	LOAD(glFinish);
	LOAD(glReadPixels);
	return 1;
}

static unsigned
shader(unsigned type, const char *source)
{
	unsigned s = glCreateShader(type);
	int ok = 0;

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		say("S8GL-TEST: a %s shader did not compile\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment");
		return 0;
	}
	return s;
}

static void
pixel(const uint8_t *p, int width, int x, int y, uint8_t out[4])
{
	memcpy(out, p + ((size_t)y * (size_t)width + (size_t)x) * 4, 4);
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 2, 0 };
	/* where, and what colour: a triangle that covers the middle of the frame and none of its corners */
	static const float corners[] = {
		-0.8f, -0.6f, 1, 0, 0,
		 0.8f, -0.6f, 0, 1, 0,
		 0.0f,  0.7f, 0, 0, 1,
	};
	static unsigned char window[16 * 16 * 4];
	int width = argc > 2 ? atoi(argv[1]) : 256, height = argc > 2 ? atoi(argv[2]) : 256;
	const char *path = getenv("S8GL_LIB");
	unsigned texture = 0, framebuffer = 0, program;
	OSMesaContext context;
	uint8_t *drawn, p[4];
	uint64_t began, took;
	void *lib;
	int ok = 0;

	logf_ = fopen("/var/mobile/s8gl_test.log", "a");
	if (logf_ == NULL) {
		logf_ = fopen("/tmp/s8gl_test.log", "a");
	}
	say("S8GL-TEST: start pid %d uid %d\n", (int)getpid(), (int)getuid());
	if (width < 16 || height < 16 || width > 4096 || height > 4096) {
		width = height = 256;
	}
	/* started at boot, the S8Mali service may not be there yet; Mesa would fall back to softpipe without a word */
	verdict("S8Mali service present (waited up to 90 s)", s8mali_wait_service(90));
	lib = dlopen(path != NULL ? path : "/usr/local/lib/s8mesa/libOSMesa.8.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (lib == NULL) {
		say("S8GL-TEST: dlopen failed: %s\n", dlerror());
		verdict("load Mesa", 0);
		say("S8GL-TEST: %d failures\n", failures);
		return 1;
	}
	*(void **)&pCreateContextAttribs = dlsym(lib, "OSMesaCreateContextAttribs");
	*(void **)&pMakeCurrent = dlsym(lib, "OSMesaMakeCurrent");
	*(void **)&pGetProcAddress = dlsym(lib, "OSMesaGetProcAddress");
	if (!pCreateContextAttribs || !pMakeCurrent || !pGetProcAddress) {
		verdict("find the OSMesa entry points", 0);
		return 1;
	}
	verdict("load Mesa", 1);
	{
		/* give Mesa the real DRM layer: without it Mesa's stand-in fails every call and it draws on the CPU */
		int (*set_ops)(const struct s8_drm_ops *) = (int (*)(const struct s8_drm_ops *))dlsym(lib, "s8_drm_set_ops");
		struct s8_drm_ops table;

		fill_ops(&table);
		verdict("hand the DRM table to Mesa", set_ops != NULL && set_ops(&table) == 0);
	}
	context = pCreateContextAttribs(attributes, NULL);
	if (context == NULL || !pMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		verdict("OpenGL ES context", 0);
		say("S8GL-TEST: %d failures\n", failures);
		return 1;
	}
	verdict("OpenGL ES context", 1);
	if (!load_gl()) {
		verdict("GL functions", 0);
		return 1;
	}
	say("S8GL-TEST: vendor \"%s\" renderer \"%s\" version \"%s\"\n", (const char *)glGetString(GL_VENDOR),
	    (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
	{
		const char *r = (const char *)glGetString(GL_RENDERER);

		/* what drew: Mesa names its panfrost renderer "Mali-G71 (Panfrost)", softpipe "softpipe" */
		say("S8GL-TEST: drawn by %s\n", r != NULL && strstr(r, "anfrost") != NULL ? "the Mali GPU (panfrost)" :
		    r != NULL && strstr(r, "softpipe") != NULL ? "the CPU (softpipe)" : "an unknown driver");
		verdict("renderer is panfrost (GPU)", r != NULL && strstr(r, "anfrost") != NULL);
	}

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	verdict("framebuffer complete", glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
	    "attribute vec2 where; attribute vec3 colour; varying vec3 c;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); c = colour; }\n"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec3 c;\n"
	    "void main() { gl_FragColor = vec4(c, 1.0); }\n"));
	glBindAttribLocation(program, 0, "where");
	glBindAttribLocation(program, 1, "colour");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	verdict("shaders compile and link", ok);
	if (!ok) {
		say("S8GL-TEST: %d failures\n", failures);
		return 1;
	}
	glUseProgram(program);
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), corners);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), corners + 2);

	began = now_us();
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	took = now_us() - began;
	say("S8GL-TEST: a %d x %d frame took %llu us\n", width, height, (unsigned long long)took);

	drawn = calloc((size_t)width * (size_t)height, 4);
	if (drawn == NULL) {
		verdict("memory for the readback", 0);
		return 1;
	}
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, drawn);
	verdict("no GL error", glGetError() == 0);

	/* the corners are outside the triangle: the clear colour (black, alpha 1) */
	pixel(drawn, width, 1, 1, p);
	say("S8GL-TEST: corner pixel %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
	verdict("a corner is the clear colour", p[0] < 8 && p[1] < 8 && p[2] < 8 && p[3] == 255);
	pixel(drawn, width, width - 2, height - 2, p);
	verdict("the opposite corner is the clear colour", p[0] < 8 && p[1] < 8 && p[2] < 8 && p[3] == 255);

	/* the centre is inside: an interpolation of red, green and blue, so not black and with alpha 255 */
	pixel(drawn, width, width / 2, height / 2 - height / 16, p);
	say("S8GL-TEST: centre pixel %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
	verdict("the centre is part of the triangle", p[3] == 255 && p[0] + p[1] + p[2] > 120);

	/* near the red corner of the triangle (bottom left): red wins */
	pixel(drawn, width, (int)(width * 0.16f), (int)(height * 0.24f), p);
	say("S8GL-TEST: near the red corner %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
	verdict("red dominates near the red corner", p[0] > 150 && p[0] > p[1] + 80 && p[0] > p[2] + 80);
	/* near the blue corner (top): blue wins */
	pixel(drawn, width, width / 2, (int)(height * 0.82f), p);
	say("S8GL-TEST: near the blue corner %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3]);
	verdict("blue dominates near the blue corner", p[2] > 150 && p[2] > p[0] + 80 && p[2] > p[1] + 80);

	{
		/* a count of the pixels that are not black: the triangle is 0.8 x 0.65 / 2 = 26% of the frame */
		size_t lit = 0, total = (size_t)width * (size_t)height;

		for (size_t i = 0; i < total; i++) {
			const uint8_t *q = drawn + i * 4;
			if (q[0] | q[1] | q[2]) {
				lit++;
			}
		}
		say("S8GL-TEST: %zu of %zu pixels are lit (%zu%%)\n", lit, total, lit * 100 / total);
		verdict("a plausible share of the frame is lit (18 to 40 percent)", lit * 100 / total >= 18 && lit * 100 / total <= 40);
	}
	free(drawn);
	if (failures == 0) {
		say("S8GL-TEST: all passed\n");
	} else {
		say("S8GL-TEST: %d failures\n", failures);
	}
	say("S8GL-TEST: done\n");
	return failures != 0;
}
