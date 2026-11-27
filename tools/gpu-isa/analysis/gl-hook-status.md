# GL hook route (CoreAnimation GLES on Mesa/Panfrost/Mali): status 2026-11-27

Evidence = observations on the phone; guesses are marked. Route and rule: see memory project-s8-gl-hook-route. The hook (libs8gl in backboardd) is TEST ONLY
(user 2026-11-23); every working part has to move into kernel/loader/driver stand-ins before it counts.

## Observed
- libs8gl injected into backboardd (kernel patch 0135, boot-arg `s8_inject=/usr/local/lib/libs8gl.dylib:backboardd`), replaces the 12 EAGLContext methods,
  Mesa 25.0.7 OSMesa ES3 context: software (`s8gl_sw=1`, softpipe) and `Mali-G71 (Panfrost)` on the S8Mali service. No GL errors with `glerrors=1`.
- User photo 2026-11-27 (Mali run): SpringBoard home screen drawn (clock, battery, app labels Photos/App Store/Settings/Search, two widgets "Waiting...", blue
  gradient). Missing: the app icons themselves, widget contents. Earlier runs: lock screen, gradient red->black (software run: red->blue).
- Phone responds to touch with the Mali path (not with softpipe), but is very slow.
- Frame cost (hook_log "frame N:" lines): simple frames 0.2 s, frames with 40-48 draws 3-6 s. Per frame ~16 MB of IOSurface textures are copied in and one picture out
  (no zero-copy IOSurface import yet).
- Kernel-call timing in the shim (this run): SUBMIT (0x40) is the cost: vertex/tiler jobs (0xf0) and fragment jobs (0xf1) both ~50 ms EACH, also for a 6-vertex
  quad; CREATE_BO (0x42) up to 80 ms for the heap buffer, otherwise a few ms; map/wait/madvise/get_offset negligible. Same ~40-50 ms with and without
  `s8gpudvfs=546000 s8gputmu=1` (the kernel log shows the GPUDVFS thread answering 546000 kHz).

## Guesses (not verified)
- A constant ~50 ms per job, independent of the work, points at the GPU running at a very low real clock (the DVFS request may not change the PLL; plan
  gpu-plan.md lists the 0x120 vs 0x140 PLL offset ambiguity) or at a fixed per-job latency in the driver. Test: read GPU CYCLE_COUNT (0x90) before/after a job
  in s8mali_gpu_submit and log it next to mach_absolute_time (kernel change, needs a Mac-built kernel).
- Colour difference (blue lost on the Mali run, correct on softpipe) is NOT a rejected GL call (no errors); suspects: texture/readback channel handling.

## Fixes found on the way (kernel, keep)
- 0156 IOSurface create: b3a8/w30r/w30R/l10r -> one plane BGRA. 0157: IOSurface selector 6 (WrapClientImage) makes a surface with a copy of the picture,
  selector 7 refused (an all-zero "success" reply made IOSurface.framework build a client with no state page: backboardd fault at address 0x30 in
  IOSurfaceClientGetBulkAttachments).
- 0158 (S8Mali selector 0x70: a text line into the kernel log): written, applies, never run (only built on the hosted runner, whose kernel panics).

## Tooling notes
- Hosted macos-15 builds a kernel that panics at boot (device tree pointer outside region): only the self-hosted Mac may build kernels. Userland/mesa builds are fine on hosted.
- Log extraction: tools/pull-root-changes.ps1 + tools/root-readfile.sh (read-only mount without the repair pass) ~6 min; console writes from backboardd are muted
  once the screen is up, so the kernel ring does not carry the hook's lines (0158 would).
- Boot-arg line limit: 608 bytes in the loader. Keep s8gpudvfs/s8gputmu/dvfsch/s8dvfs/s8dvfs_ms in test images (they were dropped by mistake for a while).

## Next
1. Mac-built kernel with: 0158 log selector + GPU cycle counter / per-job timing in s8mali_gpu_submit (find out why a trivial job takes 50 ms).
2. Zero-copy IOSurface import (S8Mali selector 0x7e + Mesa patch 0006) to remove the ~16 MB copies per frame.
3. Colour channel problem on the Mali path; missing icons/widget contents.
4. Move the working parts into the kernel (the hook is test-only).
