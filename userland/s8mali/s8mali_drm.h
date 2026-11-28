/*
 * s8mali_drm: a minimal "libdrm" for the S8Mali IOKit service (Mali-G71 of the Galaxy S8 on our iOS 16 port).
 *
 * Adapted from hakios.drm.c / xf86drm.h of the HakiOS Galaxy S9+ project (baseline-support/hakios-drm),
 * Copyright (c) 2025-2026 the HakiOS authors, MIT License (below). Changes: service name S8Mali, no Mesa
 * dependency (the few drm_panfrost_* structs are declared here), the S9+ opt-in extras (HFT trap, HWC completion
 * cache, HMD madvise, IOSurface import, sync objects) removed.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 * to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 * the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 * THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef S8MALI_DRM_H
#define S8MALI_DRM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioccom.h>
#include <sys/types.h>

/* request number = low byte of the ioctl code = IOKit external method selector (S9+ numbers) */
#define DRM_COMMAND_BASE 0x40
#define DRM_PANFROST_SUBMIT 0x00
#define DRM_PANFROST_WAIT_BO 0x01
#define DRM_PANFROST_CREATE_BO 0x02
#define DRM_PANFROST_MMAP_BO 0x03
#define DRM_PANFROST_GET_PARAM 0x04
#define DRM_PANFROST_GET_BO_OFFSET 0x05
#define DRM_PANFROST_MADVISE 0x08
#define S8MALI_SEL_GEM_CLOSE 0x09 /* DRM_IOCTL_GEM_CLOSE & 0xff */
#define S8MALI_SEL_SURFACE_SEQ 0x7c /* S9+ HakiosMali kHakiosMaliSurfaceSeq: the job after which an IOSurface is drawn (NOT in S8Mali yet) */
#define S8MALI_SEL_WAIT_SEQ 0x7d  /* wait for a job by its number (struct s8mali_wait_seq) */
#define S8MALI_SEL_SURFACE 0x7e   /* S9+ kHakiosMaliSurface: an IOSurface's memory as a buffer (struct s8mali_surface); S8Mali: kernel patch 0163 (never run) */
#define S8MALI_SEL_SHOW 0x7f      /* S9+ kHakiosMaliShow: a test's pixels onto the panel (not needed) */
#define S8MALI_SURFACE_DESCRIBE 0xffffffffu /* struct s8mali_surface.handle: only describe the buffer, make none */

#ifndef _PANFROST_DRM_H_ /* Mesa's uapi header, if one is included instead */
#define PANFROST_BO_NOEXEC 1
#define PANFROST_BO_HEAP 2

enum drm_panfrost_param {
	DRM_PANFROST_PARAM_GPU_PROD_ID = 0,
	DRM_PANFROST_PARAM_GPU_REVISION,
	DRM_PANFROST_PARAM_SHADER_PRESENT,
	DRM_PANFROST_PARAM_TILER_PRESENT,
	DRM_PANFROST_PARAM_L2_PRESENT,
	DRM_PANFROST_PARAM_STACK_PRESENT,
	DRM_PANFROST_PARAM_AS_PRESENT,
	DRM_PANFROST_PARAM_JS_PRESENT,
	DRM_PANFROST_PARAM_L2_FEATURES,
	DRM_PANFROST_PARAM_CORE_FEATURES,
	DRM_PANFROST_PARAM_TILER_FEATURES,
	DRM_PANFROST_PARAM_MEM_FEATURES,
	DRM_PANFROST_PARAM_MMU_FEATURES,
	DRM_PANFROST_PARAM_THREAD_FEATURES,
	DRM_PANFROST_PARAM_MAX_THREADS,
	DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ,
	DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ,
	DRM_PANFROST_PARAM_COHERENCY_FEATURES,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES0,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES1,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES2,
	DRM_PANFROST_PARAM_TEXTURE_FEATURES3,
	DRM_PANFROST_PARAM_JS0_FEATURES, /* 22 .. 37: JS0..JS15 */
	DRM_PANFROST_PARAM_NR_CORE_GROUPS = 38,
	DRM_PANFROST_PARAM_THREAD_TLS_ALLOC = 39,
	DRM_PANFROST_PARAM_AFBC_FEATURES = 40,
};

struct drm_panfrost_submit { /* 40 bytes: the S9+ pinned ABI (no jm_ctx_handle) */
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns; /* ABSOLUTE CLOCK_MONOTONIC ns, as Linux's (s8mali_ioctl converts) */
};
struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset; /* GPU VA chosen by the kernel */
};
struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};
struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};
struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};
#endif

