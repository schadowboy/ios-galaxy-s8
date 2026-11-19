# mediaserverd hot loop (sweetlog-v32): BTAudioHALPlugin reconnecting to a non-existent bluetoothd service

Static only, phone untouched. Cache = iPhone X SystemOS cryptex (apfs-fuse /mnt/ms3sys), root = /root/work/ios16-root-v3.img (/mnt/ms3ro) and
pristine /root/work/ios16-root.img (/mnt/ms3pr); all unmounted afterwards. Scripts: tools/ms3-setup.sh, ms3-clean.sh, ms3-a.py (frame symbolication),
ms3-e.py (find the image for the non-cache frames), ms3-f.py (annotated Mach-O disassembler with chained-fixup import names and stub names), ms3-g.py/ms3-h.py (job tables).
Tags: [E] evidence, [G] guess.

## 0. Result in one paragraph

The hot thread is `BTAudioHALPlugin` (a CoreAudio HAL plug-in, `/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver/BTAudioHALPlugin`) loaded into
mediaserverd. It calls `xpc_connection_create_mach_service("com.apple.BTAudioHALPlugin.xpc", queue, 0)`, `xpc_connection_set_event_handler`,
`xpc_connection_resume`. That service is published only by `com.apple.bluetoothd`, which the lean pass removed from the launchd cache. launchd answers the
lookup (routine 0x324) with an error, libxpc delivers an XPC error event to the handler, and on a non-internal OS variant the handler **immediately
calls the connect function again, with no delay**. The 30 s / 1 s back-off exists only behind `os_variant_has_internal_content("com.apple.bluetooth")`. [E]
Fix: give launchd the `com.apple.BTAudioHALPlugin.xpc` port again (re-add a trimmed on-demand `com.apple.bluetoothd` job) or stop the plug-in loading. See section 4.

## 1. Symbolication [E]

Unslid cache address = runtime - 0xb2c4000 (same slide as in analysis/launchd-media-spin.md). Frames of the hot thread (mediaserverd[28] t1), innermost first:

| runtime | image!symbol | role |
|---|---|---|
| 0x1cd8d5030 | libsystem_kernel `_mach_msg2_trap`+8 | pc, return from the mach_msg2 syscall |
| 0x1cd8e6b18 | `_mach_msg2_internal`+0x4c | lr |
| 0x1cd8e6db8 | `_mach_msg_overwrite`+0x1e4 | |
| 0x1cd8d5524 | `_mach_msg`+0x14 | |
| 0x1dcec62f0 | libxpc `_xpc_pipe_try_receive`+0x1d0 | wait for launchd reply |
| 0x1dcec586c | libxpc `__xpc_pipe_interface_routine`+0x164 | xpc_pipe_routine to launchd |
| 0x1dcea8358 | libxpc +0x4358 (stripped, inside the routine wrapper family at 0x1d1be42a4) | `_xpc_domain_routine` style wrapper |
| 0x1dcea8a60 | libxpc +0x4a60 (stripped; function at ~0x4958 builds dict {type, handle, lookup-handle, flags, name, instance, cputypes}, routine 0x324, reads `port` / `non-launching`) | launchd service lookup (named Mach service) |
| 0x1dceb0688 | libxpc +0xc688 (stripped, connection internals) | connection -> lookup |
| 0x1dceb3200 | libxpc +0xf200 (stripped) | |
| 0x1dceb1cbc | libxpc `_xpc_connection_activate`+0x184 | |
| 0x1dceb1d8c | libxpc `_xpc_connection_resume`+0x5c | |
| **0x10309a5ec** | **BTAudioHALPlugin +0x325ec** (load base 0x103068000) | return address after `bl xpc_connection_resume` at +0x325e8, inside the connect function +0x32508 |
| **0x10309ab14** | **BTAudioHALPlugin +0x32b14** | return address after `bl +0x32508` (the connect function) at +0x32b10, inside the XPC event handler block function +0x32924 |
| 0x1dceb3788, 0x1dceb3b84 | libxpc +0xf788, +0xfb84 (stripped; event delivery to the connection handler) | |
| 0x198e5c834 | libdispatch +0x64834 (client callout) | |
| 0x198e1786c, 0x198e15048, 0x198e02168 | libdispatch (stripped; queue drain / workloop invoke) | |
| (pc2 continues) 0x198e02d70, 0x198e0cdc8 | libdispatch `_dispatch_workloop_worker_thread` region | |
| 0x1dce5ebd0, 0x1dce5e720 | libsystem_pthread `__pthread_wqthread`+0x11c, `_start_wqthread` | **the hot thread is a libdispatch workqueue thread, not the process main thread** (t1 is only the first thread index) |

