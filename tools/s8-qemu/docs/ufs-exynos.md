# exynos-ufs: register contract and behaviour (Agent C)

Device: `exynos-ufs` (SysBusDevice), source `qemu/hw/misc/ufs-exynos.c`. Test: `qemu/tests/ufs-test.c` + `run-ufs-test.sh`.
Base addresses: HCI 0x11120000 (0x200) + vendor HCI 0x11121100 (the device decodes one 0x2000 window at 0x11120000), UniPro 0x11110000 (0x8000, read-as-written, mmio 1), UFS protector 0x11130000 (0x100, mmio 2, only the loader reads UFSPRSECURITY).
IRQ: SPI 334 (Samsung DTS `interrupts = <0 334 0>`); level = IS & IE. All our software polls (IE=0), so the line never matters today.
Properties: `drive` (raw image = LUN 0 incl. GPT, 4096-byte blocks), `lun-offset`, `lun-size`, `byte-utrd`=on, `inverted-clr`=on, `booted`=on, `start-stopped`=off, `cap`=0x1303ff0f.

## Contract: what the drivers read/write/poll (sources: xnu 0028/0030/0034/0069/0124 sweet_ufs.c, shim/dreamlte/ufs_probe.c, Samsung ufs-exynos.c/ufshcd.c)

| Register | Off | Who | Access | Expected / ends which wait | Model |
|---|---|---|---|---|---|
| CAP | 0x00 | kernel (64AS check), loader | R | 0x1303ff0f (16 slots, bit 24 = 64-bit DMA) | const |
| VER | 0x08 | loader | R | 0x210 | const |
| IS | 0x20 | both | R, W1C | 0 at start; after each command both write back what they read (ack) | W1C; set UTRCS(0) on completion if UTRD.I=1 |
| IE | 0x24 | both | W 0 | 0 (polled) | stored, IRQ = IS&IE |
| HCS | 0x30 | both | R | must have DP(0)+UTRLRDY(1): 0xf from S-Boot | booted: 0xf |
| HCE | 0x34 | both | R | reads 0 while running (USE_OF_HCE); kernel ignores it | booted: 0; write 1 = Linux-style reset to HCS=0xe |
| UTRLBA/U | 0x50/0x54 | both | RW | written then read back; kernel refuses to ring a doorbell if the readback differs; 64-bit (DMA at 0x883a88000 on the phone) | stored |
| UTRLDBR | 0x58 | both | W 1, poll bit0 -> 0 | bit0 clears when the command finished (5 s kernel timeout) | slot executed synchronously inside the write |
| UTRLCLR | 0x5c | kernel (non-Exynos only) | W | inverted on Exynos: bit set = clear slot | implemented, unused |
| UTRLRSR | 0x60 | both | W 0 then 1, readback bit0 must be 1 | | stored |
| UTRLCNR | 0x64 | - | | | completion notification bits |
| UTMRL* | 0x70-0x80 | Linux only | | | TM request executes, resp 0x24, IS.UTMRCS(9) |
| UICCMD/ARG1-3 | 0x90-0x9c | Linux only (kernel/loader never issue UIC) | | GET/SET/PEER_*, LINKSTARTUP (sets HCS.DP, IS.ULSS), HIBERN8, power-mode SET (IS.UPMS, UPMCRS=1), ENABLE/RESET; result code 0 in ARG2[7:0]; every command sets IS.UCCS(10) | MIB store, defaults for lane/gear attrs |
| vendor +0x00/+0x04 TXPRDT/RXPRDT_ENTRY_SIZE | 0x1100 | loader read | R | 0xc | const-initial, r/w |
| vendor +0x40 UTRL_NEXUS_TYPE | 0x1140 | both | RMW `|= 1` before each doorbell | 0xffff at power-up | stored |
| vendor +0x50 SW_RST | 0x1150 | Samsung driver poll | R | 0 (self-clearing) | reads 0 |
| vendor +0x38 VS_IS | 0x1138 | - | W1C | | W1C |
| all other vendor, UniPro | | Samsung driver only | RW | read-as-written | stub |
| UFSP +0x10 UFSPRSECURITY | 0x11130010 | loader | R | 0xefe26492 (bits 20:19 = 0 -> 16-byte PRDT entries; ==3 would mean 128) | const, r/w |

