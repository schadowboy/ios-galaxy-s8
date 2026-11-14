/* SPDX-License-Identifier: GPL-2.0-only
 * Samsung ACPM DVFS plugin firmware (exynos8895_acpm_dvfs.fw), taken from the phone's own
 * recovery kernel. Proprietary: kept local, never committed.
 */
	.section .rodata
	.balign 4
	.global acpm_dvfs_fw
	.global acpm_dvfs_fw_end
acpm_dvfs_fw:
	.incbin "fw/dvfs.fw"
acpm_dvfs_fw_end:
