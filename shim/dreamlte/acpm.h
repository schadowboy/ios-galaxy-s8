/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>

#define ACPM_FREQ_REQ		0
#define ACPM_FREQ_GET		1
#define ACPM_DVFS_IDX_MIF	0
#define ACPM_DVFS_IDX_CPUCL0	2	/* Mongoose */
#define ACPM_DVFS_IDX_CPUCL1	3	/* Cortex-A53 */
#define ACPM_DVFS_IDX_G3D	4

/* Read-only dump of the ACPM framework and its IPC channel table. */
void acpm_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* Load + attach the DVFS plugin (needs acpm_dvfs_fw[] linked in). 0 on success. */
int acpm_attach_dvfs(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* Read-only dump of the APM frequency/voltage map for the CPU clusters. */
void acpm_fvmap_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* Request the table level 'from_low' steps above the lowest (0 = lowest freq); waits 50 ms. */
int acpm_dvfs_level(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		    uint32_t domain, uint32_t from_low);
/* Request an exact rate in kHz, only if it is an operating point in the APM's table for the domain. */
int acpm_dvfs_rate(void (*kv)(const char *, uint64_t), void (*out)(const char *),
		   uint32_t domain, uint32_t khz);
/* Step the A53 cluster (domain CPUCL1) up through the APM's table levels to 'mhz'. Logs "A53FREQ:" lines.
 * Stops (leaving the frequency alone) at the first step whose PLL read-back does not match. 0 = reached. */
int a53_set_freq(void (*kv)(const char *, uint64_t), void (*out)(const char *), uint32_t mhz);
/* Boot-arg mngsfreq: step CPUCL0 (Mongoose) up through the fvmap levels to 'mhz'. "MNGS:" lines. 0 = reached. */
int mngs_set_freq(void (*kv)(const char *, uint64_t), void (*out)(const char *), uint32_t mhz,
		  void (*flush)(void));
/* Read-only dump of the APM firmware log ring. */
void acpm_log_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* Dump the APM plugin table (id, base, size, attached flags, firmware name). */
void acpm_plugins_dump(void (*kv)(const char *, uint64_t), void (*out)(const char *));
/* Send a 4-word command on an ACPM channel; response words replace cmd[]. 0 = ok. */
int acpm_send(uint32_t chan_id, uint32_t cmd[4], uint32_t timeout_ms);
/* Boot-arg dvfsch=<n>: send DVFS requests on channel n (default: firmware table lookup). */
void acpm_set_dvfs_channel(uint32_t ch);
/* Read-only FREQ_GET control experiment + 512 B SRAM dump on the override channel; flush() saves the log. */
void acpm_ch0_experiment(void (*kv)(const char *, uint64_t), void (*out)(const char *),
			 void (*flush)(void));
/* DVFS request on channel 5; logs the response. 0 = ok. */
int acpm_dvfs(void (*kv)(const char *, uint64_t), void (*out)(const char *),
	      uint32_t domain, uint32_t req, uint32_t khz);
