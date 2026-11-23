/* Host syntax-check stub (NOT used for the iOS build). */
#ifndef STUB_MACH_TIME_H
#define STUB_MACH_TIME_H
#include <stdint.h>
typedef struct mach_timebase_info {
	uint32_t numer;
	uint32_t denom;
} mach_timebase_info_data_t;
int mach_timebase_info(mach_timebase_info_data_t *info);
uint64_t mach_absolute_time(void);
#endif
