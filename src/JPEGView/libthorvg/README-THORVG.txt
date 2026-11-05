ThorVG 1.1.3 — vendored for JPEGView SVG support (Format C-3)
================================================================

WHAT IS HERE
  include/thorvg.h, include/thorvg_capi.h
      Headers from the official 1.1.3 release tarball
      (thorvg-1.1.3.tar.xz, sha256
       ef97785ed6d0f25ab84d6b4a0eed4325c68c4d80b1c2a61ea0b4029192fea746).
      Only the C API (thorvg_capi.h) is used by SVGWrapper.cpp; see its
      header comment for why the C++ API is not used.
  bin/libthorvg-1.dll        Win32 (x86) build, 1,118,948 bytes
  bin64/libthorvg-1.dll      x64 build, 1,116,757 bytes
  lib/libthorvg-1.lib        Win32 import library (dlltool-generated)
  lib64/libthorvg-1.lib      x64 import library (dlltool-generated)

WHY THE DLL IS NAMED libthorvg-1.dll
  Meson names the library "thorvg-<major>", so the MinGW build produces
  libthorvg-1.dll. The vcxproj DelayLoadDLLs entry and the import
  libraries both use that exact name; do not rename the DLL without
  regenerating the .lib files (the name is baked into them).

HOW THE DLLS WERE BUILT (reproducible)
  ThorVG is meson-only; there is no prebuilt MSVC DLL. The vendored DLLs
  were cross-compiled on Linux (2026-11-06) with MinGW-w64 GCC 13
  (x86_64-w64-mingw32 / i686-w64-mingw32), meson 1.12.1, ninja 1.13.2:
    meson setup build-tvg-<arch> thorvg-1.1.3 \
      --cross-file cross-<arch>.txt --buildtype=release \
      --default-library=shared \
      -Dloaders=svg,ttf -Dbindings=capi -Dengines=cpu \
      -Dsavers= -Dtools= -Dtests=false -Dsimd=false -Dextra= \
      "-Dcpp_link_args=['-static-libgcc','-static-libstdc++']"
    ninja -C build-tvg-<arch>
  Notes:
  - loaders=svg,ttf only (lottie dropped: fewer dependencies).
  - simd=false: no AVX2/SSE code is emitted, so the DLL runs on any x86
    CPU JPEGView itself supports (verified: no VEX instructions in either
    DLL; the only "vfprintf" hits are the C library symbol name).
  - extra= (empty): the default "openmp" was dropped to avoid a libgomp
    runtime dependency.
  - -static-libgcc -static-libstdc++: without these the DLL imports
    libgcc_s_seh-1.dll / libstdc++-6.dll (x64) and libgcc_s_dw2-1.dll /
    libstdc++-6.dll (x86), which would have to be shipped alongside.
    With them the DLL imports only KERNEL32.dll and msvcrt.dll, both
    present on every Windows system. Verified by PE inspection of both
    DLLs: machine 0x8664 (x64) / 0x014c (x86), 410 exports, 191 tvg_*
    exports, all 14 C API functions used by SVGWrapper.cpp present.
  - The import libraries were made from the DLL exports with dlltool:
      x86_64-w64-mingw32-dlltool -d thorvg-x64.def -l thorvg-x64.lib
      i686-w64-mingw32-dlltool   -d thorvg-x86.def -l thorvg-x86.lib
    (the .def files list all 410 exports with
     "LIBRARY libthorvg-1.dll"). dlltool .lib files link from MSVC for
    plain-C symbols, but that link step can only be proven by the CI
    build, not on the Linux VM — if CI link fails, that is the first
    place to look.

SHA256 (as vendored)
  bin64/libthorvg-1.dll  f28dc3c1bc8dedbeff0079a6fe6e855616b35278bd4875a4210452289e486386
  bin/libthorvg-1.dll    7392171a7b0b555e1b7a2f6b886717fae7ad1d44fcc67b8f38485db04f3ec00e
  lib64/libthorvg-1.lib  ab02f9d71b3c49cefc6f6653ef1cd86f4a3d1f809fd033980faabe75b9f2c80f
  lib/libthorvg-1.lib    70ea6d2d122d37db6e6c1a400fec85e0955cfb11c79e1af55b3d3f4d2a38fc16

DELAY-LOAD
  libthorvg-1.dll is delay-loaded (vcxproj DelayLoadDLLs): opening JPEGView
  or any non-SVG file never loads it. The import library is linked so the
  delay-load helper can resolve the tvg_* symbols on first use.
