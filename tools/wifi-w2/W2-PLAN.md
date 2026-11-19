# W2a: host side of the BCM4361 MSGBUF protocol from the loader (MAC + version by iovar)

Status 2026-11-19: code written and compile-checked (loader flags, -Werror, 0 ABS64 relocations); NOT run on the phone.
Files: `tools/wifi-w2/wifi_w2.c` (source of truth), a copy appended to `shim/dreamlte/smp_probe.c` (markers
`BEGIN wifi_w2.c` / `END wifi_w2.c`; `tools/wifi-w2/_append.sh` re-appends after edits), 4-line hook in `shim/dreamlte/loader.c`
(`wifiprobe=9`). `_fetch.sh`/`_ref/` hold public Linux brcmfmac sources (data only, do not commit `_ref`); `_check.sh` = compile check.

Legend: [E] read in source (file named), [M] measured on the phone (logs/loader-wifi8.txt, logs/loader-v64.txt), [A] assumption.

## 1. What the firmware tells us [M]

W1b log: `pcie_shared` at TCM 0x1fc8b4 (the last RAM word, RAM = 0x170000..0x2C0000), word 0 = `0x30040007`:

| bits | meaning (bcmpcie.h of Samsung's tree) | consequence |
|---|---|---|
| 7:0 = 7 | PCIE_SHARED_VERSION 7 | ring_info has max_submission/completion; item sizes v7 (tx cpl 24, rx cpl 40) |
| 0x10000 clear | no PCIE_SHARED_DMA_INDEX | read/write indices live in TCM, the host needs no index arrays |
| 0x100000 clear | no 2BYTE_INDICES | index cells are 4 bytes apart (DHD `rw_index_sz` = 4), accessed as 16-bit at the cell start |
| 0x40000 set | PCIE_SHARED_D2H_SYNC_XORCSUM | each D2H item carries an epoch byte (hdr byte 3) and an XOR checksum so that the XOR of all its 32-bit words is 0 (`dhd_prot_d2h_sync_xorcsum`). The code verifies and logs it |
| 0x10000000 set | PCIE_SHARED_HOSTRDY_SUPPORT | host must ring doorbell 1 ("host ready") once rings are set up |
| 0x20000000 set | PCIE_SHARED_NO_OOB_DW | firmware has no OOB device-wake deep sleep: host announces HOSTCAP_DS_NO_OOB_DW |
| IDMA 0x400000, IFRM 0x1000000, DAR 0x80000000 clear | no implicit DMA / DAR doorbells | doorbells are the classic PCIe2 H2D mailbox registers (below) |

`pcie_shared` words seen: console 0x23f444 (word 5), fwid 0x2c4297ff (word 7). Everything else was 0 at that time (the host
fills the scratch/host_cap fields).

## 2. Structures, each with its source

`pciedev_shared_t` byte offsets [E bcmpcie.h:`pciedev_shared_t`, Linux pcie.c `BRCMF_SHARED_*_OFFSET`, cross-checked with the
W1b dump]: flags 0, trap_addr 4, assert_exp_addr 8, assert_file_addr 12, assert_line 16, console_addr 20, msgtrace_addr 24,
fwid 28, total_lfrag_pkt_cnt u16 32, max_host_rxbufs u16 34, dma_rxoffset 36, h2d_mb_data_ptr 40, d2h_mb_data_ptr 44,
rings_info_ptr 48, host_dma_scratch_buffer_len 52, host_dma_scratch_buffer (lo,hi) 56, host_scb_addr 64, host_scb_size 72,
buzz_dbg_ptr / trap buffer len 76, flags2 80, host_cap 84, host_trap_addr 88, device_fatal_logbuf_start 96, hoffload_addr 100.
NOTE: Linux brcmfmac writes a "ring update" buffer at 64/68; in v7 that is `host_scb_addr`, so the code deliberately does NOT.

`ring_info_t` [E bcmpcie.h]: ringmem_ptr 0, h2d_w_idx_ptr 4, h2d_r_idx_ptr 8, d2h_w_idx_ptr 12, d2h_r_idx_ptr 16,
h2d_w/h2d_r/d2h_w/d2h_r_idx_hostaddr (8 bytes each) 20/28/36/44 (unused without DMA_INDEX), max_tx_flowrings u16 52,
max_submission_queues u16 54, max_completion_rings u16 56, max_vdevs u16 58.

`ring_mem_t` (16 bytes, ring i at ringmem_ptr + 16*i) [E bcmpcie.h, dhd_bus_cmn_writeshared]: idx u16, type u8, rsvd u8,
max_item u16 @4, len_items u16 @6, base_addr lo @8, hi @12. Host writes base_addr, max_item, len_items (DHD `dhd_prot_ring_init`).

Index cells [E dhd_fillup_ring_sharedptr_info]: ring i (0,1 H2D): w = h2d_w_idx_ptr + 4*i, r = h2d_r_idx_ptr + 4*i;
ring 2,3,4 (D2H): w = d2h_w_idx_ptr + 4*(i-2), r = d2h_r_idx_ptr + 4*(i-2). 16-bit values; host zeroes them at init
(`RING_WR_UPD`/`RING_RD_UPD`). The H2D write index and D2H read index are written by the host, the others by the chip.

Common rings [E bcmpcie.h, bcmmsgbuf.h]: 0 H2D control submit (64 x 40), 1 H2D rx post (512 x 32), 2 D2H control complete
(64 x 24), 3 D2H tx complete (1024 x 24), 4 D2H rx complete (512 x 40). The same depths/sizes DHD uses, so the firmware sees
nothing unusual. Only rings 0 and 2 carry traffic in W2a.

Message header `cmn_msg_hdr_t` (8 bytes) [E bcmmsgbuf.h]: msg_type u8, if_id u8, flags u8, epoch u8, request_id u32.
H2D epoch = ring seqnum % 253, seqnum starts at 254 and increments per message (`dhd_prot_h2d_sync_init`, `dhd_fillup_ioct_reqst`); flags = 0
because HOSTCAP_H2D_VALID_PHASE is not announced. D2H: epoch must equal seqnum % 253 with seqnum starting at 254.

`ioctl_req_msg_t` (40 bytes) [E bcmmsgbuf.h, dhd_fillup_ioct_reqst]: hdr (type MSG_TYPE_IOCTLPTR_REQ = 0x09, request_id
0xFFFE), cmd u32 @8, trans_id u16 @12, input_buf_len u16 @14, output_buf_len u16 @16, rsvd u16[3], host_input_buf_addr
(lo,hi) @24, rsvd u32[2]. The payload lives in host RAM; the chip DMA-reads it. DHD sets input_buf_len = output_buf_len = len.
`ioctl_resp_evt_buf_post_msg_t` (40 bytes, MSG_TYPE_IOCTLRESP_BUF_POST = 0x0B): hdr (request_id = our pktid), host_buf_len
u16 @8, rsvd u16[3], host_buf_addr @16, rsvd u32[4]. The completion's request_id returns the pktid of the buffer that holds the reply.
D2H control items (24 bytes): ack MSG_TYPE_IOCTLPTR_REQ_ACK = 0x0A: hdr, status s16 @8, flow_ring_id u16 @10, cmd @12, rsvd, marker;
completion MSG_TYPE_IOCTL_CMPLT = 0x0C: hdr, status s16 @8, flow_ring_id @10, resp_len u16 @12, trans_id u16 @14, cmd @16, rsvd, marker.
Reply data starts at byte 0 of the posted buffer. Linux msgbuf.c uses the same layouts (checked).

iovar encoding [E wlioctl_defs.h]: WLC_GET_VAR = 262; input buffer = "name\0"; reply (6 bytes MAC / text) in the response buffer.

Doorbells, PCIe2 core rev 23 < 64 [M erom: core 0x83c rev 23 at 0x18003000; E bcmdhd pcie_core.h: PCIH2D_MailBox 0x140,
PCIH2D_DB1 0x144; PCIMailBoxInt(rev<64) 0x48, mask 0x4C; Linux uses the same for rev < 64]. DB0 (0x140) = "I wrote a ring",
value ignored (DHD 0x12345678). DB1 (0x144) = host ready (DHD `dhd_bus_hostready`: 0x12345678; Linux 1). DAR is off, so no DAR_* addresses.
The BAR0 window (config 0x80) is set to 0x18003000 for the whole run (the chip's own registers are reached through it; TCM is
reached through BAR2 independently).

host_cap (`dhd_set_host_cap`) = 7 (API version) | HOSTRDY 0x400 | DS_NO_OOB_DW 0x1000 | UR_FW_NO_TRAP 0x800000, at pcie_shared+84.
(UR_FW_NO_TRAP: firmware does not trap if one of its DMAs gets an Unsupported Request: a failed inbound path then shows as a
timeout plus log lines instead of a trap. Override with -DW2_HOSTCAP=...)

Order [E dhd_prot_init, then dhd_msgbuf_rxbuf_post_ioctlresp_bufs, dhd_fillup_ioct_reqst]: host_cap -> scratch buffer (len 8)
-> ring_mem of 5 rings + zero index cells -> HOST READY (DB1) -> post ioctl response buffers (ring 0, DB0) -> ioctl request (ring 0, DB0)
-> poll ring 2 write index. Event buffers and rx buffers are not posted (not needed for an ioctl; DHD posts them for traffic/events).

## 3. Inbound path for the chip's DMA: nothing to program [A1, strong evidence]

* Samsung's pci-exynos.c programs only OUTBOUND ATU regions (cfg0/cfg1/mem; no inbound anywhere), and `enable_cache_cohernecy`
  is a no-op for ip-ver 0x889500 (`ip_ver > 0x889500` test), so DMA is plain non-coherent and PCI address == CPU physical address.
* exynos8895.dtsi `pcie0@116A0000` has no iommu/sysmmu property; Samsung's bcmdhd sets `DMA_BIT_MASK(64)` on the PCI device and passes
  `dma_alloc_coherent` physical addresses straight into the ring/ioctl structures (dhd_pcie_linux.c:293).
* The loader runs with the MMU off (SCTLR 0x30c5183e: M = 0): CPU data accesses are Device-nGnRnE, so there is no cache to clean and
  every store is in the DRAM when the chip reads it (the code still issues `dsb sy` and a `dc civac` sweep).
* Bus master + memory space are enabled in the chip's command register (0x6, wifi_link), and the RC command is 0x107.
* If the chip's reads/writes were dropped, the symptoms are in section 6.

Region: 1 MiB reservation concept, 0x18000 bytes used at CPU 0x8c000000 (low bank; the S-Boot initrd is 0x89000000..0x8a000000, the DTB
0x8a000000 [M]). [A2] nothing else uses 0x8c000000..0x8c018000 while the loader runs; XNU does not use the low bank (its ADT window is the
high bank). Layout: 0x0000 ring0, 0x1000 ring1, 0x5000 ring2, 0x6000 ring3, 0xC000 ring4, 0x11000 scratch, 0x12000 ioctl request
payload, 0x14000 / 0x16000 two 8 KiB response buffers. Change with `-DW2_DMA_BASE=...` (the high bank is possible too: bcmdhd uses a
64-bit mask on Android; the low bank is the conservative choice).

## 4. Edit points

1. `shim/dreamlte/loader.c` (done, 4 lines, inside the `wp >= 3` branch after `wifi_link`): `if (wp == 9) wifi_w2(kv, out, gpu0_flush);`.
   `wifiprobe=9` therefore runs the whole W1/W1b sequence (wifi_link stage >= 8 includes stages 7 and 8: halt, download, boot) and then W2a.
   The boot-arg is a single digit so 9 parses.
2. `shim/dreamlte/smp_probe.c`: `wifi_w2.c` appended at the end (done via `tools/wifi-w2/_append.sh`; idempotent). `build-v141.sh`-style scripts
   copy only start.S / smp_probe.c / loader.c into /root/work/upstream, which is why it lives in smp_probe.c and not in a new file.
   (If a separate object is preferred: add `$CC $CFLAGS -c wifi_w2.c` and the object to `shim/dreamlte/build.sh` and to the copy list.)
3. Image: copy `tools/build-v132.sh` (it already passes `wifiprobe=8`), change `wifiprobe=8` to `wifiprobe=9`, new image name, e.g. `s8-ios-boot-v1xx-wifi9`.
Compile check done: `tools/wifi-w2/_check.sh full` -> wifi_w2.c, smp_probe.c, loader.c compile with the loader flags (-Wall -Wextra -Werror), 0 ABS64.

## 5. Test procedure

1. Build the image with `wifiprobe=9` (keep `s8recov=` short per the reset-timeout rule, <= 180 s; W2a itself needs < 10 s after the firmware boot, worst case ~7 s: two ioctl timeouts of 2.5 s).
2. Boot; loader log lines are appended to the ramoops console; after the run pull the log with `tools\pull-loaderlog.ps1` (as for wifi8).
3. Read in this order: `W2: ELBI link state`, `last RAM word`, flags decode, ring_info, `RAM test word`, `ring written`, `chip read the posted buffers`,
   `chip consumed the request`, `D2H ctrl item`, `MAC ADDRESS`, `FIRMWARE VERSION`, `W2a RESULT` (bit0 MAC, bit1 version), console tail.
4. If it hangs (reset to TWRP after ~40 s) the last flushed line names the step: each risky step is announced and flushed first.

## 6. How a failed log tells the failure modes apart

| observation | meaning |
|---|---|
| `ABORT: no pcie_shared pointer` | stage 8 did not run/boot in this boot (not W2) |
| `ABORT: ... only version 7 / TCM indices` | firmware flags differ from W1b |
| `ring written ... readback` differs | TCM write path or structure offsets wrong (ring_mem/ring_info layout assumption) |
| `chip read the posted buffers` = 0 and `H2D ctrl read index` stays 0, MAILBOXINT unchanged | chip did not process the ring: doorbell/host-ready not received (register offsets A3), or the chip could not DMA-read the ring (inbound A1); distinguish with the console tail (the firmware prints its pciedev/host-ready messages and, with UR_FW_NO_TRAP, DMA errors) |
| read index advanced but no D2H item, flags trap set | firmware trapped while handling the request: `trap_t` words + console tail |
| read index advanced, no D2H item, no trap, D2H write index 0 | chip read the request but its DMA WRITE to host RAM failed/was dropped (inbound writes, region A2) or the response is not on ring 2 |
| D2H item present but `failed epoch/xor check` | DMA of the item incomplete or sync-mode assumption wrong (read is still decoded) |
| ACK seen (status != 0) / completion status != 0 | iovar name/length problem (BCME code), the transport works |
| completion OK, MAC all zero / odd `resp_len` | response buffer offset assumption (dma_rxoffset); the log prints `dma_rxoffset` and the raw words |
| W1b stage numbers fine but `EP command` not 0x6 | bus master lost after the firmware boot (code re-enables it and logs it) |

## 7. Assumptions and unknowns (every one is visible in the log)

* A1 inbound pass-through, A2 free low-bank RAM (above); A3 doorbell/host-ready register values taken from bcmdhd for corerev < 64 (rev 23 measured, not the
  layout of its register file: if nothing reacts to DB0/DB1, test `W2_REG_DB0/DB1` alternatives such as the config-space SBMbx 0x98 mailbox write that
  Linux uses for D3/deep-sleep messages).
* A4 H2D flags/epoch: epoch counters mimic DHD; if the firmware had HOSTCAP_H2D_VALID_PHASE-like checking enabled by default it would drop items (visible as no consumption).
* A5 `D2H dma_rxoffset` (shared word 9) is not added to control items (DHD applies it to rx data only); value is logged.
* A6 trap_t layout of 20 words is a guess; only used for the failure dump.
* A7 `dc civac` loops are harmless with the MMU off (loader already does `dc civac` elsewhere).
* Not covered: events, flow rings, data path (W2b), MSI/INTx (we poll), power management (L1 is not enabled on the EP: ASPM bits untouched).

## 8. Risks (bus hangs)

New accesses compared with wifi_link/stage 8: (a) BAR0 window switched to 0x18003000 and reads/writes of PCIe2-core registers 0x48, 0x4C (read), 0x140, 0x144 (write) -
the PCIe2 core is the endpoint's own bus-facing core and always clocked, same path as ChipCommon/CR4 wrapper accesses. (b) 16/32-bit TCM accesses in the ring/index
areas, all validated to be inside 0x170000..0x2C0000 before use. (c) ~100 KiB of CPU stores to 0x8c000000.. (DRAM, ordinary). No PMU, PHY, DBI or ATU register is touched
(PMU+0x720 and PHY-before-0x71c remain avoided). A hang would be a watchdog return to TWRP after about 40 s; every register step is flushed before it runs.
