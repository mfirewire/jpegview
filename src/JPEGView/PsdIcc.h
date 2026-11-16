// PsdIcc.h — build a PsdHdr::ColorSpace from a PSD file's ICC profile
// (the 0x040F image resource), for the PSD HDR decode path (H-02).
//
// Pure C++ (no MFC / no Windows headers): it compiles on Linux as well
// as in the MSVC project, so the math can be verified without a Windows
// run. It intentionally does NOT include stdafx.h (precompiled headers
// are disabled for this file in the vcxproj); it only needs the C++
// standard library plus lcms2.h, which resolves to the vendored lcms2
// headers on Windows (on the MSVC IncludePath) and to the system
// header on Linux. lcms2.lib is already linked by the project; no new
// library dependency.
//
// What this does:
// ---------------
// BuildColorSpace reads the TRC (tone-reproduction curve) tags and the
// primaries (colorant) + white-point tags out of an ICC profile and
// turns them into what PsdHdr::DecodeImageData needs:
//   * per-channel 16-bit linearization tables (TRC evaluated at every
//     16-bit code, encoded -> linear), and
//   * a 3x3 matrix converting linear source RGB to linear sRGB.
//
// The TRC as stored in the profile maps encoded -> linear (verified
// empirically with lcms2 on an sRGB profile: cmsEvalToneCurveFloat
// (trc, 0.5) = 0.214041, the sRGB EOTF value — NOT the inverse), so
// the table is built by evaluating the stored curve directly. Do NOT
// use cmsReverseToneCurve here: that maps linear -> encoded (0.735 at
// 0.5) and would be the wrong direction for linearization.
//
// sRGB detection: a profile whose colorants and white point match
// sRGB (within 1e-3 per component) AND whose red TRC at 0.5 matches
// the sRGB EOTF value 0.2140 (within 1e-3) is treated as sRGB, and
// BuildColorSpace returns false — the caller then passes NULL to
// DecodeImageData and the sRGB path stays byte-identical to the
// pre-H-02 code. The white point may be either D50 (0.9642, 1.0,
// 0.8249, as in lcms2's cmsCreate_sRGBProfile) or D65 (0.9505, 1.0,
// 1.0890): the standard sRGB IEC 61966-2-1 profile stores D65 in
// its wtpt tag even though its colorants are D50 values, and
// accepting D50 only mis-detects every real sRGB file as non-sRGB
// (regression fixed 2026-11-16: the D65 wtpt with D50 colorants
// built a diagonal matrix of 0.827/1.022/1.370 — a massive blue
// shift). The 1e-3 tolerance is loose enough to absorb the float
// rounding of the stored colorant values (lcms2 returns the sRGB
// colorants as e.g. R=(0.4360, 0.2225, 0.0139) vs the spec
// (0.4360657, 0.2224884, 0.0139160)) yet tight enough to separate
// real spaces: Adobe RGB's TRC at 0.5 is ~0.2176 (gamma 2.2) and its
// colorants differ from sRGB by far more than 1e-3; ProPhoto differs
// on both counts as well.
//
// D50/D65 note: ICC colorants are stored in the profile connection
// space (D50 white point). The primaries matrix is therefore built
// against the D50 sRGB XYZ->RGB matrix (the Bradford-adapted sRGB
// matrix, standard published values) — NOT the familiar D65 matrix
// from IEC 61966-2-1, which would mix D50 XYZ with a D65 matrix and
// give visibly wrong colours for a D65 profile such as Adobe RGB
// (matrix[0] 1.488 instead of 1.398; verified against lcms2's own
// transform). The D50 matrix folds the Bradford D50->D65 chromatic
// adaptation into the fixed matrix (it equals the D65 matrix times
// the Bradford D50->D65 matrix to 5e-4), so no separate adaptation
// step is needed. Residual error from the published matrix
// coefficients and the s15Fixed16 storage of the colorants is
// ~1e-4 per matrix entry.
//
// Failure policy: any missing tag, unparseable profile, non-RGB/gray
// colour space, singular primaries matrix, or non-finite matrix entry
// returns false, and the caller falls back to the sRGB path. A corrupt
// profile must never produce NaN pixels, so the matrix is sanity-
// checked for finiteness and the TRC table values are clamped to
// [0, 1] (a malformed TRC could return out-of-range values).
// No exception escapes this file (bad_alloc from the table storage is
// caught and reported as false).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "PsdHdr.h"

namespace PsdIcc {

// Build a PsdHdr::ColorSpace from an ICC profile.
//   iccData / iccSize : the ICC profile bytes (the 0x040F resource),
//     may be NULL/0.
//   out : filled with the TRC table pointers and the primaries matrix.
//   trcStorage : caller-owned std::vector<float>; the tables are
//     appended here (3*65536 floats for RGB, 1*65536 for gray) and
//     out.trc[] points into it. The caller must keep trcStorage alive
//     until DecodeImageData returns.
// Returns true when a non-sRGB colour space was built (caller passes
// &out to DecodeImageData). Returns false when the profile is missing,
// unparseable, not RGB/gray, or IS sRGB (caller passes NULL to
// DecodeImageData and keeps the existing sRGB path byte-identical).
bool BuildColorSpace(const uint8_t* iccData, size_t iccSize,
                     PsdHdr::ColorSpace& out, std::vector<float>& trcStorage);

}  // namespace PsdIcc
