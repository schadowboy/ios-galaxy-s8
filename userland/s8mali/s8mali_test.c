/*
 * s8mali_test: exercises the whole S8Mali (Mali-G71) contract through the s8mali_drm shim.
 * Output goes to stdout and is appended to /var/mobile/s8mali_test.log (fallback /tmp/s8mali_test.log).
 * Job layout = osfmk/kern/s8_mali.c (patch 0130): header word4 = (type<<1)|1|(1<<16), next_job words 6-7,
 * WRITE_VALUE payload: address u64 words 8-9, type (6 = 32-bit immediate) word 10, immediate word 12.
 * MIT License, see s8mali_drm.h.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "s8mali_drm.h"

#define JOB_TYPE_NULL 1u
#define JOB_TYPE_WRITE_VALUE 2u
#define WV_IMMEDIATE_32 6u
#define FILL 0xa5a5a5a5u

static FILE *logf_;
static int failures;

static void
out(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	fflush(stdout);
	if (logf_) {
		fputs(buf, logf_);
		fflush(logf_);
	}
}

static void
verdict(const char *name, int ok)
{
	out("S8MALI-TEST: %s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok) {
		failures++;
	}
}

struct bo {
	uint32_t handle, size;
	uint64_t va;
	volatile uint32_t *cpu;
};

static int fd = -1;

static int
bo_create(struct bo *b, uint32_t size, uint32_t flags, const char *what)
{
	struct drm_panfrost_create_bo c = { .size = size, .flags = flags };
	struct drm_panfrost_mmap_bo m;
	struct drm_panfrost_get_bo_offset g;
	void *p;

	memset(b, 0, sizeof(*b));
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) != 0 || c.handle == 0) {
		out("  %s: CREATE_BO(%u, flags %u) failed errno=%d kr=%#x\n", what, size, flags, errno, (unsigned)s8mali_last_kr());
		return -1;
	}
	b->handle = c.handle;
	b->size = size;
	b->va = c.offset;
	out("  %s: handle %u size %u flags %u GPU VA %#llx (%s)\n", what, c.handle, c.size, flags, (unsigned long long)c.offset,
	    c.offset >= 0x2000000ull && c.offset + size <= 0x100000000ull ? "in 32MB..4GB" : "OUTSIDE 32MB..4GB");
	memset(&g, 0, sizeof(g));
	g.handle = c.handle;
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &g) == 0) {
		out("  %s: GET_BO_OFFSET %#llx%s\n", what, (unsigned long long)g.offset, g.offset == c.offset ? "" : " (DIFFERS)");
	} else {
		out("  %s: GET_BO_OFFSET errno=%d (optional)\n", what, errno);
	}
	memset(&m, 0, sizeof(m));
	m.handle = c.handle;
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m) != 0) {
		out("  %s: MMAP_BO errno=%d kr=%#x\n", what, errno, (unsigned)s8mali_last_kr());
		return -1;
	}
	out("  %s: MMAP_BO offset %#llx\n", what, (unsigned long long)m.offset);
	p = hakios_drm_map(fd, c.handle, size);
	if (p == (void *)-1) {
		out("  %s: map failed kr=%#x\n", what, (unsigned)s8mali_last_kr());
		return -1;
	}
	b->cpu = (volatile uint32_t *)p;
	out("  %s: mapped at %p\n", what, p);
	return 0;
}

static void
bo_free(struct bo *b)
{
	if (b->cpu) {
		hakios_drm_unmap(fd, b->handle, (void *)b->cpu);
	}
	if (b->handle) {
		drmCloseBufferHandle(fd, b->handle);
	}
	memset(b, 0, sizeof(*b));
}

static int
pattern_test(struct bo *b, const char *what, int whole)
{
	uint32_t words = b->size / 4, bad = 0, i;
	uint32_t lo = whole ? 0 : 0, hi = whole ? words : 1024;

	for (i = lo; i < hi; i++) {
		b->cpu[i] = i * 0x9e3779b1u + 0x5338414cu;
	}
	if (!whole) {
		b->cpu[words - 1] = 0x600dcafeu; /* and the last word */
	}
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = lo; i < hi; i++) {
		if (b->cpu[i] != i * 0x9e3779b1u + 0x5338414cu) {
			bad++;
		}
	}
	if (!whole && b->cpu[words - 1] != 0x600dcafeu) {
		bad++;
	}
	out("  %s: pattern %u words checked, %u bad\n", what, whole ? words : hi + 1, bad);
	return bad == 0;
}

static void
job_header(volatile uint32_t *j, uint32_t type, uint64_t next)
{
	for (int i = 0; i < 8; i++) {
		j[i] = 0;
	}
	j[4] = (type << 1) | 1u | (1u << 16);
	j[6] = (uint32_t)next;
	j[7] = (uint32_t)(next >> 32);
}

