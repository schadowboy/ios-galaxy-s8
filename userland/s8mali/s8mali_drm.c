/*
 * s8mali_drm: Linux's DRM panfrost requests as calls of the S8Mali IOKit user client (Mali-G71, Galaxy S8).
 * Adapted from hakios.drm.c of the HakiOS Galaxy S9+ project, Copyright (c) 2025-2026 the HakiOS authors,
 * MIT License (full text in s8mali_drm.h). A request's number (low byte of the ioctl code) is the external
 * method selector, its structure goes in and comes back as it is; a buffer's memory is the IOKit client memory
 * of the type that is the buffer's handle. SUBMIT hands back a job number in jc (the shim restores jc and keeps
 * the number: s8mali_last_seq()); selector 0x7d waits for a job number.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>

#include "s8mali_drm.h"

/* the ONE place that names the kernel service */
#define SERVICE_NAME "S8Mali"

/* IOKit is private on iOS: the few functions needed, declared here (the SDK's IOKit.framework stub provides them) */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, void *matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);
#define S8_kIOMapAnywhere 1
#define S8_kIOReturnNoMemory 0xe00002bd
#define S8_kIOReturnBadArgument 0xe00002c2
#define S8_kIOReturnTimeout 0xe00002d6
#define S8_kIOReturnBusy 0xe00002d5
#define S8_kIOReturnNotFound 0xe00002f0
#define S8_kIOReturnUnsupported 0xe00002c7

static const struct service {
	const char *name; /* Linux's driver name, what drmGetVersion() says (Mesa picks its backend by it) */
	int major, minor;
	const char *desc;
} panfrost = { "panfrost", 1, 2, "Mali-G71 on Darwin (S8Mali)" };

static bool opened;
static io_connect_t connection;
static int connection_fd = -1;
static int last_kr;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;
	kern_return_t kr;

	(void)busid;
	(void)type;
	if (strcmp(name, panfrost.name) != 0) {
		errno = ENODEV;
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(SERVICE_NAME));
	if (service == 0) {
		errno = ENODEV;
		return -1;
	}
	kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
	last_kr = (int)kr;
	IOObjectRelease(service);
	if (kr != 0) {
		errno = EACCES;
		return -1;
	}
	opened = true;
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

/*
 * Wait up to `seconds` for the S8Mali service to be registered (the kernel brings it up some seconds after launchd starts
 * its jobs; Mesa would silently fall back to the CPU if it looked too early). 1 = found, 0 = not there.
 */
int
s8mali_wait_service(int seconds)
{
	for (int i = 0; i <= seconds; i++) {
		io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching(SERVICE_NAME));

		if (service != 0) {
			IOObjectRelease(service);
			return 1;
		}
		if (i < seconds) {
			sleep(1);
		}
	}
	return 0;
}

void
drmClose(int fd)
{
	if (fd >= 0) {
		close(fd);
	}
	if (opened) {
		IOServiceClose(connection);
		opened = false;
		if (connection_fd >= 0) {
			close(connection_fd);
			connection_fd = -1;
		}
	}
}

int
s8mali_last_kr(void)
{
	return last_kr;
}

int64_t s8mali_now_ns(void);

/* what the calls into the kernel service cost, by selector (0x100: mapping a buffer into the process): s8mali_stats() prints and clears */
static struct {
	uint64_t count, ns, most;
} stats[0x101];

static void
stat_add(unsigned number, int64_t began)
{
	uint64_t took = (uint64_t)(s8mali_now_ns() - began);
	unsigned at = number < 0x100 ? number : 0x100;

	stats[at].count++;
	stats[at].ns += took;
	if (took > stats[at].most) {
		stats[at].most = took;
	}
}

