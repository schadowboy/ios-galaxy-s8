# launchd ~71% and mediaserverd ~49% idle CPU - static analysis (sweetlog-v29b)

Static only, phone untouched. Cache = iPhone X SystemOS cryptex dyld_shared_cache_arm64 (apfs-fuse /mnt/hp2sys, now unmounted), launchd/mediaserverd from
ios16-root.img (/mnt/hp2ro, unmounted). Scripts: tools/hp2-setup.sh, hp2-clean.sh, hp2-ro.sh, hp2-img.py/near.py/adis.py (copies of aks-* with the hp2 mount),
hp2-mdis.py (raw Mach-O disassembly, offsets from mh), hp2-a..e.sh. Unslid = runtime - 0x31eb8000.
Tags: [E] evidence (disassembly / log), [G] guess.

## 1. Symbolication [E]

| unslid | image!symbol | meaning |
|---|---|---|
| 0x1c26114f0 | libsystem_kernel `___workq_kernreturn`+8 (`mov x16,#0x170; svc`) | return point of the workqueue-thread park/return syscall |
| 0x1c2611538 | `_kevent_id`+8 (`x16=0x177`, kevent_id = kevent_workloop) | |
| 0x1c2612fb8 | `___sandbox_ms`+8 (0x17d) (lr libsystem_sandbox `sandbox_extension_issue_file_to_process`+0x...) | sandbox syscall |
| 0x1c2610ee0 | `__kernelrpc_mach_port_deallocate_trap`+8 (-0x12) (lr `_mach_port_deallocate`+0x18) | |
| 0x1c2612840 | `___proc_info`+8 (0x150) | |
| 0x1c2611030 | `_mach_msg2_trap`+8 (x16=-0x2f) (lr 0x1c2622b18 = `_mach_msg2_internal`+0x4c) | mach_msg send/receive |
| 0x1c261135c | `_mach_absolute_time` fallback `mov w16,#-3; svc` (mislabelled `_mach_absolute_time_kernel`); lr 0x1c2611624 is inside `mach_get_times` (0x1c26115e8) | kernel-trap time read |
| 0x1c2610f28 / 0x1c2610f34 | `__kernelrpc_mach_port_construct_trap` (-0x18) / `_destruct_trap` (-0x19); lr = `_mach_port_construct`+0x24 / `_mach_port_destruct`+0x24 | |
| 0x1d1b9a718/720, 0x1d1b9ac1c | libsystem_pthread `_start_wqthread`, `__pthread_wqthread`+0x168 | workq thread trampoline / call to workq_kernreturn |
| 0x1d1b9c0ec, 0x1d1b9a72c | `__pthread_start`+0x74, `_thread_start` | plain pthread base frames |
| 0x1c2622db8 / 0x1c2611524 | `_mach_msg_overwrite`, `_mach_msg` | |
| 0x1d1c022f0 / 0x1d1c0186c | libxpc `_xpc_pipe_try_receive`+0x1d0 / `__xpc_pipe_interface_routine`+0x164 | client side of xpc_pipe_routine (send request to launchd, wait reply) |
| 0x1d1be4358, 0x1d1be4a60 | libxpc, two functions: the one at 0x1d1be42a4 calls `__xpc_pipe_interface_routine` (dict key `pre-exec`, `xpc-fault`, `error`) = `_xpc_domain_routine`-style wrapper; the one at 0x1d1be4958 builds a dict {type, handle, **lookup-handle**, flags, **name**, instance, cputypes}, calls `_xpc_domain_routine(0x324, dict)` and then reads `port` and `non-launching` from the reply | **launchd service lookup (xpc_look_up_endpoint / bootstrap-style lookup of a named Mach service), routine 0x324** |
| 0x1d1bff1b4/1fc/234 -> 0x1d1bef030 -> 0x1d1bedd8c -> 0x1d1bedcbc | libxpc port-creation helper <- connection internals <- `_xpc_connection_resume`+0x5c <- `_xpc_connection_activate`+0x184 | chain of the `mach_port_construct` samples: an xpc connection is being activated |
| 0x1d1c01b80, 0x1d1c0676c | libxpc `_xpc_pipe_routine_reply`+0x5c ... | seen in launchd samples: launchd replying to a pipe routine |
| 0x18db56454 lr, chain 0x18db55f9c 0x18db55e7c 0x18db399b0 0x18db39b64 | libdispatch (stripped; around `_dispatch_mach_*`/`_dispatch_workloop_worker_thread`) | launchd dispatch mach source/workloop handling |
| 0x194557584/970 -> 0x194557eb8 | libsystem_malloc `_malloc`... `_malloc_zone_free` | malloc/free from user code (leaf frames of the same loop) |

