# AppleKeyStore selector 17 flood (Spotlight / SpringBoard) and idle CPU (launchd, mediaserverd)

Static analysis only (phone never touched). Sources: sweetlog-v24.txt, SystemOS cryptex dyld_shared_cache_arm64
(unslid addresses), pristine root ios16-root.img, lean root ios16-root-v3.img (launchd.plist, 240 jobs).
Helper scripts (new): tools/aks-img.py (cache image/exports/dis), aks-adis.py (annotated dis), aks-xref2.py (callers incl. stub islands),
aks-near.py (addr -> image+export), aks-rate.py (log rates), aks-jobs.py (launchd.plist scan), aks-setup.sh / aks-clean.sh (mounts /mnt/an2ro, /mnt/an2ro3, /mnt/an2sys; all unmounted at the end).
No kernelcache was in /root/work/ipsw/x (only dmgs), so the selector table was mapped from user space; the shim's own enum already agrees.

## 1. What selector 17 is

Selector 17 = `aks_get_device_state` (device lock/backoff state). Confidence high.

* libAppleKeyStore (`AppleKeyStore.framework/AppleKeyStore`): `_aks_get_device_state` at 0x1f2ec5d20 is
  `mov x2,x1 ; mov w1,#0x11 ; b 0x1f2ec5d2c`. `_aks_get_extended_device_state` (0x1f2ec5e28) is the same with `mov w1,#0x23` (35).
  The common body (0x1f2ec5d2c..) zeroes a 0x1000 byte stack buffer (`mov w22,#0x1000 ; bl bzero`), stores 0x1000 as the out size,
  scalar-in = the keybag handle (`sub x2,x29,#0x40 ; mov w3,#1`), calls IOConnectCallMethod-style helper (0x1f2fb28e8) with struct-out = that buffer, then
  on rc==0 calls `0x1f2eb50f8(buf, outsize, out_struct)` (decode_extended_state, 0x2a-byte result struct).
* MobileKeyBag has its own copy: `0x1ae3e082c: mov x2,x1 ; mov w1,#0x11 ; b 0x1ae3df95c` (sel 17, struct-out 0x1000, scalar-in 1)
  and `0x1ae3e0838` -> sel 0x23. 0x1ae3df95c does `IOConnectCallMethod(conn, sel, &handle,1, 0,0, 0,0, buf,&size)` (0x1ae3df9f8) and decodes with 0x1ae3dfbe0.
  `MKBGetDeviceLockState` (0x1ae3df468) = `0x1ae3e0130(NULL,&st)` then returns `st[+4]` if rc==0 (`ldr w8,[sp,#4]; cmp w0,#0; csel w0,w8,w0,eq`).
  `MKBGetDeviceLockStateInfo` (0x1ae3df4a0) is the same call plus building a dictionary.
  (The `in 1/0 out 0/4096` in our log is the REQUESTED struct-out size, not what the shim writes.)
* Decoded DER (der_decode_set 0x1f2eb3354, key table at 0x1f2ed5810 via slots 0x21235cc18..cc88). Fields in order:
  `ss`->[+0] (u32), `sls`->[+4] (u32 = lock state, what MKBGetDeviceLockState returns), `sb`->[+8] u64, `sfa`->[+0x10], `sgs`->[+0x14],
  `sas`->[+0x18] u8, `sgpe`->[+0x19] u8, `srcd`->[+0x1a] u64, `sr`->[+0x22] u64.

## 2. What our shim does today (not "zeros")

0038-sweet-applekeystore.patch: `case kAKSGetDeviceState` (17) and, since 0055, 35 -> `sweet_aks_device_state()` writes a 128-byte DER SET
`{ss=SWEET_AKS_LOCK_STATE(4), sls=0, sb=0, sfa=0, sgs=0, sas=0, sgpe=0, srcd=0}` (no `sr`, decoded as 0), returns kIOReturnSuccess.
This matches the library's key set, so rc=0 and `MKBGetDeviceLockState()` returns 0 = unlocked.
Each call also does `proc_selfname` + `IOLog` (fixed cost per call, and it is why the flood is visible at all).

