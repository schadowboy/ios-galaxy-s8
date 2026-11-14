#pragma once
#include <stdint.h>

/* Stage 0 Mali-G71 identity probe (boot-arg gpuprobe=1). flush() writes the loader log to USERDATA. */
void gpu0_flush(void);
void gpu0_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *), void (*flush)(void));
