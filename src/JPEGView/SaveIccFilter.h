// SaveIccFilter.h — save-path ICC segment filter (N2) and the raw-byte
// sRGB classifier it uses.
//
// Pure C++ (no MFC / no Windows headers): it compiles on Linux as well
// as in the MSVC project, so the filter can be verified against real
// ICC profiles without a Windows run. It intentionally does NOT include
// stdafx.h (precompiled headers are disabled for this file in the
// vcxproj); it only needs the C++ standard library.
//
// What this does:
// ---------------
// FilterNonRGBICCProfileSegments drops ICC_PROFILE APP2 chunks from the
// preserved JPEG segments when the embedded profile does not match the
// pixels being saved (the saved pixels are always RGB, converted to
// sRGB on load when UseEmbeddedColorProfiles() is on):
//   * a profile whose colour space is not RGB (e.g. CMYK) is always
//     dropped — the saved pixels are RGB either way;
//   * an RGB profile that is provably NOT sRGB (Adobe RGB, Display P3,
//     ProPhoto, ...) is dropped, but ONLY when bStripNonSrgbRgb is
//     true. The caller passes CSettingsProvider::This()
//     .UseEmbeddedColorProfiles(): when that setting is off the pixels
//     were not converted on load, so the source profile still
//     describes them and must be kept.
// An sRGB profile, or anything unrecognisable, is kept verbatim:
// failing safe preserves metadata rather than dropping it on a guess.
//
// sRGB is decided from the raw ICC bytes, NOT with lcms2 (the save
// path deliberately avoids lcms2). The colourant tags (rXYZ/gXYZ/bXYZ)
// are stored in the profile connection space, i.e. D50, so they are
// compared against the D50 sRGB values (0.4361/0.2225/0.0139,
// 0.3851/0.7169/0.0971, 0.1431/0.0606/0.7141 — the same references
// ICCProfileTransform::IsNoTransformNeeded uses), with a 0.05
// Euclidean tolerance per primary, all three required. The white
// point may be either D65 (0.9505, 1.0, 1.0890, as stored by the
// standard sRGB IEC 61966-2-1 profile and by real Photoshop/Apple
// profiles) or D50 (0.9642, 1.0, 0.8249, as stored by lcms2-built
// profiles), same 0.05 tolerance. Comparing the stored colourants
// against the D65-native values (0.4124, ...) instead would be wrong:
// the stored blue primary is 0.24 away from the D65 value, so every
// real sRGB profile would be mis-detected as non-sRGB and stripped
// (the mirror image of the H-02 D50-only failure, which mis-detected
// every real sRGB profile the other way).
//
// Only chunk 1 of a multi-chunk profile is read: the tag table and the
// four XYZ tags must all fit inside chunk 1's profile bytes, or the
// profile is kept (kUnknown). Real matrix profiles are a few KB at
// most and always fit in one chunk; a profile whose colourant tags
// spill past chunk 1 is kept as a residual mis-tag risk, same
// fail-safe philosophy as the rest of the filter. A LUT-based RGB
// profile (no colourant tags) cannot be proven sRGB and is likewise
// kept.

#pragma once

#include <cstddef>
#include <cstdint>

enum class IccSrgbVerdict {
    kSrgb,     // provably sRGB: keep the profile
    kNonSrgb,  // provably not sRGB: strip it (when the caller allows)
    kUnknown,  // unparseable / missing tags / not RGB: keep (fail-safe)
};

// Classify an ICC profile from its raw bytes.
//   pProfile / nProfileSize : the profile bytes (chunk 1's profile
//     data is enough for real matrix profiles; see the note above).
// Returns kSrgb when the colourants and white point match sRGB,
// kNonSrgb when they parse cleanly but do not match, kUnknown when
// the profile cannot be classified (too short, non-RGB colour space,
// tag table or tag data out of bounds, missing/wrong-typed tags).
IccSrgbVerdict ClassifyIccProfile(const uint8_t* pProfile, size_t nProfileSize);

// Drop stale ICC_PROFILE APP2 chunks from preserved JPEG segments.
// The filter was moved here verbatim from SaveImage.cpp (F-H10) so it
// can be unit-tested on Linux; only the bStripNonSrgbRgb gate (N2)
// was added. Contract (unchanged apart from the gate):
//   * pSegments is the preserved APP/COM segment blob in source order
//     (each segment: FF marker, BE16 length counting itself, payload);
//     nSegmentsLen <= 16 MB (capped by the F-H10 walker).
//   * A profile may span several APP2 chunks; only chunk 1 carries
//     the header, so the decision reads chunk 1 and the strip drops
//     all ICC chunks.
//   * Anything unrecognisable (no chunk 1, truncated chunk 1,
//     malformed segment chain, allocation failure) keeps the segments
//     untouched.
// Returns true when the caller must use the filtered blob:
// *ppFilteredSegments is the new buffer, owned by the caller (NULL
// when every segment was ICC and nothing is left); false keeps the
// original blob unchanged.
bool FilterNonRGBICCProfileSegments(const unsigned char* pSegments, int nSegmentsLen, int nSegmentsBeforeEXIF,
    unsigned char** ppFilteredSegments, int& nFilteredLen, int& nFilteredBeforeEXIF, bool bStripNonSrgbRgb);