## 3. Callers and the "loop" - the return value is NOT what keeps it looping

MKBGetDeviceLockState callers found by scanning every executable cache mapping (incl. stub islands): 146 sites, 108 for MKBDeviceUnlockedSinceBoot
(that one uses selector 7, not 17, and caches a true result in a static byte, 0x1ae3df83c-0x1ae3df89c). Relevant to the Spotlight process:

| caller | code | how it consumes the state |
|---|---|---|
| UIKitCore 0x189a042ec (fn 0x189a042d0) | `mov x0,#0 ; bl MKBGetDeviceLockState ; cbz w0 -> 0x189a04360 ; cmp w0,#2` | 0 -> `respondsToSelector applicationProtectedDataDidBecomeAvailable:` and posts `UIApplicationProtectedDataDidBecomeAvailable`; 2 -> WillBecomeUnavailable. Notification handler, no retry. |
| UIKitCore 0x1891226c8, 0x18929916c, 0x189a09c88 | state queries inside UIApplication/scene code | not loops by themselves |
| SearchUI 0x1ce1cce9c | `bl MKBGetDeviceLockState ; cmp w0,#0 ; ccmp w0,#3,#4,ne ; cset w0,eq` | pure predicate "unlocked" (state 0 or 3) |
| SpotlightUI 0x1f439f054 / 0x1f439f208 | `cmp w0,#7 ; lsl w8,w21,w0 ; and w8,w8,#0x89 ; ccmp ... b.eq` | mask 0x89 = states {0,3,7} count as unlocked; sets a flag byte and continues |
| Spotlight.framework 0x20126ccd8 | `bl MKBGetDeviceLockState ; mov x22,x0 ... cmp w22,#6 ; b.hi` | switch on state 0..6 |

None of the callers re-queries because the value is "wrong": with sls=0 all of them take the "unlocked" branch and move on. So changing the
4096-byte content / the scalar return is very unlikely to stop the flood (confidence medium-high). The AKS call is a by-product of whatever drives the
process, not the thing being retried.

Evidence that it is driven by activity, not a tight retry loop (tools/aks-rate.py on sweetlog-v24):

* Spotlight sel 17 rate is 65.3/s (first 15 s interval 40/s), later 70.3/s, constant for ~12 min; csw/s is ~7500/s at the same time (~115 context
  switches per AKS call, i.e. a lot of Mach IPC around each iteration), usr 36% / sys 21% of the 4 cores. With Spotlight at ~104% CPU that is ~15 ms of
  CPU per iteration; the AKS call is cheap compared to that, so a pure `while(state!=x)` spin is not what we see.
* At up=1245..1305 s (SpringBoard becomes active, HID touches, SB sel 26/27/35/8 calls) the Spotlight rate jumps to 160-174/s AND SpringBoard
  itself does 1339/1397/1373/788 sel-17 calls per 15 s (~90/s) - then SpringBoard drops back to 0. A shared event stream (scene/settings updates between
  Spotlight and SpringBoard/FrontBoard) fits; a rate that went UP while the CPU was shared suggests it is event-driven, not CPU-bound.
* SpringBoard stack samples (v24 lines 5549-5561, 13266..) show all SB threads parked in `mach_msg` (pc 0x21184d030 = `mach_msg2_trap`, via
  `mach_msg2_internal`/`mach_msg_overwrite`/`CFRunLoopRunSpecific`) i.e. SB idles between bursts; no Spotlight samples exist in any log (s8prof_pc was not enabled:
  zero `S8PROF pc` lines in v20-v24).

### Guess (not verified): Spotlight's search-bar scene delegate re-layout ping-pong

