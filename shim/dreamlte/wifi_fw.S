/* Samsung BCM4361 firmware and NVRAM from the phone's own system partition. Proprietary: kept local, never committed. */
	.section .rodata
	.balign 4
	.global wifi_fw
	.global wifi_fw_end
	.global wifi_nvram
	.global wifi_nvram_end
wifi_fw:
	.incbin "fw/wifi_fw.bin"
wifi_fw_end:
	.balign 4
wifi_nvram:
	.incbin "fw/wifi_nvram.bin"
wifi_nvram_end:
	.balign 4
	.global wifi_clm
	.global wifi_clm_end
wifi_clm:
	.incbin "fw/wifi_clm.bin"
wifi_clm_end:
