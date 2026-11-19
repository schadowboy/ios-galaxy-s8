# The ~800 s boot stall after a root-image change (datamigrator wait)

Status: static analysis only (root image v3 mounted read-only, pristine image and cryptex dyld cache read-only).
Nothing was applied, nothing run on the phone. Tags: [E] = evidence (disassembly/strings/logs), [I] = inference, [G] = guess.

## Short answer

The wait is not a datamigrator timeout and not a launchd boot task. The idle 800 s is lsd's registration thread, which
(for every registered bundle) calls MobileGestalt for `UniqueDeviceID`; MG then blocks 5 s in a CFRunLoop waiting for an
`IONetworkController` that does not exist on the S8. 141 apps x 5 s = 705 s, plus extensions = the observed ~800 s.
It only happens when the MG cache file is missing (the cache lives on the device side and is lost whenever the root
image/HFS metadata is replaced), so it appears after every root change and never on boots that reuse the cache.
The datamigrator (its 00LaunchServicesMigrator plugin) waits for that lsd work, which is why the user session
(installd, PosterBoard ...) starts only at 870 s.

Best fix = give MG a `mac-address-wifi0` property in /chosen of our ADT (one line in `tools/p132/mkadt_dreamlte.py`,
boot-image side) [E for the mechanism, not yet tested]. Root-image fix = ship a valid MG cache file (`tools/dm-fix.py`).

## 1. How datamigrator is launched and what it keeps  [E]

* There is NO launchd job or boot task for it. `grep` over /System/Library/LaunchDaemons and the job cache
  /System/Library/xpc/launchd.plist: only `LaunchEvents` of other jobs (atc, contactsd, coreduetd, lockdown ...) mention the
  notification `com.apple.datamigrator.migrationDidFinish`. The launchd boot-task list in the log is fsck, mount-phase-1,
  data-protection, finish-obliteration, detect-installed-roots, commit-boot-mode, restore-datapartition, mount-phase-2,
  init-with-data-volume, MSUEarlyBootTask, fips, keybag, usermanagerd, dirs_cleaner, xpcroleaccountd, init_featureflags,
  auearlyboot, tzinit, finish-restore, finish-demo-restore, sysstatuscheck, prng_seedctl, launchd_cache_loader: none is migration.
  So option (d) "un-gate the job" does not exist.