Other frames: CoreAudio `_AudioDeviceExitMode` region, caulk `messenger::enqueue`, `pooled_semaphore_mutex::_lock`, CoreMedia `FigEventWaitRelative`: parked
mediaserverd IO/worker threads (state W, cpu 0), not hot.

## 2. What the stubs are, and what actually spins [E]

The profiler saves the user PC of the LAST kernel entry (find_user_regs). So every "pc" is the return point after an `svc`:
* `0x1c26114f0` = after `__workq_kernreturn` (0x170). `st=W`, bt = `start_wqthread` only: these are idle-looking libdispatch worker threads.
* mediaserverd's hot thread is ONE thread (t3, `st=R`, cpu 9.8 s per 20 s interval = 49%, the only mediaserverd thread with big cpu; all others are
  <= 94 ms). Its saved PC cycles over `mach_msg2_trap` (62% of weighted samples), `mach_absolute_time` svc -3 (21%), `mach_port_construct` (12%),
  `mach_port_destruct` (5%), i.e. one tight user-space loop that does: build xpc dict -> `xpc_pipe_routine` to launchd (lookup, routine 0x324) -> port
  construct/destruct -> malloc/free -> again. Full bt of the mach_msg2 sample: mach_msg2_trap <- mach_msg2_internal <- mach_msg <- xpc_pipe_try_receive <-
  `__xpc_pipe_interface_routine` <- (lookup wrapper) <- (xpc_look_up_endpoint-like at 0x1d1be4a60). Port-construct sample: ... `_xpc_connection_resume` <-
  `_xpc_connection_activate`. So mediaserverd repeatedly creates/activates an XPC (Mach service) connection and asks launchd to resolve a name.
* launchd's CPU: threads t1 and t3 (about 7.6 s and 9.2 s per interval) are libdispatch workqueue/workloop threads; sampled `st=W` at `__workq_kernreturn` or
  `st=R` in `kevent_id` / `mach_msg2` (bt into launchd +0x3edfc/+0x288f4/+0x33f08/+0x33c08/+0x30d38 and a `_xpc_pipe_routine_reply` frame). They never leave
  the "get message -> handle -> reply -> park -> woken immediately" cycle: csw/s ~8000 (S8PROF up= lines) = the ping-pong with mediaserverd. There is NO
  `__ulock_wait`, `psynch`, semaphore or timer-kevent wait among the hot PCs, i.e. no timeout syscall that could be returning early. [E]
* The two are one loop: launchd cost = servicing mediaserverd's lookups (2-4 context switches per lookup -> ~2-4 k lookups/s). [E for the pairing, the lookup
  rate is an estimate from csw/s]
* Why CPU did not move with +60% clock: the pair is a request/reply ping-pong gated by wake-up/IPI/idle-exit latency and scheduling on 2 cores; it is
  never 100% runnable on either side (49% / 72%), so more clock mostly shortens the compute slice, not the cycle. [G, consistent]

## 3. Timer / timebase hypothesis: no [E]