### UTRD / UCD format (Exynos UFSHCD_QUIRK_BROKEN_DWORD_UTRD, `byte-utrd`)
UTRD (32 B in the list at UTRLBA, slot n at +32n): dw0 = CT<<28 | DD<<25 | I<<24 (DD 1 = host to device, 2 = device to host); dw2 low byte = OCS (device writes 0 = success, 7 = fatal); dw4/dw5 = UCD address; dw6 = respOffset<<16 | respLength (BYTES); dw7 = prdtOffset<<16 | prdtLength (BYTES; entries = length/16). PRDT entry 16 B: addr lo, addr hi, rsvd, DBC (bits 17:0 = bytes-1). With `byte-utrd=off` offsets are in dwords and PRDT length counts entries (standard UFSHCI).
All DMA goes through the system address space (above 4 GiB works, tested at 0x883a88000).

### UPIU handled
COMMAND (0x01; flags 0x40 = read, 0x20 = write; CDB at +16; expected length at +12), NOP OUT (0x00 -> NOP IN 0x20), QUERY REQUEST (0x16 -> 0x36). Response: [0]=0x21, [3]=tag, [6]=0 target success, [7]=SCSI status, [12..15]=residual, sense length at [32..33], 18 bytes fixed-format sense at [34..] on CHECK CONDITION. (The loader's last_sense() reads length at 30 and key at 34: debug print only.)

### SCSI target (LUN 0 + W-LUNs 0xd0/0xb0/0xc4/0x81)
TEST UNIT READY, REQUEST SENSE, INQUIRY (std + VPD 00/80/b0), MODE SENSE(6/10) (pages 08, 0a, 3f), MODE SELECT (accept), START STOP UNIT (to W-LUN 0xd0 with power condition 1 sets the device active), SYNCHRONIZE CACHE(10/16) = blk_flush, READ CAPACITY(10/16), READ/WRITE(6/10/12/16), VERIFY/UNMAP/PREVENT-ALLOW/READ BUFFER (no-ops), REPORT LUNS (LUN 0 only). Anything else: CHECK CONDITION 05/20/00. Out-of-range LBA: 05/21/00. Block size 4096, capacity = (drive size - lun-offset)/4096.
Queries: read descriptor (device 0x40 B, spec 2.10, 1 LU + 4 W-LUN; config; unit 0..7 (only 0 enabled); interconnect; strings 1-5; geometry 0x48; power; health), read/write attribute (array, bBootLunEn=1, bCurrentPowerMode=0x11), read/set/clear/toggle flag (fDeviceInit self-clears at once, i.e. init completes immediately). Unknown IDN 0xfa, bad index 0xfc, other 0xff.

## Not confirmed from sources (assumptions)
* Exact OCS/response/status combination real hardware returns for CHECK CONDITION (the phone showed OCS=7, resp=1, status=2 for the stopped LU before SSU; the model returns OCS 0, resp 0, status 2). The kernel retries TUR up to 3 times and does not read sense, so both work.
* Query response codes for invalid IDN/index/length (0xfa/0xfc/0xf9 follow the UFS spec numbering from memory), descriptor field contents beyond the fields the Samsung driver reads (manufacturer/product strings are made up: SAMSUNG / KLUCG4J1ED-B0C1).
* UIC MIB defaults (lane counts, gears) - only used by the Linux driver, never by our kernel/loader.
* `start-stopped` (LU not ready until W-LUN SSU) mirrors the phone log but is off by default.
* The kernel's interrupt use: none (IE=0 everywhere); IRQ line is wired but untested with a real guest.