struct drm_gem_close {
	uint32_t handle, pad;
};
struct s8mali_wait_seq {
	uint64_t seq;
	int64_t timeout_ns; /* relative */
};
/*
 * Selector 0x7e (the S9+ project's HakiosMali kHakiosMaliSurface, hakios.mali.cpp struct mali_surface), implemented by kernel
 * patch 0163 (xnu/patches/0163-s8-mali-surface-bo.patch; before it: kIOReturnUnsupported -> ENOSYS; this struct is 40 bytes). Semantics, from the S9+ shim (hakios.drm.c drmPrimeFDToHandle and
 * hakios_drm_surface): the kernel looks the IOSurface up by `surface` (its ID, what IOSurfaceGetID() returns; for us a
 * SweetSurface of patch 0046) and
 *   - handle == S8MALI_SURFACE_DESCRIBE: only fills size/offset/row_bytes/width/height/pixel_format/planes (describe);
 *   - otherwise: makes a BO of the surface's own memory in the GPU address space (every page of it mapped RW into AS0 like a
 *     CREATE_BO buffer, same VA rules: 0x2000000..4 GiB, 16 KiB aligned), keeps the surface alive while the BO exists,
 *     returns the new BO handle in `handle` (the same handle then works with GET_BO_OFFSET, SUBMIT's bo_handles and
 *     GEM_CLOSE; the CPU mapping is the surface's, no IOConnectMapMemory64 needed), `size` = the BO's size, `offset` = where
 *     the pixels start inside it, `row_bytes` = bytes a row. Asking twice for one surface returns the same handle (the shim
 *     calls this with the driver's table of BOs locked in the S9+ code); the BO ends at GEM_CLOSE or when the surface goes.
 * Memory attributes: the S9+ driver used write-combined/uncached memory for surfaces the GPU draws; S8's SweetSurface memory
 * is ordinary cached kernel memory, so 0163 does the second of the two options: the buffer takes part in SUBMIT's clean+
 * invalidate (FlushPoC_DcacheRegion by the surface's kernel VA) of the buffers a submit lists (patches 0161/0162) -- the GPU is
 * not coherent. The CPU's own mapping of the surface stays cacheable. Scan-out pool surfaces (memory outside XNU's RAM, below
 * 4 GiB) are entered into the GPU's tables from their contiguous physical range.
 * 0x7c (surface seq) lets the display driver wait for the job that drew a surface; for us hook_gl_surface_wait()/glFinish
 * does that in user space before AppleCLCD::swap reads the pixels (gpu-plan.md), so 0x7c is not needed first.
 */
struct s8mali_surface {
	uint32_t surface; /* in: the IOSurface's ID */
	uint32_t handle;  /* in: S8MALI_SURFACE_DESCRIBE or anything else (make the BO); out: the BO's handle */
	uint64_t size;    /* out: how big the buffer is */
	uint32_t offset, row_bytes; /* out: where the pixels start in it and how long a row of them is */
	uint32_t width, height, pixel_format, planes; /* out: and what they are */
};

/* ioctl codes as Linux's: only the low byte (the selector) and the size (IOCPARM_LEN) are used */
#define S8MALI_IOCTL(n, type) _IOWR('d', DRM_COMMAND_BASE + (n), type)
#define DRM_IOCTL_PANFROST_SUBMIT S8MALI_IOCTL(DRM_PANFROST_SUBMIT, struct drm_panfrost_submit)
#define DRM_IOCTL_PANFROST_WAIT_BO S8MALI_IOCTL(DRM_PANFROST_WAIT_BO, struct drm_panfrost_wait_bo)
#define DRM_IOCTL_PANFROST_CREATE_BO S8MALI_IOCTL(DRM_PANFROST_CREATE_BO, struct drm_panfrost_create_bo)
#define DRM_IOCTL_PANFROST_MMAP_BO S8MALI_IOCTL(DRM_PANFROST_MMAP_BO, struct drm_panfrost_mmap_bo)
#define DRM_IOCTL_PANFROST_GET_PARAM S8MALI_IOCTL(DRM_PANFROST_GET_PARAM, struct drm_panfrost_get_param)
#define DRM_IOCTL_PANFROST_GET_BO_OFFSET S8MALI_IOCTL(DRM_PANFROST_GET_BO_OFFSET, struct drm_panfrost_get_bo_offset)
#define DRM_IOCTL_PANFROST_MADVISE S8MALI_IOCTL(DRM_PANFROST_MADVISE, struct drm_panfrost_madvise)
#define DRM_IOCTL_GEM_CLOSE _IOW('d', S8MALI_SEL_GEM_CLOSE, struct drm_gem_close)

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

/* All return 0 / a fd >= 0 / a pointer, or -1 (MAP_FAILED, NULL) with errno (drmCommand*: 0 or minus an errno, as libdrm's). */
int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmCloseBufferHandle(int fd, uint32_t handle);
void drmClose(int fd);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
/* an IOSurface comes in by its ID where Linux has a dma-buf descriptor: needs the kernel's selector 0x7e (see above) */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int s8mali_surface(int fd, struct s8mali_surface *surface); /* describe only (selector 0x7e, handle = DESCRIBE): 0 or -1/errno */
/* sync objects = job numbers kept in this process (the kernel has numbers, not fences) */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);
/* wait up to `seconds` for the S8Mali service to be registered (1: found) */
int s8mali_wait_service(int seconds);
void *hakios_drm_map(int fd, uint32_t handle, uint64_t size);
void hakios_drm_unmap(int fd, uint32_t handle, void *address);

/* the job number the kernel gave to the last successful SUBMIT (it comes back in jc; the shim restores jc) */
uint64_t s8mali_last_seq(void);
/* wait for a job number: 0 or an errno. deadline = absolute CLOCK_MONOTONIC ns, INT64_MAX = whenever */
int s8mali_wait_seq(uint64_t seq, int64_t deadline_ns);
/* current CLOCK_MONOTONIC in ns */
int64_t s8mali_now_ns(void);
/* last kern_return_t of the transport, for logs */
int s8mali_last_kr(void);

#endif
