# JPEGView v1.3.46-20261121

HDR support: PQ (ST 2084) and HLG images now display tone-mapped to SDR, gain-map photos apply their gain maps (JPEG and AVIF), and 16/32-bit PSD files open with tone mapping. This release also fixes the HDR issues found in review and updates five bundled libraries. Everything below was verified against the code and — for the device-testable items — on a real Windows machine.

## What's new

- **HDR tone mapping**: images with a PQ or HLG transfer now display tone-mapped to SDR (1000-nit peak, SDR white 203 nits) instead of showing the raw signal too dark. Supported in PNG, AVIF, HEIF and JXL. SDR images are unaffected.
- **Gain maps (ISO 21496-1)**: JPEG UltraHDR and AVIF gain-map photos now apply their gain map. Apple XMP gain-map JPEGs (iPhone-shared HDR photos) are supported too, including files whose headroom lives in the auxiliary image's XMP. HEIF gain maps are NOT applied in this release — see the limitation below.
- **PSD HDR**: 16-bit and 32-bit PSD/PSB files (RGB and grayscale, Raw/RLE/ZIP) now open with tone mapping instead of falling back to the embedded thumbnail. 8-bit PSDs are byte-identical to before.

## Bugs fixed

- **HDR PSD garbled output**: a 16/32-bit PSD with an alpha channel came out garbled because the tone mapper wrote 3-byte BGR into a 4-byte BGRA buffer; the HDR path now writes BGRA directly.
- **HDR PNG black screen (8-bit)**: an 8-bit PNG declaring a PQ/HLG transfer could write past the image buffer; 8-bit rows are now expanded to 16-bit first.
- **HDR PNG black screen (no alpha)**: a 16-bit RGB PNG with no alpha channel rendered black because the HDR path copied the libpng filler as alpha (255/257 = 0); files without a real alpha channel are now forced opaque.
- **HDR tone mapping edge cases**: infinite or absurdly bright pixels no longer produce NaN or darken the whole image; HLG super-white is clamped; a .hdr file with an absurd GAMMA header is clamped to 0.25–4.0.
- **DCI-P3 colour**: HDR images in DCI-P3 now use their own colour matrix instead of falling back to BT.709; Display P3 (D65) files are unaffected.
- **Transparent HDR**: transparent pixels in AVIF, HEIF, JXL and PNG (and gain-map files) now keep their alpha instead of rendering as black blocks.
- **Gain-map robustness**: absurd gain-map metadata is clamped; truncated ISO segments are skipped; malformed XMP gamma rejects the packet; running out of memory while applying a gain map falls back to the SDR base instead of failing the load; absurd HEIF/AVIF gain-map dimensions are rejected before decoding.
- **Out of memory**: a genuine out-of-memory while tone-mapping an HDR PNG no longer falls back to GDI+ (which showed a dark image); a 65535×65535 JXL canvas no longer throws while allocating.
- **PSD ZIP**: a 16/32-bit PSD with ZIP compression failed to open (inflateInit returned -2) because the bundled zlib.lib ships no default allocator; the decoder now provides explicit allocators. 32-bit ZIP-with-prediction files also decoded with wrong pixel values (prediction reversed as uint32 sums instead of byte-plane deltas); they now follow the byte-plane scheme.
- **Apple gain-map JPEG**: an iPhone-shared HDR JPEG silently showed SDR because the headroom lives in the auxiliary image's XMP, which was never read; the auxiliary XMP is now tried as a second fallback (verified on real files).
- **Saving**: a JPEG whose embedded ICC profile is not sRGB (e.g. Adobe RGB, Display P3) no longer keeps that profile on the output file after the pixels were converted to sRGB; the stale profile is stripped when embedded colour profiles are enabled.
- **Hardening (crafted files)**: crafted PSD/PSB, ICC and gain-map files no longer hang or crash the viewer (21 hardening fixes, see CHANGELOG).
- **Drag and drop**: dropping a file whose path is longer than 511 characters no longer reads from an uninitialised buffer; such a path is ignored instead (the app does not support long paths).

## Library updates

- libjxl 0.12.0 → 0.13.0 (mitigates CVE-2026-111497, out-of-bounds write in the Brotli decoder, CVSS 4.0 9.2, under active exploitation; bundled brotli is now 1.2.1).
- zlib 1.3.2 → 1.4 (14 security fixes, several in inflate reachable from crafted image files).
- tinyexr: CVE-2026-110597 heap out-of-bounds read fixed in place with the reporter's patch (no upstream release fixes it yet).
- ThorVG 1.1.3 → 1.1.4 (SVG loader hardening, CSS cascade/!important fixes, mask-type="alpha", image-rendering support).
- libopenjpeg 2.5.4 + upstream PR #1674 (out-of-bounds read/write in the DWT encoder, reachable when saving a JP2; CVE-2026-107080). No version bump: upstream declared the repository unmaintained, so 2.5.4 is the last release.

## Important limitation: HEIF gain maps are disabled in this release

iPhone HDR photos in HEIF format are shown as SDR for now: gain-map application for HEIF is temporarily disabled. The highlights stayed too bright on one real sample (iPhone 15 Pro, iOS 18.0) even with the correct headroom, and gain-map opens are slow (about 7–9 s against under 1 s for SDR). The parser still implements the headroom rule (a file whose XMP carries only HDRGainMapVersion and no HDRGainMapHeadroom shows SDR; a file that does carry a headroom uses it as a linear ratio, converted with log2), and JPEG/AVIF gain maps are unaffected and still apply. See docs/HDR-HEIC-exploration-2026-11-21.md.

## Known issues (not fixed in this version)

- A file whose first load fails usually stays unopenable for the rest of the session; the dead request can be evicted under cache pressure, after which a non-RAW/TIFF file may load again on revisit.
- A path given with forward slashes does not open the image at all: the window shows the no-image empty state and no file list is built.
- Renaming with name sorting leaves the entry at its old position.
- Shift+Delete permanently deletes the current file from disk (with a confirmation prompt); it does not use the Recycle Bin. Plain Delete moves the file to the Recycle Bin.
- Colour management (embedded ICC profiles) applies to JPEG and PNG. Exceptions: images decoded through the WIC fallback get no colour management, and CMYK JPEGs always convert - the switch only chooses between the embedded CMYK profile and the shipped cmyk.icm (a naive formula is the last resort).
- 16-bit CMYK PSD files still show only the embedded thumbnail (16-bit CMYK decode was never implemented; 8-bit CMYK works).
- If lcms2 cannot open an embedded profile, the pixels stay unconverted but the save filter still strips a non-sRGB profile, so the saved file can be mis-tagged the other way; and the sRGB check does not look at the profile's tone curve.
- If applying a JPEG gain map throws while out of memory, a few small buffers can leak (bounded, does not accumulate across images).
- The ZIP-prediction fix was validated against independent decoders and generated files, not a Photoshop-written sample (none was available); the 65535×65535 JXL out-of-memory guard is covered by unit tests only (no real sample exists).
- The brotli patch level (1.2.1) cannot be confirmed from the binary itself; the upstream changelog is the only evidence.

## Downloads

| Platform | File |
|---|---|
| Windows x64 | `JPEGView-x64-v1.3.46-20261121.zip` |
| Windows x86 | `JPEGView-win32-v1.3.46-20261121.zip` |

Extract and run `JPEGView.exe`; no installer required.