Other samples of the thread: 0x1cd8d4f28/lr 0x1cd8d75fc = `_kernelrpc_mach_port_construct_trap` / `_mach_port_construct`+0x24 (a new connection allocates its
receive port every pass), pc 0x1cd8d535c (mach_absolute_time syscall fallback, no userland timebase), and malloc/free frames 0x19f81b584/0x19f81beb8.
This matches the note in launchd-media-spin.md (connection activate -> lookup 0x324 -> reply -> construct/destruct -> repeat).

### Why the non-cache frames are BTAudioHALPlugin, not the mediaserverd binary

mediaserverd's own __TEXT is only 0x18000 bytes (mh=0x100c74000), so 0x10309a5ec (= mh+0x2425ec) cannot be in it. It is a non-cache image mapped later.
I scanned all Mach-O files under /System/Library/Audio and /usr/lib for an image where both return addresses (page offsets 0x25ec and 0x2b14 modulo
the 16K page, distance 0x528, both preceded by a `bl`) fit: only `BTAudioHALPlugin` (+0x325ec/+0x32b14), `AppleAOPAudioPlugin` (+0xa5ec) and a few
generic libs matched by chance; disassembly of BTAudioHALPlugin confirms exactly (resume call just before 0x325ec, `bl +0x32508` just before 0x32b14).
Strings in the function leave no doubt. Confidence 95%.

## 2. What the code does [E]

Connect function `BTAudioHALPlugin+0x32508` (disassembly, annotated):
```
+0x32574 'Starting BTAudioPlugin for %@'            (os_log)
+0x32594 adr x0 -> 'com.apple.BTAudioHALPlugin.xpc'
+0x32590 ldr x1,[x19,#0x20]                          (dispatch queue)
+0x3259c mov x2,#0                                   (flags = 0, not LISTENER)
+0x325a0 bl _xpc_connection_create_mach_service      -> store in [x19+0xd8]
+0x325e0 bl _xpc_connection_set_event_handler        (block = function +0x32924)
+0x325e8 bl _xpc_connection_resume                   <- return 0x325ec = frame 0x10309a5ec
+0x3260c 'Register audio plugin connection with bluetoothd'
+0x3263c xpc dict: 'kBTAudioMsgMethod', 'kBTAudioMsgProcess' -> xpc_connection_send_message
```
Event handler block `+0x32924(block, event)`:
```
xpc_get_type(event) == _xpc_type_error ?
  yes: log xpc_dictionary_get_string(event, _xpc_error_key_description)
       xpc_release([x19+0xd8]); [x19+0xd8]=0; call +0x32c5c (cleanup)
       +0x329f8 bl +0x48f94 = tail-call os_variant_has_internal_content("com.apple.bluetooth")
       +0x329fc cbz w0, +0x32b0c    ; NOT internal  -> bl +0x32508 (reconnect right now, inside the handler)  <- return 0x32b14 = frame 0x10309ab14
       internal: event == _xpc_error_connection_interrupted && +0x48fa0 (MGGetStringAnswer + CFStringFind = dev board check)
                   -> 1 s (x20 = 1000000000) 'Rate limiting BTAudio xpc connection restart to %d sec on dev boards'
                 event == _xpc_error_connection_invalid -> 30 s (x20 = 0x6fc23ac00)
                 otherwise x20 = 0 ; 'No connection exist or bluetoothd is rejecting connection, reconnect in %d secs'
                 dispatch_after(dispatch_time(NOW, x20), queue, block -> +0x32df8 (reconnect))
```
So the only back-off exists for internal OS variants. On our (customer-variant) system every error event gives an immediate synchronous reconnect: the stack
shows exactly that recursion shape (handler 0x32b14 -> connect 0x325ec -> resume). A failed lookup yields an invalidation error event -> handler -> new
connection -> lookup -> ... forever, on a dispatch workqueue thread; each pass costs: connection alloc (port construct/destruct), pipe routine to launchd
(2-4 context switches), malloc/free, mach_absolute_time syscalls. That is the 49% mediaserverd + ~72% launchd spin.

## 3. Which service, and who provides it [E]

* Name: `com.apple.BTAudioHALPlugin.xpc` (a constant, nothing dynamic).
* Current v3 job cache (241 jobs, 629 MachServices): **not provided**.
* Pristine image cache (530 jobs): provided by `/System/Library/LaunchDaemons/com.apple.bluetoothd.plist` (label `com.apple.bluetoothd`), which the lean
  pass removed (lean-removed.txt: "bluetoothd respawn loop ... no BT hardware"). The plist file still exists on disk in the v3 image; only the cache entry is gone.
* Side finding: `AudioAppleSiriRemoteInput` (another HAL plug-in) uses `com.apple.BTLEAudioController.xpc`, published by removed job `com.apple.BTServer.le`
  (`/System/Library/LaunchDaemons/com.apple.BTServer.le.plist`); it would loop the same way if it ever loads (not seen in the hot stack). Other HAL plug-ins
  (AirPlayHalogen, AOP, USB, TimeSync, Baseband, CarPlay, Octavia, MIDI drivers) contain no `.xpc` mach service names.
