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

DRM side (`userland/s8mali/s8mali_drm.c`, handed to Mesa as a `s8_drm_ops` table by `userland/hook/gl_osmesa_s8.c` / `s8gl_test.c`):
what Mesa 25.0.7's panfrost uses is implemented: drmOpenWithType/drmIoctl (all panfrost selectors)/drmGetVersion/drmCloseBufferHandle/
drmCommandWrite(Read)/drmGetCap (all 0)/drmSyncobj{Create,Destroy,Reset,Wait,Import/ExportSyncFile,FDToHandle,HandleToFD} (job numbers kept
in the process)/hakios_drm_map+unmap. Still failing on purpose: drmPrimeHandleToFD, drmSyncobjTimelineWait, drmSyncobjTransfer (ENOSYS; panthor/
export only). drmPrimeFDToHandle calls kernel selector 0x7e (IOSurface as a buffer, `struct s8mali_surface` in `s8mali_drm.h`, semantics
written there; kernel patch 0163, on an older kernel it fails with ENOSYS). The Mesa side (S9+ `panfrost-darwin.patch` hunks) is patch 6:
6. IOSurface import (zero-copy textures): `pan_kmod_bo_import` takes the size from `s8_drm_surface()` (overlay: describe call via the ops table's
   `Surface` entry = `s8mali_surface()`), `panfrost_bo_import` returns NULL when the kernel refuses, the OSMesa frontend gets `get_egl_image` /
   `validate_egl_image` (glEGLImageTargetTexture2DOES with the surface ID as the image, enables GL_OES_EGL_image) and the OSMesa target gets
   `osmesa_surface_resource()` (linear 'BGRA' single plane, 64-byte aligned offset/pitch, `resource_from_handle`). NOT built or run: checked only
   that it applies on top of 1-5 on a mesa-25.0.7 checkout (`git apply --check`). Dylib install names are Meson's default (`@rpath/...`); use `install_name_tool -id` as needed.
