# locationd / WirelessRadioMan crash loop (iOS 16.7.16, iPhone X userland) - static analysis

Method: SystemOS cryptex dyld_shared_cache_arm64 (apfs-fuse mounted at /mnt/sysos) + /usr/libexec/locationd from the pristine root image.
Disassembly with capstone through tools/lc-dis.py (ipsw dyld disass OOMs in WSL). Unslid = runtime - 0x45f20000.

## Symbolicated frames (locationd pid 62, same for pids 177/183/193)

| runtime | unslid | symbol |
|---|---|---|
| pc 0x217b20894 | 0x1d1c00894 | libxpc `__xpc_api_misuse`+76 (`brk #1`) |
| lr 0x217b2088c | 0x1d1c0088c | libxpc `__xpc_api_misuse`+68 |
| bt0 0x217b0c4cc | 0x1d1bec4cc | libxpc `_xpc_connection_last_xref_cancel`+244 (return addr after the `bl __xpc_api_misuse` at 0x1d1bec4c8) |
| bt1 0x217b01c44 | 0x1d1be1c44 | libxpc `-[OS_xpc_connection _xref_dispose]`+24 |
| bt2 0x1e4c04164 | 0x19ece4164 | libATCommandStudioDynamic `qmi::Client::create(std::string const&, qmi::ServiceType, dispatch_queue_s*, dispatch_queue_s*, _xpc_connection_s*)`+212 (the objc_release right after the impl call) |
| bt3 0x101819c9c | locationd +0xd11c9c | caller of Client::create, string arg "location" (0x10187b32f), x4 = NULL xpc connection |
| bt4 0x10181593c | locationd +0xd0d93c | wrapper |
| bt5 0x100fe9830 | locationd +0x4e1830 | passes string "QMI-PDS" (0x1016f3845) - CLQmiPds / CLQmi* manager ctor |
| bt6 0x100febf68 | locationd +0x4e3f68 | singleton init, string "QmiPds" (0x1016f3b0a), stores into global 0x101cd8e60 |
| bt7/8 | 0x18db98780 / 0x18db3964c | libdispatch `_dispatch_client_callout`+16 / `_dispatch_once_callout`+28 |
| bt9 0x100fe1c00 | locationd +0x4d9c00 | accessor `dispatch_once(&..., ...)` for the QMI-PDS singleton |

(locationd is stripped; names above are from strings/call structure.)
WirelessRadioMan crash (pid 41) has the identical first three frames (0x217b0c4cc 0x217b01c44 0x1e4c04164), i.e. the same qmi::Client::create path.

## Root cause

