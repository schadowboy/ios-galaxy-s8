/*
 * The DRM calls of Mesa's panfrost driver as a table of function pointers. Mesa's drm stand-in (s8_drm_stub.c) answers every
 * drm* call by calling the table's entry, or fails (ENODEV/ENOSYS) when there is none. The program that loads libOSMesa
 * (userland/s8mali/s8gl_test.c, with the real implementation in s8mali_drm.c) hands its table over with s8_drm_set_ops()
 * before it creates a context. This replaces relying on the program's symbols overriding the library's weak ones, which
 * dyld does not do for calls inside one image.
 *
 * Layout shared by both sides: change it only together.
 */
#ifndef S8_DRM_OPS_H
#define S8_DRM_OPS_H

#include <stddef.h>
#include <stdint.h>

struct s8_drm_ops {
	uint32_t size; /* sizeof(struct s8_drm_ops) as the caller knows it */
	int (*OpenWithType)(const char *name, const char *busid, int type);
	int (*Ioctl)(int fd, unsigned long request, void *arg);
	int (*CommandWrite)(int fd, unsigned long index, const void *data, unsigned long size);
	int (*CommandWriteRead)(int fd, unsigned long index, void *data, unsigned long size);
	void *(*GetVersion)(int fd);
	void (*FreeVersion)(void *version);
	int (*GetCap)(int fd, uint64_t capability, uint64_t *value);
	int (*CloseBufferHandle)(int fd, uint32_t handle);
	int (*PrimeHandleToFD)(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
	int (*PrimeFDToHandle)(int fd, int prime_fd, uint32_t *handle);
	int (*SyncobjCreate)(int fd, uint32_t flags, uint32_t *handle);
	int (*SyncobjDestroy)(int fd, uint32_t handle);
	int (*SyncobjReset)(int fd, const uint32_t *handles, uint32_t count);
	int (*SyncobjWait)(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
	    uint32_t *first_signaled);
	int (*SyncobjFDToHandle)(int fd, int obj_fd, uint32_t *handle);
	int (*SyncobjHandleToFD)(int fd, uint32_t handle, int *obj_fd);
	int (*SyncobjImportSyncFile)(int fd, uint32_t handle, int sync_file_fd);
	int (*SyncobjExportSyncFile)(int fd, uint32_t handle, int *sync_file_fd);
	int (*SyncobjTimelineWait)(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
	    unsigned flags, uint32_t *first_signaled);
	int (*SyncobjTransfer)(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
	    uint32_t flags);
	void *(*Map)(int fd, uint32_t handle, uint64_t size);
	void (*Unmap)(int fd, uint32_t handle, void *address);
};

/* exported by libOSMesa (s8_drm_stub.c): returns 0 when the table was taken */
int s8_drm_set_ops(const struct s8_drm_ops *ops);

#endif