static void
job_write_value(volatile uint32_t *j, uint64_t next, uint64_t target, uint32_t value)
{
	job_header(j, JOB_TYPE_WRITE_VALUE, next);
	j[8] = (uint32_t)target;
	j[9] = (uint32_t)(target >> 32);
	j[10] = WV_IMMEDIATE_32;
	j[11] = 0;
	j[12] = value;
	j[13] = 0;
}

/* submit chain at 'jc', all BOs in handles[]; wait for number and for each BO. 0 = ok */
static int
run_chain(uint64_t jc, const uint32_t *handles, uint32_t n, const char *what)
{
	struct drm_panfrost_submit s = { .jc = jc, .bo_handles = (uint64_t)(uintptr_t)handles, .bo_handle_count = n };
	int e1, e2 = 0;
	uint32_t i;

	__asm__ volatile("dsb sy" ::: "memory");
	if (drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) != 0) {
		out("  %s: SUBMIT failed errno=%d kr=%#x\n", what, errno, (unsigned)s8mali_last_kr());
		return -1;
	}
	out("  %s: SUBMIT ok, job number %llu, jc restored %#llx\n", what, (unsigned long long)s8mali_last_seq(),
	    (unsigned long long)s.jc);
	e1 = s8mali_wait_seq(s8mali_last_seq(), s8mali_now_ns() + 2000000000ll);
	out("  %s: wait seq -> %d (kr %#x)\n", what, e1, (unsigned)s8mali_last_kr());
	for (i = 0; i < n; i++) {
		struct drm_panfrost_wait_bo w = { .handle = handles[i], .timeout_ns = s8mali_now_ns() + 2000000000ll };
		int e = drmIoctl(fd, DRM_IOCTL_PANFROST_WAIT_BO, &w) ? errno : 0;

		out("  %s: WAIT_BO handle %u -> %d\n", what, handles[i], e);
		if (e) {
			e2 = e;
		}
	}
	return (e1 || e2) ? -1 : 0;
}