* mediaserverd/launchd hot PCs contain no timed wait. The only time-related stub is `mach_absolute_time` -> svc -3, and that is a plain read, not a wait.
* `mach_absolute_time` (0x1c26112d0) checks commpage byte 0xfffffc090 (`_COMM_PAGE_USER_TIMEBASE`): 0 -> `b.eq 0x1c2611354` = syscall. Our kernel
  (`/root/work/xnu-patched/osfmk/arm64/machine_routines.c: user_timebase_type()`) returns `USER_TIMEBASE_NONE` under `A5XELTE` ("CNTVCT has different firmware
  offsets across Exynos7580 CPUs"), so CNTKCTL PL0VCTEN is off and EVERY userland time read is a kernel trap. This is a real, fixed per-call cost and is why
  21% of mediaserverd's samples sit on svc -3 (`mach_get_times`, called from the loop). It amplifies the loop but is not its cause.
* Timebase: `pexpert/arm/pe_identify_machine.c` defaults `timebase_frequency_hz = 24000000` and overrides only from a `timebase-frequency` property under /cpus
  cpuN. analysis/s8-adt-tree.txt has NO timebase-frequency (cpu nodes carry only clock-frequency 0x64bb5a80 = 1.69 GHz little, 0x89ecd680 = 2.3 GHz big) and no
  patch in 0124-0131 sets it (the 26 MHz appears only in SweetFramebuffer fMinGap and S8_I2C_CLKIN). The S8 arch timer is 26 MHz (patch 0124 comments), so the
  kernel's notion of a tick is 1/24e6 s while the counter runs at 26e6: kernel time runs 26/24 = 1.083x real time, all timeouts/sleeps/timers expire 7.7%
  EARLY in real time, and mach_timebase_info (125/3) is 8% off in userland. This is a correctness bug (wall clock/uptime drift, short timers) but it can
  never produce a zero interval or an immediate-return timeout: a scale, not a zero. It cannot make a loop spin at fixed cost. Rank: not the cause, but worth
  fixing (see 5b).
* Missing IOKit pieces (IOPMrootDomain, RTC): no stub in the hot set waits on them. s8rtc boot-arg provides wall time. Not implicated. [G]

## 4. Ranked candidate root causes

1. (E for mechanism, G for which service) **mediaserverd re-activates an XPC Mach-service connection in a loop and each activation hits launchd's lookup
   (0x324) and fails/returns immediately.** The loop is in mediaserverd's CoreAudio/CoreMedia client code, not in libxpc/libdispatch. The service being looked
   up is almost certainly one of the ~290 launchd jobs our lean root removed (analysis/aks-sel17.md candidate 2 is the same idea): a lookup of an
   unregistered name returns an error at once, the client's invalidation/error handler creates a new connection, repeat. Best candidates from tools/lean-removed.txt
   (all bluetooth/fairplay): `com.apple.BTServer.*`, `com.apple.bluetoothd`, `com.apple.BluetoothServices` (mediaserverd entitlement list contains
   com.apple.BluetoothServices, com.apple.BTServer.le, com.apple.bluetooth.internal/system), `com.apple.fairplayd.H2`; also `com.apple.audio.*`/coreaudiod-style
   names if the lean pass removed any (check lean-removed.txt: no entry matches audio/media/fig other than listed bluetooth/analytics).
   Probability ~50% that it is a removed-job lookup, ~25% a different failure (sandbox deny of a service name, `non-launching` lookup returning ENOENT for an
   existing job whose binary is absent), ~25% something else in the loop.
2. (G, 15%) A service that does exist but whose job crashes/exits at once (KeepAlive/ThrottleInterval 0), so launchd's reply path is short and the client reconnects.
   Disfavoured: only 6-9 `exec` lines in the window.
3. (G, 10%) Hardware-less CoreAudio device: IO thread notifications re-arming. Disfavoured: the other 12 mediaserverd threads are idle (cpu <= 94 ms, parked in
   CoreAudio/caulk/FigEventWait) and the hot thread is in xpc, not in an IO proc.
4. (E: NOT the cause) timebase 24 vs 26 MHz, CNTFRQ, user timebase NONE, timer precision.

## 5. Proposed fixes / proofs

### 5a. Name the lookup (cheap, read-only; do this first)
Patch 0129 line 139 caps the sampled backtrace at 6 frames (`for (int i = 0; i < 6 && fp != 0 ...`), which stops inside libxpc. Raise it to 24 and
the S8PROF line buffer accordingly: the next v29-style log then shows the frames below `_xpc_connection_activate` (CoreAudio/CoreMedia/mediaserverd +offsets) so
the caller names itself via hp2-near.py / hp2-mdis.py (mediaserverd frames are mh=0x100eb4000-relative; image /usr/sbin/mediaserverd is only 188 KB, so the
loop is in a cache framework). Also print `th->thread_name` / dispatch queue label of the thread to get a free hint.
Proof that this is the loop: the bt of mediaserverd t3 `st=R` samples contains `_xpc_connection_activate` + `__xpc_pipe_interface_routine` in >60% of samples
(already true for 8/21 samples) and the same caller frame repeats every sample.

### 5b. Stop the loop (after 5a names the service)
* If it is a removed job: re-add that job's plist entry to the lean launchd.plist but with a harmless stand-in (`ProgramArguments=/usr/bin/true`,
  `MachServices` kept, `RunAtLoad=false`, `KeepAlive=false`, ThrottleInterval large) so the lookup succeeds and the client parks waiting for messages; or keep the
  original job but leave it ThrottleInterval=3600. (bluetoothd crash-respawn was the reason for removal, so a stub that exits is not enough: use `Disabled=true`
  with MachServices so launchd holds the receive right without launching it; messages then queue and the client blocks.)