Spotlight (/Applications/Spotlight.app/Spotlight, 109 KB shell; classes SPUISearchBarWindowSceneDelegate etc.) contains this cycle (disassembly via
`ipsw macho disass -x __TEXT.__text`):

    sizeBar (0x100001e64): view.setNeedsLayout; view.layoutIfNeeded; h = [view systemLayoutSizeFittingSize:UILayoutFittingExpandedSize]; [self didSizeWithHeight:h]
    didSizeWithHeight: (0x100001ee4): if (headerHeight != h)   // fcmp d0,d8 ; b.eq -> return   (NaN or a jittering h never compares equal)
                           { setHeaderHeight:h; [[searchBarWindow _scene] updateClientSettingsWithBlock:...] }
    _performActionsForUIScene:...settingsDiff: (0x100001ff8): if (settings.interfaceOrientation != view.activeInterfaceOrientation)
                           { view.setActiveInterfaceOrientation:...; [self sizeBar] }

If the height computed on our display (no real screen metrics, reduced rendering) never converges (NaN, 0 vs non-0, or the orientation never matches
because `setActiveInterfaceOrientation:` normalises the value), `updateClientSettingsWithBlock:` is sent to FrontBoard/SpringBoard on every pass and the
reply (settings diff) triggers the next pass. That would explain: steady ~65 Hz, heavy Mach IPC (csw 7500/s), SpringBoard waking in lock-step, and a lock
state query per scene update (UIKit 0x189a042d0 / SearchUI predicate). Confidence low (about 25%); it is the best static candidate in the app itself.
How to confirm cheaply: boot with `s8prof_pc=1` (patch 0129 already prints top-3 tasks' thread pc/lr/bt per interval) and symbolicate with
`aks-near.py <addr - slide>`; Spotlight threads should show `-[SPUISearchBarWindowSceneDelegate sizeBar]` / `_UIViewLayoutEngine` / CA commit frames.
Alternative check: set the Spotlight scene to not connect (see section 5).

## 4. Proposed shim behaviour

Minimal change for correctness: none required - sel 17/35 already return a decodable DER with unlocked state.
Optional hygiene (safe, high value for the idle profile): drop or rate-limit the per-call `IOLog` for selectors 7/17/35 (log first N calls per process, then
every 1024th), because `proc_selfname + IOLog` runs 65-170 times/s and contributes to the sys% and to sweetlog ring churn (the v24 ring shows 68707 AKS lines
and only 694 other lines, so the AKS spam is also evicting real information). Add the missing `sr` key (value 0) only if you want the struct fully populated;
it decodes to 0 anyway. If a future caller needs "device has been unlocked since boot" it already reads selector 7 -> 4 (bit 2 set), cached by libMobileKeyBag.
Notifications: nothing in the callers above waits for a notification to arrive (MKBGetDeviceLockState is a synchronous query), so no
`com.apple.mobile.keybagd.lock_status` post is needed to stop these callers.

## 5. launchd ~70% CPU and mediaserverd ~47% - ranked candidates

Data points: S8PROF top rows are `Spotlight[112]:104%/4t launchd[1]:74%/5t mediaserverd[28]:48%/13t` in 40+ consecutive intervals with identical numbers
(possible profiler artefact is worth a sanity check: per-core busy 45/80/49/72% = 246% total does match 104+74+48, so the load is real). After the
mediaserverd crash (log line 54026) the new mediaserverd[125] is again ~44%/13t, so it is systematic per instance, not a stuck instance. The log shows only 6-9
`exec xpcproxy` in the whole window, so launchd is NOT busy respawning jobs (the v23 respawners bluetoothd/fairplayd/nanotimekitcompaniond are removed).

launchd (ranked):
1. Mach IPC servicing from the Spotlight/SpringBoard/mediaserverd churn (csw/s steady at ~7500). launchd serves bootstrap lookups, xpc pipe routines and
   endpoint checkins for every XPC connection/lookup these processes make. Evidence: the constant 7.5k csw/s, the three busiest tasks all being heavy XPC
   clients, launchd's 5 threads. Fix the Spotlight loop first and re-measure; if launchd falls with it, this is the cause. Confidence medium (guess, no launchd sampling).
2. Lookups of removed services: the lean pass deletes ~290 jobs from launchd.plist (bluetooth, analytics, nano, dev-mode...). Clients that retry a failing
   `bootstrap_look_up`/`xpc_connection_create_mach_service` each pass (e.g. each frame/scene update) hit launchd's slow path every time. Candidate (guess, ~25%).
   Check: count `bootstrap_look_up` failures with a launchd trace or by restoring analyticsd/aggregated/triald in a test image.
3. Watch/queue paths that do not exist (in the lean root they are all missing; at runtime they may be created on the data volume - verify): QueueDirectories
   `/var/wireless/Library/Logs/Baseband/` (com.apple.DumpBasebandCrash), `/var/mobile/Library/com.apple.nsurlsessiond-launchd` (nsurlsessiond); WatchPaths
   `/Library/Preferences/com.apple.symptoms.plist` (symptomsd); fsevents LaunchEvents (MobileFileIntegrity x2, nehelper-embedded x7, revisiond `/.docid`);
   KeepAlive PathState jobs with absent paths (apsd, livefileproviderd, inboxupdaterd, installcoordinationd, managedconfiguration.passcodenagd/profiled,
   mobilerepaird, nanoregistryd, terminusd, timezoneupdates.tzd, cryptexd=False-state). Apple launchd should only register kevent/fsevents for these,
   not poll, so this is unlikely to reach 70% by itself (confidence low, ~10%). Zero jobs have a StartInterval below 3600 (timed 3600, geod 432000, CommCenterRootHelper
   86400, cfnetworkagent 86500), so no short StartInterval/timer storm exists in the job table; ThrottleInterval=0 jobs (AdminLite, CommCenterMobileHelper,
   CommCenterRootHelper, DMHelper, certui.relay, lskdd, managedconfiguration.profiled, wifi.wapic) would respawn instantly IF they crash, but none shows
   up in the exec log.
4. Always-running KeepAlive=True jobs with no matching hardware that may crash-loop silently below the log's exec visibility: wifid, configd, dhcp6d, sntpd,
   powerd, UserEventAgent-System, distnoted.xpc.daemon, timed, runningboardd, logd (ThrottleInterval 1), SpringBoard/backboardd/checkerboard/ClarityBoard.
   launchd work per respawn is small, and the exec counter shows only 6-9 execs, so rank low.
5. logd[25]:3% is steady - launchd/libxpc/os_log chatter is not the dominant cost.

mediaserverd (ranked):
1. Audio/media IO threads with no real hardware (13 threads, steady 44-48%): CoreAudio HAL / AudioServer timers spinning on a fake or absent audio device
   (no codec in our device tree). Guess, ~35%. It also crashed once with `abort` via C++ terminate (CRASH mediaserverd sig 6; frames: __pthread_kill, pthread_kill,
   abort, then libc++abi terminate/`__cxa_throw` family at unslid 0x1d1adef54/0x1d1acfa4c, thread start via libdispatch) i.e. an uncaught C++ exception in
   an IO/worker context at ~54026 lines in; the restarted instance shows the same load.
2. AKS: only 2x sel 7 + 1x sel 17 + sel 0 at startup, so AKS polling is NOT mediaserverd's problem.
3. Mach IPC with launchd/coreaudiod-style services (same ~7.5k csw/s pool) - cannot be separated from item 1 without pc samples.
Confirm with `s8prof_pc` samples for pid 28/125 (the profiler already supports top-3 tasks) and symbolicate with aks-near.py.

## 6. Confidence summary

* selector 17 = aks_get_device_state, DER in 4 KiB buffer, scalar-in = handle: high.
* shim already returns valid DER, callers see "unlocked", return value change will not stop the flood: medium-high.
* flood is activity-driven (frame/scene update path), ~65 Hz, ~15 ms CPU/iteration: medium (rate evidence); the specific
  sizeBar/didSizeWithHeight ping-pong: low-medium (guess).
* launchd cost is IPC servicing for the busy clients: medium-low (guess); path-watch/StartInterval causes: low.
* mediaserverd = audio IO with no hardware: low (guess).
Note: pip-installed numpy into WSL python (--break-system-packages) for the cache scan; no other system changes. All /mnt/an2* mounts are unmounted and removed.