* It is an XPC service: `/System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator`
  (ServiceType System, role account mobile; registered via the framework's SystemLibraryTreeState entry in launchd.plist).
  Clients (they call DMIsMigrationNeeded / startMigrationAndOrBlock...): installd, powerd, dasd, mc_mobile_tunnel, SpringBoard.
  Plugins: /System/Library/DataClassMigrators/*.migrator (59 bundles; first is 00LaunchServicesMigrator, which calls
  `populateLSDatabase`), run out of process via com.apple.migrationpluginwrapper.xpc.
* Decision logic (datamigrator 0x100005900-0x100005ac0): no current build -> "Unable to get current build version"; build != last build ->
  "System build version changed ... Migration necessary"; else sentinel `/private/var/datamigrator/migrate` present ->
  "Migration sentinel present"; else "unchanged ... not necessary". State: `/var/mobile/Library/Preferences/com.apple.migration.plist`
  (image has `LastSystemVersion=20H392`, `DMUserDataDisposition.buildVersion=20H392`; SystemVersion ProductBuildVersion is 20H392, so equal).
  The sentinel dir does not exist in the image.
* Unlock wait (not the culprit): `_excecutePluginsNotificationCallback` (0x10000b7e4) loads the PreBoard key stash (MKBKeyBagKeyStashLoad); only if
  that fails and lock state is not 0/3 does it start PreBoard and print "Waiting for user unlock". [E]; no sign of it in the log [I].
* Watchdog constants in the binary are 30 s and 3600 s (no 780/800 s constant; no 0xB2D05E00 pattern). [E]

## 2. What the 800 s is  [E unless marked]

* Sampler data (v35b): between 90 s and 850 s the only thread that is not idle in a Mach wait is lsd t1, in
  `st=W pc=...fac` (semaphore wait) with an identical stack at every 20 s sample: dispatch_sema wait -> libMobileGestalt
  +0x274bc/+0x276e8/+0x270f8 -> +0x1c770 -> +0x16c8.. -> InstalledContentLibrary +0x6f10 .... The same offsets appear in v32c and v33b.
  At 870 s the thread is running (R) in ICL and lsd goes to 20 % CPU; installd/PosterBoard follow within seconds. datamigrator is never
  in the sample list (0 % CPU).
* MG code (cache addresses, unslid): the wait function 0x19c57c084 calls `IOServiceMatching("IONetworkController")` and then
  0x19c57c644 -> 0x19c57c430, which does `CFRunLoopRunInMode(mode, 5.0 (the w1=5 argument), true)` and a semaphore wait; after 5 s the answer is NULL.
  It is called from the Wi-Fi-MAC getter at 0x19c5716e8-0x19c571774:
  1. return immediately if `IODeviceTree:/chosen` has property `mac-address-wifi0` (CFData);
  2. else return if an IORegistry entry `sdio`, `wlan` or `marconi-wifi` exists;
  3. else (not a VM: kern.hv_vmm_present) wait for IONetworkController (5 s) and fall through.
  Our ADT builder (`tools/p132/mkadt_dreamlte.py`) sets no `mac-address-wifi0` and has no wlan/sdio/marconi node, and there is no Wi-Fi driver,
  so every call to this getter costs 5 s.
* Who calls it: ICL +0x6f10 calls `MGCopyAnswer("re6Zb+zwFKJNlkQTUeT+/w")`; that is `UniqueDeviceID`
  (verified: base64(md5("MGCopyAnswer"+name))[:22], see tools/dm-mgkey.py). UDID derivation needs the Wi-Fi MAC. [E]
* Why per bundle (800 s = N x 5 s): the image has 141 apps (361 Info.plists under /Applications). 141 x 5 s = 705 s; the rest are extensions. [I, medium]
  I did not see a log line per call; the count is consistent, not proven.
* Why only after a root change: libMobileGestalt keeps its answers in
  `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`
  ("Cache file did not exist. Rebuilding." string present). Neither image (pristine or v3) contains that directory or file,
  and /var/mobile/Library/Caches is empty, so the cache can only exist on the device partition. Replacing the root image/HFS metadata
  discards device-created files. [E for "not in image"; I for "flash discards it"; the cache loader is at 0x19c557bf0].
  Normal boots (v27a, v29b, v31: PosterBoard at 70-75 s, three datamigrator execs 45/60/70 s) have the cache and never take the slow path. [I]
* Why datamigrator looks gated: in slow and fast boots the datamigrator exec pattern is the same (exec at ~25 s, ~50-70 s, then again). Fast boots:
  second->third exec 15 s apart; v35b: 70 s -> 870 s, exactly while lsd is in the MG wait; the third exec is the migration finishing (PosterBoard follows). The 00LaunchServicesMigrator
  plugin (populateLSDatabase) is the plausible link between migrator and lsd. [I, medium; no direct sample of the plugin]
* Ruled out: launchd boot-task watchdog (no such task), MKB unlock wait (no evidence), DM plugin watchdog (30 s/3600 s constants only),
  missing MachService (no removed job was needed; lean-removed.txt not involved).

## 3. Proposed changes

A. (recommended, root cause, boot-image side, 1 line) in `mkadt_dreamlte.py` next to the other /chosen properties:

    set_prop(chosen, 'mac-address-wifi0', bytes([0x02, 0x00, 0x00, 0xa5, 0x5e, 0x01]))
    set_prop(chosen, 'mac-address-bluetooth0', bytes([0x02, 0x00, 0x00, 0xa5, 0x5e, 0x02]))

   MG then returns the Wi-Fi address from /chosen without waiting. Not tested; the two property names: first confirmed from MG's cfstring
   (`mac-address-wifi0`), bluetooth name is a guess (iBoot convention) [G]. Needs a new ADT/boot image, not a root image.

B. (root-image only) install a valid MG cache: `tools/dm-fix.py` + `tools/dm-fix.sh` (IMG=v3, remount rw at /mnt/dm6rw, fsck, manifest to
   /root/work/ios16-root-v10.json and images/ios16-root-v10.json). Validation rules decoded from the loader (0x19c557c40-0x19c557d38):
   plist with `CacheVersion` == SystemVersion build (20H392), `CacheUUID` == `42F6FA57-C5A1-4134-9BF4-AB00F962EAE8`,
   `CacheData` CFData of exactly 0x14e2 bytes, `CacheExtra` dict. [E]
   * Recommended input: a seed container pulled once from a device boot that completed (the cache plus
     `.com.apple.mobile_container_manager.metadata.plist`): `bash dm-fix.sh /path/to/systemgroup.com.apple.mobilegestaltcache`.
     The script refuses a seed that fails validation. Open: owner/mode are copied from the seed; I could not see the real ones. [G]
   * `DM_SYNTH=1 bash dm-fix.sh`: synthetic cache with zero CacheData and CacheExtra answers for UniqueDeviceID/WifiAddress/BluetoothAddress.
     EXPERIMENTAL: the CacheData layout is unknown (zeros might read as "false/0" answers for capabilities), whether CacheExtra is consulted before the
     question handler is unverified, and a container dir without containermanager metadata might get recreated. Do not use without a test boot. [G]
C. Not recommended: removing 00LaunchServicesMigrator.migrator (does not remove the MG waits; lsd registers anyway [I]).
   Setting `DMWatchdogPluginsAfterTimeout` etc.: irrelevant.

Cheapest check before any image change: after the next slow boot completes, look whether that cache file exists on the device (pull it), and
count lsd's MG calls (a 5 s log timestamp gap per UDID call). If A is built, the first boot after a root change should show PosterBoard at ~75 s.

## Tools written (read-only helpers, all under tools/)
dm-a..n.sh (mount v3 ro at /mnt/dm6ro, dump), dm-x.py / dm-y.py (datamigrator xrefs + annotated disassembly), dm-sym.sh, dm-mg.sh, dm-dsc.py, dm-mgs.sh/.py,
dm-xref.sh/.py (cryptex dyld cache via apfs-fuse at /mnt/dm6sys), dm-mgkey.py (MG key obfuscation), dm-fix.py/.sh (NOT run; `bash -n` OK).
All mounts removed after use.