/* "0x42 x3 120us (most 80us); ..." of every selector used since the last call; returns the length */
size_t
s8mali_stats(char *out, size_t size)
{
	size_t used = 0;

	out[0] = 0;
	for (unsigned i = 0; i <= 0x100; i++) {
		int n;

		if (stats[i].count == 0) {
			continue;
		}
		n = snprintf(out + used, size - used, "%s%#x x%llu %lluus (most %lluus)", used ? "; " : "", i, (unsigned long long)stats[i].count,
		    (unsigned long long)(stats[i].ns / 1000), (unsigned long long)(stats[i].most / 1000));
		if (n < 0 || (size_t)n >= size - used) {
			break;
		}
		used += (size_t)n;
		stats[i].count = stats[i].ns = stats[i].most = 0;
	}
	return used;
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	int64_t began = s8mali_now_ns();
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	if (number == 0x40 && size >= 40) {
		/* jobs split by kind: 0xf0 = vertex/tiler chains (slot 1), 0xf1 = fragment chains (slot 0) */
		stat_add(0xf0 + (((const struct drm_panfrost_submit *)data)->requirements & 1), began);
	} else {
		stat_add(number, began);
	}
	last_kr = (int)result;
	switch ((uint32_t)result) {
	case 0:
		return 0;
	case S8_kIOReturnNoMemory:
		return ENOMEM;
	case S8_kIOReturnBadArgument:
		return EINVAL;
	case S8_kIOReturnTimeout:
		return ETIMEDOUT;
	case S8_kIOReturnBusy:
		return EBUSY;
	case S8_kIOReturnNotFound:
		return ENOENT;
	case S8_kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

/*
 * A line of text into the kernel's log through the S8Mali service (selector 0x70, patch 0158): the kernel mutes its text console
 * when the screen comes up, but the lines it prints itself go on arriving, and tools/pull-kernlog.ps1 reads them within seconds.
 * 0: sent; -1: the service is not open (or refused): the caller falls back to the console.
 */
int
s8mali_log(const char *text, size_t n)
{
	char buf[240];

	if (!opened) {
		return -1;
	}
	if (n > sizeof(buf) - 1) {
		n = sizeof(buf) - 1;
	}
	memcpy(buf, text, n);
	buf[n] = 0;
	return IOConnectCallStructMethod(connection, 0x70, buf, n + 1, NULL, NULL) == 0 ? 0 : -1;
}

int64_t
s8mali_now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* Mesa's deadlines are moments of CLOCK_MONOTONIC; the kernel's waits take a length of time. */
static int64_t
left_ns(int64_t deadline)
{
	int64_t now = s8mali_now_ns();

	return deadline > now ? deadline - now : 0;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t submitted; /* the last job number the kernel gave */
static uint64_t finished;  /* a job known to be done, and with it every one before it */

uint64_t
s8mali_last_seq(void)
{
	uint64_t s;

	pthread_mutex_lock(&lock);
	s = submitted;
	pthread_mutex_unlock(&lock);
	return s;
}

int
s8mali_wait_seq(uint64_t seq, int64_t deadline)
{
	struct s8mali_wait_seq request;
	bool done;
	int error;

	pthread_mutex_lock(&lock);
	done = seq <= finished;
	pthread_mutex_unlock(&lock);
	if (done) {
		return 0;
	}
	do {
		request.seq = seq;
		request.timeout_ns = left_ns(deadline);
		error = call(S8MALI_SEL_WAIT_SEQ, &request, sizeof(request));
	} while (error == ETIMEDOUT && left_ns(deadline) > 0);
	if (error == 0) {
		pthread_mutex_lock(&lock);
		finished = seq > finished ? seq : finished;
		pthread_mutex_unlock(&lock);
	}
	return error;
}

static uint64_t *syncobj(uint32_t handle);

/*
 * A job chain to the GPU. Linux's driver gives back nothing and puts the job's fence into the sync object out_sync;
 * S8Mali gives back the job's number where the chain's address was (jc). The address is put back, as Linux leaves it,
 * and the number goes into the sync object. The fences to wait for before the job (in_syncs) are looked at neither here
 * nor by the kernel: they can only stand for jobs given before this one, which the queue has run by the time this one
 * begins.
 */
static int
submit(struct drm_panfrost_submit *request)
{
	uint64_t chain = request->jc, seq, *fence;
	int error = call(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, request, sizeof(*request));

	if (error != 0) {
		return error;
	}
	seq = request->jc; /* the kernel gives the job's number where the chain's address was */
	request->jc = chain;
	pthread_mutex_lock(&lock);
	submitted = seq > submitted ? seq : submitted;
	if (request->out_sync != 0 && (fence = syncobj(request->out_sync)) != NULL) {
		*fence = seq;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static int
wait_buffer(struct drm_panfrost_wait_bo *request)
{
	struct drm_panfrost_wait_bo asked = *request;
	int error;

	do {
		asked.timeout_ns = left_ns(request->timeout_ns);
		error = call(DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO, &asked, sizeof(asked));
	} while (error == ETIMEDOUT && left_ns(request->timeout_ns) > 0);
	return error;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	int error;

	(void)fd;
	if (!opened) {
		errno = ENODEV;
		return -1;
	}
	if (number == DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT) {
		error = submit(arg);
	} else if (number == DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO) {
		error = wait_buffer(arg);
	} else {
		error = call(number, arg, IOCPARM_LEN(request));
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version;

	(void)fd;
	if (!opened) {
		errno = ENODEV;
		return NULL;
	}
	version = calloc(1, sizeof(*version));
	if (version != NULL) {
		version->version_major = panfrost.major;
		version->version_minor = panfrost.minor;
		version->name = strdup(panfrost.name);
		version->name_len = (int)strlen(panfrost.name);
		version->date = strdup("");
		version->desc = strdup(panfrost.desc);
		version->desc_len = (int)strlen(panfrost.desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

/* As libdrm's: the driver-private request number is added to DRM_COMMAND_BASE; 0, or minus an errno. */
int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* No generic DRM capability (syncobj, prime, ...) is offered: every one reads 0. */
int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* ---- sync objects (after the S9+ project's emulation, MIT) ------------------------------------------------------ */

/*
 * Linux's fences that a program holds by a handle. Here, for panfrost, each is the number of the job it waits for, kept
 * in this process: the kernel has no fences, it has numbers. 0 is no job, nothing to wait for (a sync object made
 * signalled, or without a fence). Waiting for several is waiting for the highest of their numbers (all of them) or the
 * lowest (any), since the jobs end in order.
 */
#ifndef DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#endif
#define SYNCOBJ_FREE UINT64_MAX
static uint64_t *syncobjs; /* handle n is syncobjs[n - 1]: its job, or SYNCOBJ_FREE */
static uint32_t syncobj_count;

/* (with the lock held) NULL: no such sync object */
static uint64_t *
syncobj(uint32_t handle)
{
	if (handle == 0 || handle > syncobj_count || syncobjs[handle - 1] == SYNCOBJ_FREE) {
		return NULL;
	}
	return &syncobjs[handle - 1];
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	uint32_t i;

	(void)fd;
	(void)flags; /* (signalled or without a fence: no job either way) */
	if (!opened) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (i = 0; i < syncobj_count && syncobjs[i] != SYNCOBJ_FREE; i++) {
	}
	if (i == syncobj_count) {
		uint32_t count = syncobj_count != 0 ? 2 * syncobj_count : 64;
		uint64_t *grown = realloc(syncobjs, count * sizeof(*grown));

		if (grown == NULL) {
			pthread_mutex_unlock(&lock);
			errno = ENOMEM;
			return -1;
		}
		for (uint32_t k = syncobj_count; k < count; k++) {
			grown[k] = SYNCOBJ_FREE;
		}
		syncobjs = grown;
		syncobj_count = count;
	}
	syncobjs[i] = 0;
	*handle = i + 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	uint64_t *fence;

	(void)fd;
	if (!opened) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) != NULL) {
		*fence = SYNCOBJ_FREE;
	}
	pthread_mutex_unlock(&lock);
	if (fence == NULL) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	uint64_t *fence;

	(void)fd;
	if (!opened) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		if ((fence = syncobj(handles[i])) != NULL) {
			*fence = 0;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* As libdrm's: 0, or minus an errno (ETIME: not done by the deadline, a moment of Mesa's clock). */
int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	uint64_t seq = 0, *fence;
	unsigned first = 0;
	int error;

	(void)fd;
	if (!opened) {
		return -ENOSYS;
	}
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < count; i++) {
		if ((fence = syncobj(handles[i])) == NULL) {
			pthread_mutex_unlock(&lock);
			return -EINVAL;
		}
		if (i == 0 || (all ? *fence > seq : *fence < seq)) {
			seq = *fence;
			first = i;
		}
	}
	pthread_mutex_unlock(&lock);
	error = s8mali_wait_seq(seq, timeout_nsec);
	if (error != 0) {
		return error == ETIMEDOUT ? -ETIME : -error;
	}
	if (first_signaled != NULL) {
		*first_signaled = first;
	}
	return 0;
}

/*
 * A sync object's fence as a file descriptor (a "sync file"): panfrost makes one of its context's sync object for every
 * fence it hands out, and takes it into a new sync object at once. Here it is another descriptor of /dev/null, with the
 * job number kept beside its number. A number that comes back that was not made here is taken for the last job given to
 * the GPU so far: later than the one meant, if anything, never earlier.
 */
#define SYNC_FILES 64
static struct {
	bool used;
	int fd;
	uint64_t seq;
} sync_files[SYNC_FILES];
static unsigned sync_file_next;

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	uint64_t *fence;
	int file;

	(void)fd;
	if (!opened) {
		return none();
	}
	if ((file = fcntl(connection_fd, F_DUPFD_CLOEXEC, 0)) < 0) {
		return -1;
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		close(file);
		errno = EINVAL;
		return -1;
	}
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == file) {
			sync_files[i].used = false; /* (a number made here before, closed since) */
		}
	}
	sync_files[sync_file_next % SYNC_FILES].used = true;
	sync_files[sync_file_next % SYNC_FILES].fd = file;
	sync_files[sync_file_next % SYNC_FILES].seq = *fence;
	sync_file_next++;
	pthread_mutex_unlock(&lock);
	*sync_file_fd = file;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	uint64_t *fence, seq;

	(void)fd;
	if (!opened) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		errno = EINVAL;
		return -1;
	}
	seq = submitted;
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == sync_file_fd) {
			seq = sync_files[i].seq;
		}
	}
	*fence = seq;
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * Buffers as file descriptors for other programs: there are none. Memory somebody else made does come in, as the S9+ shim
 * does it: an IOSurface, by its ID, where Linux's driver expects a dma-buf descriptor (Mesa's pan_kmod_bo_import ->
 * drmPrimeFDToHandle(fd, surface_id), with the S9+ hunks of panfrost-darwin.patch ported as our Mesa patch 0006). The kernel
 * selector 0x7e is S8Mali's since kernel patch 0163 (s8mali_drm.h, struct s8mali_surface); on a kernel without it this fails
 * with ENOSYS from the kernel's kIOReturnUnsupported, which is what Mesa's caller expects of a surface it could not make a
 * buffer of (it then copies). Exporting a buffer (drmPrimeHandleToFD) stays unsupported: nobody needs it.
 */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	return none();
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	struct s8mali_surface request;
	int error;

	(void)fd;
	if (!opened) {
		return none();
	}
	memset(&request, 0, sizeof(request));
	request.surface = (uint32_t)prime_fd; /* (the IOSurface's ID) */
	error = call(S8MALI_SEL_SURFACE, &request, sizeof(request));
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

int
s8mali_surface(int fd, struct s8mali_surface *surface)
{
	int error;

	(void)fd;
	if (!opened) {
		return none();
	}
	surface->handle = S8MALI_SURFACE_DESCRIBE;
	error = call(S8MALI_SEL_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

/*
 * Sync objects as file descriptors (what EGL's fence fd and Linux's syncobj fd are): here, as the sync files above, another
 * descriptor of /dev/null with the job number kept beside it. Both directions carry the same number.
 */
int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	int error = drmSyncobjCreate(fd, 0, handle);

	if (error != 0) {
		return error;
	}
	return drmSyncobjImportSyncFile(fd, *handle, obj_fd);
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	return drmSyncobjExportSyncFile(fd, handle, obj_fd);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	(void)handles;
	(void)points;
	(void)count;
	(void)timeout_nsec;
	(void)flags;
	(void)first_signaled;
	return none();
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	(void)fd;
	(void)dst_handle;
	(void)dst_point;
	(void)src_handle;
	(void)src_point;
	(void)flags;
	return none();
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;
	kern_return_t kr;

	int64_t began = s8mali_now_ns();

	(void)fd;
	(void)size;
	kr = IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, S8_kIOMapAnywhere);
	stat_add(0x100, began);
	last_kr = (int)kr;
	if (kr != 0) {
		return (void *)-1; /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}
