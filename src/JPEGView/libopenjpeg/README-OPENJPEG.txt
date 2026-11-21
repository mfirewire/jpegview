OpenJPEG 2.5.4 — vendored for JPEGView JP2/J2K support
=====================================================

WHAT IS HERE
  include/openjpeg-2.5/openjpeg.h, opj_config.h
      Headers from the official 2.5.4 release (byte-identical to the
      2.5.4 source tarball).
  bin/openjp2.dll        Win32 (x86) build
  bin64/openjp2.dll      x64 build
  lib/openjp2.lib        Win32 import library (unchanged, see below)
  lib64/openjp2.lib      x64 import library (unchanged, see below)
  JPEGView-openjp2-x86.def / JPEGView-openjp2-x64.def
      Export lists used to build the DLLs (51 exports each, taken from
      the official 2.5.4 DLLs). The x86 list uses the MSVC stdcall
      decoration (_opj_create_compress@4, ...); the x64 list is
      undecorated (opj_create_compress, ...).

VERSION: 2.5.4. There is no newer release: upstream declared the
repository unmaintained in July 2026, so 2.5.4 is the last release.

CARRIED FIX: CVE-2026-107080 / PR #1674 (2026-11-21)
  Upstream PR: https://github.com/uclouvain/openjpeg/pull/1674
  CVE-2026-107080 (CVSS 6.5): out-of-bounds read/write in the
  reversible 5/3 DWT encoder. When a tile is short and the number of
  decomposition levels is large, a lower resolution level can end up
  with height 0; opj_dwt_encode_procedure() still ran the forward
  vertical pass over it, and at an odd start
  opj_dwt_encode_and_deinterleave_v() read and assigned tmp[8..15]
  for two rows that do not exist, running past the end of the scratch
  buffer when the tile's largest resolution level is a single sample.
  JPEGView encodes JP2 (SaveJP2.cpp), so this is a real exposure.
  The fix (4 lines in src/lib/openjp2/dwt.c, applied verbatim from
  the PR diff — the PR's tests/CMakeLists.txt hunk does not apply to
  the 2.5.4 tree, only the dwt.c hunk does):
      if (height == 0) {
          return;
      }
  inserted in opj_dwt_encode_and_deinterleave_v() after sn/dn are
  computed and before opj_dwt_fetch_cols_vertical_pass().

HOW THE DLLS WERE BUILT (reproducible)
  The vendored binaries were official MSVC-built 2.5.4 DLLs. The
  patched DLLs were rebuilt from the 2.5.4 source tarball
  (openjpeg-2.5.4.tar.gz from github.com/uclouvain/openjpeg, tag
  v2.5.4, sha256 a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a)
  with the PR #1674 dwt.c hunk applied, cross-compiled on Linux
  (2026-11-21) with MinGW-w64 GCC 13.2.0 (x86_64-w64-mingw32 /
  i686-w64-mingw32), CMake 3.28.3, ninja 1.11.1:
    cmake -S . -B build-<arch> -DCMAKE_SYSTEM_NAME=Windows \
      -DCMAKE_C_COMPILER=<triplet>-gcc -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-static-libgcc" \
      -DCMAKE_SHARED_LINKER_FLAGS="-static-libgcc" \
      -DOPENJP2_DEF_FILE=<this dir>/JPEGView-openjp2-<arch>.def \
      -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_CODEC=OFF \
      -DBUILD_TESTING=OFF -DBUILD_DOC=OFF -DBUILD_THIRDPARTY=OFF \
      -DBUILD_JPIP=OFF -DBUILD_JAVA=OFF
    cmake --build build-<arch>
  with a scratch-only one-line addition to
  src/lib/openjp2/CMakeLists.txt so the DLL is named openjp2.dll
  instead of libopenjp2.dll:
    set_target_properties(${OPENJPEG_LIBRARY_NAME} PROPERTIES PREFIX "")
  and the .def file added to the openjp2 target's sources
  (${OPENJP2_DEF_FILE}). Neither edit is part of the vendored tree.
  Notes:
  - -static-libgcc: without it the DLL imports libgcc_s_seh-1.dll
    (x64) / libgcc_s_dw2-1.dll (x86), which would have to be shipped
    alongside. With it the DLL imports only KERNEL32.dll and
    msvcrt.dll, both present on every Windows system (verified by PE
    inspection of both DLLs).
  - x86 ONLY, two extra build steps are needed to reproduce the
    MSVC export table exactly, because GNU ld will not emit a
    leading underscore in an export name on its own:
      1. After the first (failing) link, rename the 51 exported
         symbols in every openjp2 object from _opj_X@N to __opj_X@N:
           i686-w64-mingw32-objcopy --redefine-syms=rename-86.txt \
             --remove-section=.drectve <obj>
         (rename-86.txt maps each _opj_*@N to __opj_*@N; the
         --remove-section=.drectve drops the compiler-emitted
         -export:"opj_X@N" directives, which would otherwise fail
         the link because the symbols were renamed. The .def file
         is then the sole source of exports.)
      2. Re-run the link. GNU ld prepends one underscore when it
         looks a .def entry up, so the plain .def entry _opj_X@N
         resolves to the renamed __opj_X@N symbol and is exported
         as _opj_X@N.
    Without these steps the x86 DLL exports opj_X@N (no underscore)
    and the checked-in MSVC import library cannot delay-load it.
    The x64 build needs neither step (x64 exports are undecorated).

WHY THE .lib FILES ARE UNCHANGED
  The import libraries are the original MSVC-built ones. They are
  still valid because the rebuilt DLLs export exactly the same 51
  symbols under exactly the same names as the official 2.5.4 DLLs
  (verified by diffing the export tables, sorted, old vs new, for
  both architectures — empty diff). The DLL name baked into the
  .lib files (openjp2.dll) is unchanged. If a future rebuild ever
  changes the export set, regenerate the .lib files with MSVC
  LIB.EXE from the .def files (same pattern as regen-thorvg-lib);
  do NOT check in dlltool-generated .lib files — they carry no
  SAFESEH info and break the Win32 /SAFESEH link.

VERIFICATION PERFORMED (2026-11-21)
  - Vendored openjpeg.h is byte-identical to the 2.5.4 release source;
    opj_config.h declares OPJ_VERSION 2/5/4.
  - The PR #1674 dwt.c hunk applies cleanly to the 2.5.4 source and
    the patched code is byte-identical to the PR diff.
  - Disassembly of both DLLs' objects: opj_dwt_encode_and_deinterleave_v
    tests height for zero first and returns before
    opj_dwt_fetch_cols_vertical_pass (x86: test %eax,%eax / jne;
    x64: test %r8d,%r8d / jne).
  - Upstream's own regression test (tests/test_empty_resolution_encode.c
    from PR #1674, extracted verbatim from the PR diff) run on Linux
    against the same patched source tree: exit 0, all 4 geometries
    round-trip exactly. The same test binary against pristine 2.5.4
    aborts with a glibc heap-corruption assertion (exit 134) — the
    control proves the test exercises the bug and the fix addresses it.
  - Export tables: old vs new DLLs, both architectures, sorted diff
    is empty (51 exports each). Imports: KERNEL32.dll + msvcrt.dll
    only, both architectures. Internal DLL name is openjp2.dll.
  - The Linux test suite (which builds JP2 code against the system
    libopenjp2, not these DLLs) shows no regression; the Windows
    JP2 encode/decode paths still need a device test (see CHANGELOG).
