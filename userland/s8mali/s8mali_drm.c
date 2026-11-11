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

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

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

static int
submit(struct drm_panfrost_submit *request)
{
	uint64_t chain = request->jc, seq;
	int error = call(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, request, sizeof(*request));

	if (error != 0) {
		return error;
	}
	seq = request->jc; /* the kernel gives the job's number where the chain's address was */
	request->jc = chain;
	pthread_mutex_lock(&lock);
	submitted = seq > submitted ? seq : submitted;
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

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;
	kern_return_t kr;

	(void)fd;
	(void)size;
	kr = IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, S8_kIOMapAnywhere);
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
