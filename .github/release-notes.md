# JPEGView v1.3.46-20261128

This release builds on v1.3.46-20261123. Gain-map photos get a proper HDR/SDR toggle (Ctrl+H) and RAW files get a thumbnail/full-size toggle (Ctrl+Shift+R); JXL gain maps are now applied; the gain-map direction for HDR-base files is corrected; and the Ctrl+H key itself works now (it was silently dead). It also includes the gain-map speed-up, the rotation out-of-bounds fix, the keymap merge for old KeyMap.txt files, and four menu items translated across 29 languages.

## What's new

- **HDR/SDR toggle for gain-map photos (Ctrl+H)**: JPEG, AVIF, HEIF and JXL gain-map photos can now be flipped between the HDR view and the SDR base at any time (menu "Show HDR"). HEIF gain maps are applied in this release — the previous release showed HEIC as SDR only. The default view is HDR (INI `GainMapDefault`, 1=HDR / 0=SDR).
- **RAW thumbnail/full-size toggle (Ctrl+Shift+R)**: a RAW file can be flipped between the embedded thumbnail and the fully decoded image at any time (menu "Show RAW image"); each representation keeps its own stored processing parameters. The old one-way full-size reload is replaced by this toggle.
- **JXL gain maps**: JXL files carrying an ISO 21496-1 gain map (jhgm) now apply it under the same Ctrl+H toggle, like JPEG, AVIF and HEIF.
- **Gain-map speed**: applying a gain map is about 6x faster with byte-identical output (a fused lookup table replaces the per-pixel PQ round-trip; 8.3 MP UltraHDR: 6.44 s → 1.05 s).
- **Menu translations**: four menu items (Show HDR, Show RAW image, Convert to JXL..., Recognize text (OCR)) are now translated in 29 languages.

## Bugs fixed

- **Gain-map direction (HDR-base files)**: the SDR view of an HDR-base gain-map photo was computed with the gain applied in the wrong direction. The signed-weight rule now gives the correct SDR view (verified against Mozilla's reference file: the two greys #898989/#bcbcbc).
- **Rotation out-of-bounds read**: free-rotating a gain-map image could read outside the image buffer (heap over-read). The rotation now clamps to the source bounds, with a 16-check boundary test.
- **Stale alternate after a failed load-time gain rotation**: if rotating the retained gain map failed at load, the stale alternate view stayed installed and the next edit could swap it in as the base, destroying the HDR base; dropping the gain map now also drops the alternate buffer.
- **RAW fallback image pinned**: toggling to the full-size image of a RAW file with no embedded preview could pin the fallback image in the read-ahead cache forever; it is now released when it is not the one on screen.
- **Negative gain-map headroom**: corrupt negative-headroom metadata could make the lazy and precomputed alternate views disagree (one inverted the gain, the other did not); negative headrooms are now rejected at parse time and both paths use the same signed-weight function.
- **Gain-map apply robustness**: running out of memory while upsampling the gain map in the non-fused path no longer throws (it falls back to SDR like the fused path); an out-of-contract 16-bit alpha value can no longer wrap the output alpha byte.
- **HDR-base alternate lost**: if the accurate 16-bit alternate view was attempted and is now unavailable (out of memory, a failed load-time rotation/mirror, or an install copy/size failure), an HDR-base file no longer silently falls back to a lazily recomputed 8-bit alternate view, because that recompute applies the inverse gain from tone-mapped 8-bit pixels and is provably inaccurate; the toggle now fails, the image stays HDR, and the menu item is greyed when the toggle is known-futile. SDR-base files are unaffected. The same rule now covers the unsharp-mask edit, which used to drop the alternate buffer without recording the loss.
- **JXL gain-map size cap**: JXL no longer rejects a gain map larger than its base image; the cap is now 4x the base per side like AVIF/HEIF/JPEG, with the image-size limit also applied to the gain dimensions.
- **Ctrl+H was dead**: the key binding was missing from the shipped key table, so Ctrl+H did nothing on any install (only the menu item worked); the table is regenerated and the key works. Old KeyMap.txt files also gain any missing default bindings without overwriting the user's own rebindings.
- **RAW toggle and thumbnail provenance (review fix round)**: the RAW toggle now checks the image actually came from the RAW path before offering the full-size view; a 16-bit RAW thumbnail is only used when it provably came from the file's own thumbnail; a missing current image no longer crashes the rotation panel.

## Library updates

- libjxl 0.12.0 → 0.13.0 (mitigates CVE-2026-111497, out-of-bounds write in the Brotli decoder, CVSS 4.0 9.2, under active exploitation; bundled brotli is now 1.2.1).
- zlib 1.3.2 → 1.4 (14 security fixes, several in inflate reachable from crafted image files).
- tinyexr: CVE-2026-110597 heap out-of-bounds read fixed in place with the reporter's patch (no upstream release fixes it yet).
- ThorVG 1.1.3 → 1.1.4 (SVG loader hardening, CSS cascade/!important fixes, mask-type="alpha", image-rendering support).
- libopenjpeg 2.5.4 + upstream PR #1674 (out-of-bounds read/write in the DWT encoder, reachable when saving a JP2; CVE-2026-107080). No version bump: upstream declared the repository unmaintained, so 2.5.4 is the last release.

## Known issues (not fixed in this version)

- A file whose first load fails usually stays unopenable for the rest of the session; the dead request can be evicted under cache pressure, after which a non-RAW/TIFF file may load again on revisit.
- A path given with forward slashes does not open the image at all: the window shows the no-image empty state and no file list is built.
- Renaming with name sorting leaves the entry at its old position.
- Shift+Delete permanently deletes the current file from disk (with a confirmation prompt); it does not use the Recycle Bin. Plain Delete moves the file to the Recycle Bin.
- Colour management (embedded ICC profiles) applies to JPEG and PNG. Exceptions: images decoded through the WIC fallback get no colour management, and CMYK JPEGs always convert - the switch only chooses between the embedded CMYK profile and the shipped cmyk.icm (a naive formula is the last resort).
- 16-bit CMYK PSD files still show only the embedded thumbnail (16-bit CMYK decode was never implemented; 8-bit CMYK works).
- If lcms2 cannot open an embedded profile, the pixels stay unconverted but the save filter still strips a non-sRGB profile, so the saved file can be mis-tagged the other way; and the sRGB check does not look at the profile's tone curve.
- Saving a JPEG through the GDI+ path may strip a non-sRGB profile that GDI+ silently did not convert (the same class of issue as the lcms2 case above); no triggering profile has been found, so this is unconfirmed.
- The ZIP-prediction fix was validated against independent decoders and generated files, not a Photoshop-written sample (none was available); the 65535×65535 JXL out-of-memory guard is covered by unit tests only (no real sample exists).
- The brotli patch level (1.2.1) cannot be confirmed from the binary itself; the upstream changelog is the only evidence.
- Gain-map photos with transparency: the gain map is now applied after alpha blending, so a transparent or semi-transparent gain-map photo renders slightly differently than before; real gain-map photos are opaque, so this is accepted rather than fixed.
- The Mozilla HDR-base reference vectors are verified by a documented manual step, not by CI (CI ffmpeg 8.0.1 cannot reproduce the decode); see the tie script header (tests/check_gainmap_vectors.py, EXCEPTION).

## Downloads

| Platform | File |
|---|---|
| Windows x64 | `JPEGView-x64-v1.3.46-20261128.zip` |
| Windows x86 | `JPEGView-win32-v1.3.46-20261128.zip` |

Extract and run `JPEGView.exe`; no installer required.
