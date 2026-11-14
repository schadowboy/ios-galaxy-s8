#pragma once
#include <stdint.h>

void ufs_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *),
	       uintptr_t dma_base);

/* Write log text to USERDATA (16 blocks before XNU's ring); 0 = ok. */
int ufs_write_log(const void *buf, uint32_t len);
