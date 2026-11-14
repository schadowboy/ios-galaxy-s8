#pragma once
#include <stdint.h>

/* Powers the touch rails, logs the CPU clock; returns the raw RTC bytes
 * (sec, min, hour, weekday, day, month, year, next) little-endian. */
uint64_t pmic_init(void (*kv)(const char *, uint64_t), void (*out)(const char *));

int pmic_read8(unsigned reg);
int pmic_write8(unsigned reg, uint8_t val);
void pmic_rails(void (*kv)(const char *, uint64_t), void (*out)(const char *), const char *tag);