libxpc aborts with the message at 0x1d1c147c8: **"Release of last reference on a suspended connection."** (printed through `XPC API Misuse: %s`, then `brk #1` = SIGTRAP; the "reason ns 2 code 5" is just libsystem's generic crash-reason wrapper, not a separate check).

Disassembly (0x1d1bec3d8, `_xpc_connection_last_xref_cancel`):

    0x1d1bec3f8: ldr w8, [x0, #0x1c]      ; connection suspend count
    0x1d1bec3fc: cbnz w8, #0x1d1bec4bc    ; non-zero -> misuse path
    ...
    0x1d1bec4bc: adrp x0,... add x0,x0,#0x7c8  ; "Release of last reference on a suspended connection."
    0x1d1bec4c4: bl  <log>
    0x1d1bec4c8: bl  __xpc_api_misuse          ; ... brk #1

The connection being dropped is created inside `qmi::Client::create` (libATCommandStudio, 0x19ece4090). locationd passes `x4 = NULL`, so the library builds its own:

    0x19ece40ec: bl  ...                    ; class check of arg4
    0x19ece4100: adrp x0,0x19ed16000 ; add x0,x0,#0x760   ; "com.apple.commcenter.atcs.xpc"
    0x19ece4108: mov x1,#0 ; mov w2,#2
    0x19ece4110: bl  xpc_connection_create_mach_service  ; created suspended (count 1)
    0x19ece4158: bl  0x19ece11ec            ; Client impl ctor (should resume/retain it)
    0x19ece4160: bl  objc_release([sp+8])   ; <- last ref dropped, connection never resumed -> libxpc abort

So the QMI client for the baseband (talks to CommCenter's `com.apple.commcenter.atcs.xpc` service) cannot come up and
the half-built connection is released while still suspended. In real hardware CommCenter owns that mach service. In our
port CommCenter is deliberately removed by `upstream/tools/sweet-root-tweaks.py` (`launchd()` drops the
`com.apple.CommCenter` job: "no baseband"), and no job in launchd.plist exports `com.apple.commcenter.atcs.xpc`
(checked: no LaunchDaemon lists it in MachServices). Both locationd (CLQmiPds/QMI-PDS "location" client, created in a
dispatch_once at startup) and WirelessRadioManagerd hit this at launch, then KeepAlive respawns them -> crash loop.
Confidence: high on the abort message/path and the consumers; medium on exactly why the impl ctor did not resume (not fully
traced, but its precondition is clearly the absent ATCS service/baseband). It is not a kernel, MobileGestalt, entitlement or sysctl issue.

## Other crashers in sweetlog-v21

* WirelessRadioMan[41]: same as above (qmi::Client::create, frames 0x1e4c04164 <- 0x100b1f1fc).
* driverkitd[67]: SIGKILL (sig 9), pc 0x208531d98 = libsystem_kernel `_read`+8 (svc), called from
  Foundation `-[NSData initWithContentsOfFile:options:maxLength:error:]` <- `__NSReadFromFileDescriptorWithProgress`+192.
  Reason "ns 1 code 7" = OS_REASON_JETSAM / JETSAM_REASON_DISK_SPACE_SHORTAGE per xnu numbering (check against what our kernel
  stub emits), far 0x10219c000 = the read destination buffer. This is a kernel-originated kill during a file read of a
  large/ mmap-ed file, not a userland abort: look at the kernel's file read / memory-pressure/ page-in path (a read into a
  fresh buffer that faults and gets killed), not at launchd config. Not diagnosed further (low confidence). Note the log line
  says "(none)" for the reason text; consider which file driverkitd reads (likely a driver/dext manifest) in the next trace.
  driverkitd is RunAtLoad with no KeepAlive so it is cosmetic if nothing needs DriverKit.

## Recommended fix (no repo edits made)

Minimal, root-image only: extend `launchd()` in `C:\github\ios-s8\upstream\tools\sweet-root-tweaks.py` so that, next to the
CommCenter removal, it also deletes the jobs whose Label is `com.apple.locationd` and `com.apple.WirelessRadioManager`
(keys in /System/Library/xpc/launchd.plist -> LaunchDaemons: `/System/Library/LaunchDaemons/com.apple.locationd.plist`,
`/System/Library/LaunchDaemons/com.apple.WirelessRadioManager.plist`; optionally `com.apple.driverkitd.plist`).
Note: clients of com.apple.locationd (Maps, apps, timezone) just get no service; they tolerate it. Do not set only `Disabled`
(launchd.plist is the pre-loaded job store here, matching how CommCenter is already handled).

Better long-term alternative (restores the services instead of removing them): keep locationd/WRM but provide a stub
Mach service `com.apple.commcenter.atcs.xpc` (e.g. a tiny launchd job with MachServices = {com.apple.commcenter.atcs.xpc: true}
backed by a trivial daemon that accepts and resumes connections, or just an empty-listener job). The QMI client would then
get a live (if silent) peer instead of an un-resumed dangling connection. This needs a small arm64 binary in the root image;
only worth it if locationd must stay alive (e.g. for Wi-Fi/time zone). No kernel/loader change is needed for either option.

Scripts used (tools\): lc-extract.sh, lc-cache.sh, lc-sym.sh, lc-dis.sh/.py, lc-str.py, lc-bin.sh, lc-jobs.sh/.py.
