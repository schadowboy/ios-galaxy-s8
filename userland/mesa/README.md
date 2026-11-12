# Mesa 25.0.7 for iOS arm64 (S8 port)

CI builds OSMesa (ES profile) with gallium `panfrost` + `softpipe` as `libOSMesa.8.dylib` (+ `libglapi.0.dylib`)
for iPhoneOS arm64 (min 16.0). Workflow `.github/workflows/mesa.yml`, entry point `build-mesa.sh <out>`,
artifact `mesa-ios-arm64` (lib/, include/, build-info.txt with options, patches, sizes, otool/nm, sha256).

Meson options: see `OPTS` in build-mesa.sh (copied into build-info.txt). Key: `-Dplatforms= -Dgallium-drivers=softpipe,panfrost
-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles2=enabled`, glx/egl/gbm/vulkan disabled, cross file `ios-arm64.cross`.
No host mesa_clc/LLVM/libclc is needed for gallium panfrost in 25.0.7 (CLC is only used by intel/asahi/precomp).

Patches (`patches/`, applied with `git apply`; regenerate with `tools/mesa-mkpatches.sh` in WSL):
1. G71 model tile buffer 8192 -> 4096 (pan_props.c). PROD_ID is still matched exactly against 0x6000: the kernel shim must report 0x6000.
2. clang fix: parenthesise `dst` in pan_pack_helpers.h (S9+).
3. BO CPU map/unmap through `hakios_drm_map/unmap` on `__APPLE__` (no mmap on a device node).
4. Build system: on darwin with panfrost, `dep_libdrm` is the stand-in in `src/s8-drm`; stub compiled into libpankmod; OSMesa links
   panfrost; `osmesa_target.c` tries `drmOpenWithType("panfrost")` first, then softpipe (`OSMESA_SOFTWARE=1` forces softpipe).
5. `OSMESA_ES_PROFILE` (0x38) for OSMesaCreateContextAttribs (S9+ patch, MIT).

`overlay/src/s8-drm/`: `xf86drm.h` stand-in and `s8_drm_stub.c` (weak functions, all failing, so the library falls back to softpipe).

Missing: the real DRM implementation (`userland/s8mali/s8mali_drm.c` lacks drmPrimeFDToHandle, drmSyncobj*, drmCommandWrite*,
drmGetCap); IOSurface import (S9+ pan_kmod.c/pan_bo.c hunks) is not ported. Dylib install names are Meson's default
(`@rpath/...`); use `install_name_tool -id` as needed.