* mediaserverd's own strings/entitlements (no dynamic name needed anymore): names that are NOT in the current cache but WERE provided by removed jobs:
  `com.apple.BTServer.le`, `com.apple.BluetoothServices`, `com.apple.CompanionLink`, `com.apple.coreduetd.context`,
  `com.apple.corespeech.corespeechd.activation.xpc`, `com.apple.relatived.tempest`, `com.apple.siri.external_request`, `com.apple.voiceservices.tts`, `com.apple.wifip2pd`.
  They are lookups of mediaserverd as a client and are not the hot loop (the hot stack is the plug-in connect/handler pair). Other lookups can fail the same
  way only if the owning code lacks a back-off; nothing else was seen hot in this log. The sole mach-lookup exception in the entitlement blob is `com.apple.TapToRadarKit.service` (provided by nobody in either cache).

## 4. Minimal fix (nothing modified now)

Goal: launchd must hold the port `com.apple.BTAudioHALPlugin.xpc` so the connection becomes valid and the plug-in just sends one message and waits.

Option A (recommended, 85% confident it stops the spin): re-add `com.apple.bluetoothd` to the cache as an on-demand job, i.e. the pristine dict with the
auto-start keys removed so it is not respawned in a crash loop (there is no BT hardware; this was why it was removed):
* cache key (same convention as tools/install-userland.sh `jobs[key] = job`): `/System/Library/LaunchDaemons/com.apple.bluetoothd.plist`
* dict: take it from the pristine cache (`/root/work/ios16-root.img`, `/System/Library/xpc/launchd.plist` -> `LaunchDaemons[key]`), then delete `RunAtLoad`,
  `KeepAlive`, `LaunchEvents`, `SuccessfulExit`; add `ThrottleInterval: 600` (or larger). Keep `Label`, `ProgramArguments` (`/usr/sbin/bluetoothd`, exists in v3),
  `UserName: mobile`, `MachServices` (all 13 keys; at least `com.apple.BTAudioHALPlugin.xpc`), `EnableTransactions`, `POSIXSpawnType`.
* Effect: launchd registers the ports at boot. The plug-in's message triggers one launch of bluetoothd; if it crashes the port stays with launchd,
  the client connection stays valid (no error event, so no loop); relaunch is throttled by the message queue and ThrottleInterval.
* Procedure (same load/modify/save as install-userland.sh): mount the v3 image rw, `cache=plistlib.load(open(root+'/System/Library/xpc/launchd.plist','rb'))`,
  `cache['LaunchDaemons'][key]=job`, `plistlib.dump(cache, f, fmt=plistlib.FMT_BINARY)`, restore owner/mode; the file `/System/Library/LaunchDaemons/com.apple.bluetoothd.plist`
  is still present on disk and can be left alone (or edited identically).
* Optional extra for the Siri-remote plug-in: same trick for `/System/Library/LaunchDaemons/com.apple.BTServer.le.plist` (label `com.apple.BTServer.le`, `/usr/sbin/BTLEServer`,
  MachServices `com.apple.BTLEAudioController.xpc`, `com.apple.BTLELoggingManager.xpc`, `com.apple.BTServer.le`); the same stub also resolves mediaserverd's `com.apple.BTServer.le` lookup. [G, not seen hot]
* "Disabled": true stubs: [G] untested, not recommended; launchd may treat a disabled job's services as unregistered, which would keep the failure.
  No `/bin/true`, `/bin/sleep`, `/bin/sh` exist in v3, so a stub with a dummy program is not available; point at the real `/usr/sbin/bluetoothd`.

Option B (no launchd change, 80%): stop the plug-in from loading: move `/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver` out of the way (no BT hardware anyway).
Risk: other CoreAudio code may log about it; also removes the SiriRemote path only if you do the same for `AudioAppleSiriRemoteInput`.

Option C (binary patch, 70%): in BTAudioHALPlugin make `+0x48f94` return 1 (`mov w0,#1; ret`) so the 30 s / 1 s back-off runs (invalid -> 30 s). Needs the code-signature
check to be satisfied/disabled; keeps the 30 s retry cost trivial. Making the whole OS "internal" is not worth it.

## 5. Confidence

| claim | confidence |
|---|---|
| non-cache frames are BTAudioHALPlugin +0x325ec/+0x32b14 | 95% [E] |
| name requested is com.apple.BTAudioHALPlugin.xpc via xpc_connection_create_mach_service | 95% [E] |
| immediate re-connect because os_variant_has_internal_content("com.apple.bluetooth") is false | 90% [E disassembly; the runtime value of the check is inferred from the loop itself] |
| the lookup fails because bluetoothd's job (sole provider) is absent from the cache | 90% [E for provider table, G for the exact launchd reply] |
| thread is a dispatch workqueue thread on the plug-in's queue (not main) | 85% [E from the bottom frames; the queue arg [x19+0x20] not traced] |
| Option A stops the spin | 85% [G] |