* Quick binary A/B without new tooling: boot the lean root with only `com.apple.BluetoothServices`/`BTServer.le` (+ fairplayd.H2) MachServices re-added as
  Disabled jobs. Success criterion: `S8PROF top` shows launchd < 5% and mediaserverd < 5% and csw/s drops from ~8000 to < 1000.
* Independent hygiene fixes: (i) set `timebase-frequency` = 26000000 on /cpus/cpu0-7 in the S8 device-tree handoff (or in pe_identify_machine.c under
  the S8 define: `gPEClockFrequencyInfo.timebase_frequency_hz = 26000000;`), check with `sysctl hw.tbfrequency` / `mach_timebase_info` = 125/... ->
  numer/denom 500/13 for 26 MHz; log line: print `gPEClockFrequencyInfo.timebase_frequency_hz` and `CNTFRQ_EL0` at boot (they must be equal). (ii) once
  CNTVCT is verified identical across cores (read cntvct on all 8 cpus within a rendezvous and print deltas), return `USER_TIMEBASE_SPEC` for S8 to remove the
  svc -3 cost (21% of samples of the hot thread). Neither fixes the spin.

### 5c. Log lines that would prove the diagnosis
* 5a output: caller frames of the hot thread; or
* a one-shot kernel log of the `name` string in the first N `mach_msg2` sends from pid 28 to launchd's bootstrap port (needs a debug hook in ipc_kmsg/MIG decode;
  heavier), or run launchd with an XPC debug env in the lean root (`XPC_DEBUG`-style flags are not verified).
* After the stub fix: `S8PROF top` rows for launchd/mediaserverd and the `csw/s` field.

## 6. Confidence
* Mechanism (mediaserverd thread t3 loops on xpc connection activate + launchd lookup 0x324; launchd load is the other half): high [E].
* Not a timer/timebase/ulock issue: high [E] (no timed-wait stub in the hot set; the 24/26 MHz mismatch is real but only scales time).
* Which service is being looked up: unknown [G]; bluetooth/fairplay removed-job lookups are the leading guess.
