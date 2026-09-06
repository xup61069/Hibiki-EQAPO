# third_party

This folder is the only supported location for third-party source trees and locally built dependency outputs.

The repository currently expects:

- `vst3sdk/` - Steinberg VST 3 SDK headers and interfaces used by the VST3 host support and the x64 Monitor VST3 wrapper. The hermetic local build passes this tree as `VST3_SDK_ROOT`; retain the SDK's own license notices when redistributing a custom binary.
- `monitor_vcpkg_installed/` - generated, separate `x64-windows-static-md` install root for the in-process Monitor VST3 and Hibiki EQAPO driver. It keeps their static FFTW/libsndfile/codec graph separate from the main application's dynamic `vcpkg_installed/` tree; do not commit generated contents.
- `muparserx/` - expression parser used by Equalizer APO filters.
- `tclap/` - command-line argument parser used by helper tools.
- `vcpkg/` and `vcpkg_installed/` - created by `scripts/bootstrap-third-party.ps1` for FFTW3, libsndfile, and the official Steinberg ASIO SDK headers. The pinned vcpkg port currently resolves ASIO SDK 2.3.4 from Steinberg's official archive. Hibiki EQAPO uses the SDK's GPLv3 licensing path; preserve its license and notices and provide the corresponding source and build scripts with redistributed binaries.

The static in-process tree trades larger modules plus additional build/cache cost
for a smaller shared-process DLL collision surface. Before redistributing those binaries,
review the licenses of the exact locked dependency versions and ship every
required notice, corresponding source, or relink material. A successful local
static build is not a redistribution-license determination.

Generated build folders, package caches, and installed binary outputs under this directory are intentionally ignored by git. Recreate them with:

```powershell
.\scripts\bootstrap-third-party.ps1
```
