/*
 * Implementation of the drm stand-in (xf86drm.h next to it) by a table of function pointers (s8_drm_ops.h). Every drm* call
 * calls the table's entry; with no table (or no entry) it fails as a system without DRM does (ENODEV/ENOSYS), so a Mesa
 * without a host falls back to softpipe. The host (userland/s8mali/s8gl_test.c) sets the table with s8_drm_set_ops() after
 * loading the library and before creating a context. The real implementation is userland/s8mali/s8mali_drm.c, IOKit
 * transport to the S8Mali service, plus the syncobj emulation.
 *
 * (Earlier this file was a set of weak functions meant to be overridden by the host's own symbols; dyld does not rebind
 * calls inside one image that way, so Mesa never saw the real GPU.)
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "xf86drm.h"
#include "s8_drm_ops.h"

static struct s8_drm_ops ops;

int
s8_drm_set_ops(const struct s8_drm_ops *table)
{
	if (table == NULL || table->size < offsetof(struct s8_drm_ops, OpenWithType)) {
		return -1;
	}
	memset(&ops, 0, sizeof(ops));
	memcpy(&ops, table, table->size < sizeof(ops) ? table->size : sizeof(ops));
	return 0;
}

#define NO(code, ...) do { errno = (code); return __VA_ARGS__; } while (0)

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	if (ops.OpenWithType) {
		return ops.OpenWithType(name, busid, type);
	}
	NO(ENODEV, -1);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	if (ops.Ioctl) {
		return ops.Ioctl(fd, request, arg);
	}
	NO(ENOSYS, -1);
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	if (ops.CommandWrite) {
		return ops.CommandWrite(fd, index, data, size);
	}
	NO(ENOSYS, -1);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	if (ops.CommandWriteRead) {
		return ops.CommandWriteRead(fd, index, data, size);
	}
	NO(ENOSYS, -1);
}

drmVersionPtr
drmGetVersion(int fd)
{
	if (ops.GetVersion) {
		return (drmVersionPtr)ops.GetVersion(fd);
	}
	return NULL;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (ops.FreeVersion) {
		ops.FreeVersion(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	if (ops.GetCap) {
		return ops.GetCap(fd, capability, value);
	}
	NO(ENOSYS, -1);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	if (ops.CloseBufferHandle) {
		return ops.CloseBufferHandle(fd, handle);
	}
	NO(ENOSYS, -1);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	if (ops.PrimeHandleToFD) {
		return ops.PrimeHandleToFD(fd, handle, flags, prime_fd);
	}
	NO(ENOSYS, -1);
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	if (ops.PrimeFDToHandle) {
		return ops.PrimeFDToHandle(fd, prime_fd, handle);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	if (ops.SyncobjCreate) {
		return ops.SyncobjCreate(fd, flags, handle);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	if (ops.SyncobjDestroy) {
		return ops.SyncobjDestroy(fd, handle);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	if (ops.SyncobjReset) {
		return ops.SyncobjReset(fd, handles, count);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	if (ops.SyncobjWait) {
		return ops.SyncobjWait(fd, handles, count, timeout_nsec, flags, first_signaled);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	if (ops.SyncobjFDToHandle) {
		return ops.SyncobjFDToHandle(fd, obj_fd, handle);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	if (ops.SyncobjHandleToFD) {
		return ops.SyncobjHandleToFD(fd, handle, obj_fd);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	if (ops.SyncobjImportSyncFile) {
		return ops.SyncobjImportSyncFile(fd, handle, sync_file_fd);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	if (ops.SyncobjExportSyncFile) {
		return ops.SyncobjExportSyncFile(fd, handle, sync_file_fd);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	if (ops.SyncobjTimelineWait) {
		return ops.SyncobjTimelineWait(fd, handles, points, count, timeout_nsec, flags, first_signaled);
	}
	NO(ENOSYS, -1);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	if (ops.SyncobjTransfer) {
		return ops.SyncobjTransfer(fd, dst_handle, dst_point, src_handle, src_point, flags);
	}
	NO(ENOSYS, -1);
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	if (ops.Map) {
		return ops.Map(fd, handle, size);
	}
	NO(ENOSYS, MAP_FAILED);
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	if (ops.Unmap) {
		ops.Unmap(fd, handle, address);
	}
}
