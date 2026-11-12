/*
 * Placeholder implementation of the drm stand-in (xf86drm.h next to it): every function fails, so a Mesa
 * built with it falls back to softpipe. All weak: the real one (userland/s8mali/s8mali_drm.c, IOKit
 * transport, plus the syncobj emulation) replaces this file when it is linked in.
 */
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "xf86drm.h"

#define W __attribute__((weak))

W int drmOpenWithType(const char *name, const char *busid, int type) { errno = ENODEV; return -1; }
W int drmIoctl(int fd, unsigned long request, void *arg) { errno = ENOSYS; return -1; }
W int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size) { errno = ENOSYS; return -1; }
W int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size) { errno = ENOSYS; return -1; }
W drmVersionPtr drmGetVersion(int fd) { return NULL; }
W void drmFreeVersion(drmVersionPtr version) { }
W int drmGetCap(int fd, uint64_t capability, uint64_t *value) { errno = ENOSYS; return -1; }
W int drmCloseBufferHandle(int fd, uint32_t handle) { errno = ENOSYS; return -1; }
W int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) { errno = ENOSYS; return -1; }
W int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) { errno = ENOSYS; return -1; }
W int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) { errno = ENOSYS; return -1; }
W int drmSyncobjDestroy(int fd, uint32_t handle) { errno = ENOSYS; return -1; }
W int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count) { errno = ENOSYS; return -1; }
W int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled) { errno = ENOSYS; return -1; }
W int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { errno = ENOSYS; return -1; }
W int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd) { errno = ENOSYS; return -1; }
W int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd) { errno = ENOSYS; return -1; }
W int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd) { errno = ENOSYS; return -1; }
W int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled) { errno = ENOSYS; return -1; }
W int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags) { errno = ENOSYS; return -1; }
W void *hakios_drm_map(int fd, uint32_t handle, uint64_t size) { errno = ENOSYS; return MAP_FAILED; }
W void hakios_drm_unmap(int fd, uint32_t handle, void *address) { }
