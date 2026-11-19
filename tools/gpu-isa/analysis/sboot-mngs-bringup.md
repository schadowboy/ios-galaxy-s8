# S-Boot SMP / MNGS bring-up (static, sdb.img, 2026-11-12)
[E]=evidence (file offset / instruction) [G]=guess. Offsets are raw file offsets of backup/sdb.img.
Disassembly: llvm-objcopy -I binary -> ELF, llvm-objdump -d (VMA = file offset; ADRP/ADD/BL are PC-relative so they resolve to file offsets directly).

## 1. Base
- [E] File starts with a small header then code (0x18: ldr x20,[0x70]; 0x70 = 0x16480600 literal, tests bit31 of it). Strings are reached by PC-relative ADRP+ADD, e.g. 0x75d94-0x75d98 -> 0xf9688 "MNGS L2 power off : %d". So code and data are position independent; no absolute base needed for xrefs.
- [E] GOT-like pointer table at 0x139ee0..0x139f10 holds 0xC90979e0..0xC9097a10 = 0xC8FA2000 + string offsets (0xf59e0 read_clusterid, 0xf59f0 smp_init, 0xf5a00 smp_boot, 0xf5a10 smp_boot_test), 4/4 hits => nominal link base 0xC8FA2000 [G for code pointers: GOT code pointers e.g. 0x139b58=0xC9002288 map to file 0x60288 which is zero, so those are runtime-relocated; do not rely on it].
- [E] S-Boot runs at EL1 (mrs SCTLR_EL1 at 0x74218, MPIDR/CSSELR_EL1) and calls EL3 with plain `smc #0` after `dsb sy` (0x7403c).

## 2. The real SMP code (smp.c) -- function name strings 0xf59a8.. are __func__ for logs
Function locations via ADRP 0xf5000+0x9a8+k:
- smp_handler  0xc9bec (sets flag [0x42eb58+0xc..]) and secondary body 0xca110 (prints MIDR/MPIDR, sets [0x42eb60]=1, calls fn ptr, wfi)
- boot_exynos  0xc9cf4 (x1 = 0xf59a8+0x10)
- next_available_cpu 0xc9e0c (counter at [0x4c0 of page 0x125000], max 4, returns -1 when exhausted)
- read_clusterid 0xc9e54 (MPIDR_EL1 Aff1 -> stored in [0x42eb5c]); helper 0xc9e84 = Aff0
- smp_init 0xc9f4c, smp_boot 0xc9fb8, smp_boot_test 0xca078

### boot_exynos(cpu) @0xc9cf4-0xc9e08 -- the S-Boot way to power ONE core (used for the cluster S-Boot runs on)
1. [E] idx = cpu; if [0x42eb5c] (cluster id, Aff1) != 0: idx += clusterid*4  (0xc9d34-0xc9d40). Same formula as Linux CPU_CFG ((Aff1<<2|Aff0)).
2. [E] cfg = PMU(0x16480000) + 0x2000 + idx*0x80 (0xc9d44-0xc9d50: (idx+0x2c9040)<<7 = 0x16482000+idx*0x80). NOTE: 0x2c9040<<7 = 0x16482000, i.e. this S-Boot uses PMU = 0x16480000 and CONFIGURATION at +0x2000 -> matches smp_probe.c CPU_CFG.
3. [E] WRITE cfg |= 0xF (0xc9d54-0xc9d70).
4. [E] POLL (cfg+4) & 0xF == 0xF, 1000 iterations x delay(1) (0x7d698 = ms delay) (0xc9d78-0xc9d9c); timeout -> return -1.
5. [E] WRITE 0x0206F01C = *(GOT 0xb58) i.e. the secondary's entry address (0xc9da0-0xc9db0); this 0x0206xxxx page is SRAM/holding pen, NOT PMU. Then call 0xb0be8 (cache maintenance [G]), `dsb sy`, `sev` (0xc9db4-0xc9dbc).
6. [E] POLL [0x42eb60] != 0 (flag set by the secondary at 0xca154) 1000 x 1 ms (0xc9dc0-0xc9de0); timeout -> -1.
- No SMC is used in boot_exynos. No CPU_ON PSCI in the normal S-Boot SMP path. [E]
- smp_boot 0xc9fb8: stores the secondary function pointer to [GOT 0x6d8], calls smp_init(0xc9f4c: read_clusterid once, init), then loops next_available_cpu() -> boot_exynos() (0xca00c-0xca018). smp_init is called from the smp_boot_test path (0xca0b8) and sets [0x4c0]=1.
- 0xc9e90 (cpu off helper): cfg &= ~0xF then `smc` with x0=0x84000002 (PSCI CPU_OFF), x1=0x10000 (0xc9ea4-0xc9ef0). [E]
- Which cluster: only cores of the cluster S-Boot's boot CPU is in (idx offset by Aff1). [G] S-Boot normal flow never brings up MNGS cores; the next section is the only place it does.