int
main(void)
{
	static const struct {
		uint32_t id;
		const char *name;
	} params[] = {
		{0, "GPU_PROD_ID"}, {1, "GPU_REVISION"}, {2, "SHADER_PRESENT"}, {3, "TILER_PRESENT"}, {4, "L2_PRESENT"},
		{5, "STACK_PRESENT"}, {6, "AS_PRESENT"}, {7, "JS_PRESENT"}, {8, "L2_FEATURES"}, {9, "CORE_FEATURES"},
		{10, "TILER_FEATURES"}, {11, "MEM_FEATURES"}, {12, "MMU_FEATURES"}, {13, "THREAD_FEATURES"},
		{14, "MAX_THREADS"}, {15, "THREAD_MAX_WORKGROUP_SZ"}, {16, "THREAD_MAX_BARRIER_SZ"},
		{17, "COHERENCY_FEATURES"}, {18, "TEXTURE_FEATURES0"}, {19, "TEXTURE_FEATURES1"}, {20, "TEXTURE_FEATURES2"},
		{21, "TEXTURE_FEATURES3"}, {22, "JS0_FEATURES"}, {23, "JS1_FEATURES"}, {24, "JS2_FEATURES"},
		{38, "NR_CORE_GROUPS"}, {39, "THREAD_TLS_ALLOC"}, {40, "AFBC_FEATURES"},
	};
	struct bo small = {0}, big = {0}, heap = {0}, jobs = {0};
	uint32_t handles[2];
	drmVersionPtr ver;
	int ok, tries;

	logf_ = fopen("/var/mobile/s8mali_test.log", "a");
	if (!logf_) {
		logf_ = fopen("/tmp/s8mali_test.log", "a");
	}
	out("S8MALI-TEST: start pid %d uid %d\n", getpid(), getuid());

	for (tries = 0; tries < 30; tries++) { /* the service may appear after launchd starts us */
		fd = drmOpenWithType("panfrost", NULL, 0);
		if (fd >= 0) {
			break;
		}
		if (tries == 0) {
			out("S8MALI-TEST: no S8Mali service yet (errno %d kr %#x), retrying for 30 s\n", errno, (unsigned)s8mali_last_kr());
		}
		sleep(1);
	}
	verdict("open", fd >= 0);
	if (fd < 0) {
		out("S8MALI-TEST: done\n");
		return 1;
	}

	ver = drmGetVersion(fd);
	out("S8MALI-TEST: drmGetVersion %s %d.%d \"%s\"\n", ver ? ver->name : "(null)", ver ? ver->version_major : -1,
	    ver ? ver->version_minor : -1, ver ? ver->desc : "");
	verdict("version", ver && strcmp(ver->name, "panfrost") == 0 && ver->version_major == 1 && ver->version_minor >= 1);
	if (ver) {
		drmFreeVersion(ver);
	}

	{
		int prod_ok = 0, shader_ok = 0;

		for (unsigned i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
			struct drm_panfrost_get_param p = {.param = params[i].id};

			if (drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &p) != 0) {
				out("  param %2u %-24s refused errno=%d\n", params[i].id, params[i].name, errno);
				continue;
			}
			out("  param %2u %-24s = %#llx\n", params[i].id, params[i].name, (unsigned long long)p.value);
			if (params[i].id == 0) {
				uint64_t v = p.value > 0xffff ? p.value >> 16 : p.value; /* GPU_ID may be given whole */
				prod_ok = (v & 0xf00f) == 0x6000;
			}
			if (params[i].id == 2) {
				shader_ok = p.value == 0x17771777ull;
			}
		}
		verdict("get_param PROD_ID(0x6000 masked)", prod_ok);
		verdict("get_param SHADER_PRESENT(0x17771777)", shader_ok);
	}

	out("S8MALI-TEST: create/mmap\n");
	ok = bo_create(&small, 4096, PANFROST_BO_NOEXEC, "bo4k") == 0 && pattern_test(&small, "bo4k", 1);
	verdict("bo 4KiB create+mmap+pattern", ok);
	ok = bo_create(&big, 1 << 20, PANFROST_BO_NOEXEC, "bo1m") == 0 && pattern_test(&big, "bo1m", 1);
	verdict("bo 1MiB create+mmap+pattern", ok);
	ok = bo_create(&heap, 4 << 20, PANFROST_BO_HEAP | PANFROST_BO_NOEXEC, "heap4m") == 0 && pattern_test(&heap, "heap4m", 0);
	verdict("heap 4MiB create+mmap+pattern", ok);

	/* job BO (executable flag clear: descriptors are not shaders) */
	if (bo_create(&jobs, 4096, 0, "jobs") != 0) {
		verdict("job bo", 0);
		goto cleanup;
	}
	for (uint32_t i = 0; i < 1024; i++) {
		jobs.cpu[i] = 0;
	}
	handles[0] = jobs.handle;

	/* 1: WRITE_VALUE into the job BO's own data word (offset 0x800) */
	{
		uint64_t target = jobs.va + 0x800;
		const uint32_t magic = 0x5338414cu;

		jobs.cpu[0x800 / 4] = FILL;
		job_write_value(jobs.cpu, 0, target, magic);
		ok = run_chain(jobs.va, handles, 1, "write_value") == 0;
		out("  write_value: target word = %#x (expect %#x)\n", jobs.cpu[0x800 / 4], magic);
		out("  write_value: job header word0 exception_status = %#x\n", jobs.cpu[0]);
		verdict("write_value", ok && jobs.cpu[0x800 / 4] == magic);
	}

	/* 2: NULL job */
	{
		job_header(jobs.cpu + 0x100 / 4, JOB_TYPE_NULL, 0);
		ok = run_chain(jobs.va + 0x100, handles, 1, "null") == 0;
		out("  null: header word0 = %#x\n", jobs.cpu[0x100 / 4]);
		verdict("null_job", ok);
	}

	/* 3: chain of 2 WRITE_VALUE jobs: A -> B, A writes into the job BO, B into the 1 MiB BO (needs it in the BO list) */
	if (big.cpu) {
		uint64_t ta = jobs.va + 0x804, tb = big.va + 0x40;
		const uint32_t va_ = 0xc0ffee01u, vb = 0xc0ffee02u;

		jobs.cpu[0x804 / 4] = FILL;
		big.cpu[0x40 / 4] = FILL;
		job_write_value(jobs.cpu + 0x200 / 4, jobs.va + 0x300, ta, va_);
		job_write_value(jobs.cpu + 0x300 / 4, 0, tb, vb);
		handles[1] = big.handle;
		ok = run_chain(jobs.va + 0x200, handles, 2, "chain2") == 0;
		out("  chain2: A=%#x (expect %#x) B=%#x (expect %#x)\n", jobs.cpu[0x804 / 4], va_, big.cpu[0x40 / 4], vb);
		verdict("chain2", ok && jobs.cpu[0x804 / 4] == va_ && big.cpu[0x40 / 4] == vb);
	} else {
		verdict("chain2", 0);
	}

cleanup:
	bo_free(&jobs);
	bo_free(&heap);
	bo_free(&big);
	bo_free(&small);
	drmClose(fd);
	out("S8MALI-TEST: %s (%d failure%s)\n", failures ? "FAILURES" : "all passed", failures, failures == 1 ? "" : "s");
	out("S8MALI-TEST: done\n");
	if (logf_) {
		fclose(logf_);
	}
	return failures ? 1 : 0;
}
