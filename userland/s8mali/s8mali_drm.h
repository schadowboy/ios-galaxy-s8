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
#define S8MALI_SEL_WAIT_SEQ 0x7d  /* wait for a job by its number (struct s8mali_wait_seq) */

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

/* All return 0 / a fd >= 0 / a pointer, or -1 (MAP_FAILED, NULL) with errno. */
int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmCloseBufferHandle(int fd, uint32_t handle);
void drmClose(int fd);
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