## 3. MNGS (big) bring-up -- the crash/GPR-dump flow, function @0x75a08 (called from 0xabb6c after 0x74e00/0x74eb4 which check magic 0xcafe/0xdafe at 0x91200300 etc.)
Strings: "Dump GPR_EN & Watchdog or Warm Reset Detected", "Check LPI timeout status", "Core %d: Power Off at previous time", "ERROR: Core %d: failed to power on : 0x%x", "MNGS L2 power off : %d". Order, [E] with offsets:
1. 0x75a3c-0x75a48: READ PMU 0x16480500; if bit0 == 0 -> return (no dump).
2. 0x75a80: call 0x75944 (prints per-core status words, read only). 0x75a84-0x75a9c: READ 0x16480420, print; if bit4 set print message (0x75dd4) and continue.
3. 0x75aa4-0x75acc: WRITE 0x91200380 = 0 (S-Boot SRAM/DRAM state mask [G]); READ-MOD-WRITE 0x16482440 |= 0x30000. (0x2440 is in the 0x2404+0x20*c cluster-option block; [G] it is the MNGS/cluster L2-noncpu OPTION; same as Linux lpm option bits.)
4. 0x75ad0: call 0x75854: for each core reads 0x0206F028+4*i status and prepares per-core words in 0x91200480/0x91200510 area (software state) [G].
5. First CPU_ON loop, idx 4..N-1 (0x75ad4-0x75b80). The mpidr table copied from 0xef098 is {0x100,0x101,0x102,0x103,0x0,0x1,0x2,0x3}: idx 4..7 = MNGS cores. For each:
   - 0x75b04-0x75b10: read [0x91200510+4*(idx-4)] and [..-0x80]; if (&0x1c) != 0 print "Core %d: Power Off at previous time: 0x%x" (informational).
   - 0x75b14-0x75b30: SMC with x0=0xC4000003 (PSCI CPU_ON, built by mov x0,#3; movk #0xc400,lsl16), x1 = mpidr (0,1,2,3), x2 = [GOT 0x5e0] (entry address), x3 = 0 (wrapper 0x741a0 = mov x3,#0; smc). Return code x0.
   - rc==0: delay 10000 us (0x75d10, 0x7d760 usec delay), call 0x754e8(idx) and 0x753d0(idx) (debug unlock 0xC5ACCE55 into DBGLAR at core debug base, set up per-core dump slots) -> next core.
   - rc!=0: print "ERROR: Core %d: failed to power on : 0x%x", set bit idx in 0x91200380, next.
6. 0x75b84-0x75bf0: two SMCs with x0=0xFFFFFFFFFFFFFED2 (x1=2 then 1) = secure-memory base/size query (strings "get secure memory base addr/size error"); then call 0x76628.
7. 0x76628 (MNGS L2 check): READ 0x16482604 & 7 must be == 7 (L2 status ON) else returns -1 => the caller prints "MNGS L2 power off : %d" (0x75d90-0x75d9c). Then writes 0xC5ACCE55 to debug lock regs 0x17410fb0/0x17510fb0/0x17610fb0/0x17710fb0 and polls 0x16480f08..0x16480f14 (&0xff0000 == 0x70000). [E]
   So S-Boot checks L2 (cluster0 L2 status 0x2604, same as smp_probe CL_L2(0)) is ON AFTER CPU_ON of the MNGS cores; it does not power the L2 itself.
8. Second loop (0x75c44-0x75cc4), idx 0..N-1: cbz idx 0 -> 0x75e00 (self); others: tst status 0x1c, SMC CPU_ON again (x0=0xC4000003, x1=mpidr[idx], x2=[GOT 0xd60] second entry, x3=0), rc==0 -> poll 0x91200380 bit idx up to 1001 x 1 ms (0x75d2c-0x75d5c) -> 0x753d0(idx).
9. End (0x75cc8-0x75cd8): 0x75390(w0=0,w1=0): 0x16480500: if 0 -> 0xF0 else keep (re-arm); 0x751c8(0) then 0x74cd4:
   - 0x751c8(0): 0x1648041c &= ~3, 0x16480414 &= ~3; and (for arg 0) CPU OPTION regs 0x200c/0x208c/0x210c/0x218c &= ~0x1300, 0x244c &= ~0x2040000, 0x240c &= ~0x80010000.
   - 0x751c8(1) variant (0x75214-0x752d4, not seen called with 1 in this flow): the same regs |= those bits.
   - 0x74cd4: 0x2008/0x2088/0x2108/0x2188 = 0x10a; CONFIGURATION 0x2000/0x2080/0x2100/0x2180 &= ~0xF (power OFF cluster-0 cores 0-3).
10. [E] Nothing in S-Boot writes 0x16483c20, 0x165 45c0, 0x2488, DD regs 0x16840000 or the 0x2404/0x2604 option registers before CPU_ON; the L2/NONCPU domain is not touched by S-Boot except reading 0x2604 and 0x2440 |= 0x30000.

## 4. Compare with smp_probe.c mngs_probe
Our order: (a) QCH 0x3c20|=1 wait 0x3c24; (b) CLKSTOP 0x45c0|=1 wait 0x45c4; (c) DD4 |= 1; (d) 0x2488 &= ~1; (e) DD0 &= ~1; CONFIG(0) &= ~0xF; wait stat==0; (f) SMC CPU_ON(0xC4000003, mpidr 0, smp_entry, x3=0); (g) CONFIG(0) |= 0xF; wait stat==0xF; DD0 |= 1; (h) wait flag, CPU_ON again.
S-Boot (MNGS, dump flow): PMU 0x500 bit0 gate -> 0x2440 |= 0x30000 -> CPU_ON x0=0xC4000003 x1=mpidr x2=entry x3=0 with NO PMU CONFIG write of its own ->delay 10 ms -> debug-unlock/dump -> L2 status 0x2604&7==7 check.
Differences:
1. [E] S-Boot does NOT write 0x2000+idx*0x80 (CONFIGURATION) before/after CPU_ON for MNGS; it leaves power sequencing to EL3. We do power off -> CPU_ON -> power on (and a second CPU_ON): the 0->0xF toggle with CPU_ON in the middle may confuse EL3 (e.g. EL3 sees the core already powered when it processes the second CPU_ON, or our CONFIG=0 races the EL3 power-up). Try CPU_ON only (no CONFIG writes at all) after step (d)/(none).
2. [E] S-Boot writes PMU 0x16482440 |= 0x30000 before CPU_ON; we never write 0x2440. (Likely the cluster/L2 OPTION that lets the sequencer power the cluster; [G].)
3. [E] S-Boot (and cpu-off path 0x751c8(0)) clears 0x1648041c/0x16480414 low 2 bits and clears OPTION bits 0x1300 in 0x200c/0x208c/0x210c/0x218c (cpu0..3 OPTION), 0x244c bits 0x2040000, 0x240c bits 0x80010000 around the flow; we do none of it. The "set" variant (0x751c8(1)) is the inverse: if these OPTION bits are SET (reset-hold/ignore-power) the cores stay held. [G] check these bits read as 0 on our side before CPU_ON.
4. [E] Entry/ctx: S-Boot passes x3=0 and a normal DRAM entry from a GOT slot; we also x3=0 (matches). mpidr 0..3 for MNGS (matches).
5. [E] S-Boot waits 10 ms after CPU_ON returns 0 before touching the core (0x75d10: delay 10000 us); and polls up to 1 s for the core's flag. Our waits (100/300 ms) are longer, so timing is not the difference.
6. [E] S-Boot never touches QCH 0x3c20, CLKSTOP 0x45c0, DD 0x16840000, CPUSEQ 0x2488 for MNGS. Those writes (our a-d) are not required by S-Boot; [G] they come from Linux pmucal cluster0_on and may be wrong order/idempotent; try skipping.
7. [G] EL: S-Boot is at EL1 when it issues the SMC; our loader issues it from whatever EL we run at (MMU off). If our loader runs at EL2 the S-Boot-era EL3 monitor may route/return differently; check our current EL.
8. [E] Normal (non-dump) S-Boot SMP for the A53 cluster is PMU-only: cfg|=0xF, wait stat==0xF, write entry to SRAM 0x0206F01C, sev (boot_exynos). For MNGS S-Boot has no PMU-only path. If our core stays held, writing entry to 0x0206F01C and `sev` is the one S-Boot step we do not do (we never write 0x0206F01C) [G that it applies to MNGS].
