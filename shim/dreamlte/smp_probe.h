#pragma once
#include <stdint.h>

void mngs_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
void mngs_probe_x(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		  void (*flush)(void), unsigned mode);
void smp_probe(void (*kv)(const char *, uint64_t), void (*out)(const char *));
