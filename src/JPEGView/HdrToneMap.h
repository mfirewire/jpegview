// HdrToneMap.h — HDR (PQ/HLG) to SDR tone mapping for JPEGView.
//
// Pure functions, no MFC / no Windows headers, so the same code can be
// unit-tested on Linux. All constants are from the standards:
//   PQ  : SMPTE ST 2084 (m1/m2/c1/c2/c3, 10000-nit peak)
//   HLG : ARIB STD-B67 / ITU-R BT.2100 (OETF a/b/c, OOTF system gamma)
//   EETF: ITU-R BT.2390 (Hermite spline, content-adaptive source peak)
//   Gamut matrices: BT.2100 / GTK gdkcolordefs (same values the
//   kennylevinsen/hdr-test-images generator uses).
//
// Design (verified against kennylevinsen/hdr-test-images in Python before
// this file was written — PQ/HLG BT.2020 and P3 all reproduce
// test_reference.jpg with mean abs diff 0.03):
//   signal (PQ/HLG, source gamut) -> display-linear (SDR white = 1.0)
//   -> BT.2390 EETF (content-adaptive peak; identity when the content
//   peak is at or below SDR white, so the test set is reproduced exactly)
//   -> gamut matrix inverse (BT.2020/P3 -> sRGB) -> gamut mapping
//   (desaturate out-of-gamut toward luminance) -> sRGB OETF -> 8-bit.
//
// The EETF is applied in the PQ signal domain (where BT.2390 defines it).
// For HLG the display-linear values are taken through a PQ round-trip
// (linear -> nits -> PQ signal -> EETF -> nits -> linear) so both
// transfers share one highlight roll-off. The source peak LW for the
// EETF is content-adaptive: LW = max(content peak nits, SDR white).
// When the content peak is at or below SDR white (as in the test set)
// LW == SDR white, KS == maxLum == 1 and the EETF is the identity, so
// the test images are reproduced exactly. For real HDR content above
// SDR white the EETF rolls the highlights off smoothly (monotonic,
// bounded by SDR white) instead of hard-clipping them.
//
// SDR content (sRGB transfer, or no CICP) must never reach this code:
// callers gate on HdrToneMap::IsHdr() first, so SDR takes the exact
// pre-existing path and stays byte-identical.

#pragma once

#include <cstddef>
#include <cstdint>

namespace HdrToneMap {

// CICP (ISO/IEC 23001-17 / ITU-T H.273) codes.
enum class Transfer : int {
    kUnknown = 0,
    kSrgb = 13,  // IEC 61966-2-1 sRGB
    kPq = 16,    // SMPTE ST 2084 (PQ)
    kHlg = 18,   // ARIB STD-B67 (HLG)
};

enum class Primaries : int {
    kUnknown = 0,
    kBt709 = 1,   // BT.709 / sRGB
    kBt2020 = 9,  // BT.2020
    kP3Dci = 11,  // DCI-P3 (SMPTE RP 431-2, DCI white 0.314, 0.351)
    kP3 = 12,     // Display P3 (DCI-P3, D65)
};

// One CICP (colour description) as carried by a PNG cICP chunk, an
// AVIF/HEIF nclx box, or a JXL colour encoding.
struct Cicp {
    int primaries = 0;  // CICP colour primaries code
    int transfer = 0;   // CICP transfer characteristics code
    int matrix = 0;     // CICP matrix coefficients code (unused here)
    int fullRange = 0;  // 0 = studio/limited, 1 = full range
};

Transfer TransferFromCode(int code);
Primaries PrimariesFromCode(int code);

// True when the CICP describes an HDR transfer (PQ or HLG). This is the
// only gate callers need: SDR (sRGB transfer, unknown transfer, or no
// CICP) returns false and must take the pre-existing decode path.
bool IsHdr(const Cicp& cicp);

// SDR white in nits, and the HDR target peak in nits. These are the
// values the kennylevinsen test set is built around (its generator uses
// sdr_white_nits=203 and a 1000-nit HLG display); they are choices, not
// standard constants, and changing them changes the rendering.
const double kSdrWhiteNits = 203.0;
const double kHdrPeakNits = 1000.0;

// sRGB luma coefficients (Y row of the RGB-to-XYZ matrix). These are
// the weights ComputePeakLinear uses for the content peak, and the
// gain-map fused path (GainMap.cpp) replicates that peak from byte
// inputs, so the weights live here, not in HdrToneMap.cpp.
const double kSrgbLuma[3] = {0.2126, 0.7152, 0.0722};

// PQ (SMPTE ST 2084) EOTF inverse: PQ signal [0,1] -> absolute nits.
// Clamps the signal to [0,1] and the intermediate fraction to >= 0, so
// out-of-range or NaN inputs cannot produce NaN (they map to 0 nits).
double PqSignalToNits(double signal);
// PQ OETF: absolute nits -> PQ signal [0,1].
double PqNitsToSignal(double nits);

// HLG (ARIB STD-B67 / BT.2100) OETF inverse: HLG signal [0,1] ->
// scene-linear [0,1]. Out-of-range signals are clamped to [0,1]
// (P2-C10): super-white is real (BT.2100 Note 5h), but the inverse
// OETF's exp diverges there and the EETF saturates to white anyway.
double HlgSignalToSceneLinear(double signal);

// BT.2390 EETF in the PQ signal domain (content-adaptive source peak).
// `signal` is a PQ signal [0,1]; `peakNits` is the content peak in nits
// (LW = max(peakNits, kSdrWhiteNits)). Returns the tone-mapped PQ
// signal. When peakNits <= kSdrWhiteNits the EETF is the identity.
// The Hermite spline is the BT.2390 one (value KS and tangent 1 at the
// knee, value maxLum and tangent 0 at the top); KS is derived from
// maxLum as KS = 1.5*maxLum - 0.5.
double Bt2390EetfPq(double signal, double peakNits);

// BT.2390 EETF parameters, precomputed once per image (P2-C3 hoisting).
// `pqLw` is the PQ signal of the source peak LW (clamped to >= SDR
// white); `denom` is pqLw - pqLb, and pqLb is 0 (black level), so
// denom == pqLw; `maxLum` is pqLmax / denom (PQ signal of SDR white
// over denom); `ks` is 1.5 * maxLum - 0.5 (the spline knee).
struct Bt2390Params {
    double pqLw = 0.0;
    double denom = 0.0;
    double maxLum = 0.0;
    double ks = 0.0;
};

// Precompute the BT.2390 parameters for a content peak (nits). This is
// the preamble of Bt2390EetfPq, hoisted out of the per-pixel loop.
Bt2390Params Bt2390MakeParams(double peakNits);

// Inner BT.2390 EETF with precomputed parameters. Bit-identical to
// Bt2390EetfPq(signal, peakNits) when params ==
// Bt2390MakeParams(peakNits). Public so the gain-map fused path
// (GainMap.cpp) can build its table with the same function the
// per-pixel loop uses, with the parameters built once.
double Bt2390EetfPqWithParams(double signal, const Bt2390Params& params);

// Gamut mapping: if the linear sRGB colour is out of [0,1]^3,
// desaturate it toward its luminance (grey) until it is in gamut
// (preserves hue and luminance; never hard-clips a single channel).
// Public so the gain-map fused path can run the same step the
// per-pixel loop runs, after its table lookup.
void GamutMap(double* r, double* g, double* b);

// Gamut inverse matrix (source primaries -> sRGB), or NULL for sRGB
// (BT.709: identity, no matrix needed). Public so the 16-bit
// gain-map apply (GainMap.cpp) can run the same step
// ConvertPixelWithParams runs, after its own signal->linear
// conversion. Moved out of the anonymous namespace in HdrToneMap.cpp
// for this (like GamutMap/ToByte before it).
const double (*GamutInverse(Primaries primaries))[3];

// Luma coefficients (Y row of the RGB-to-XYZ matrix) for the given
// primaries (sRGB coefficients for BT.709/unknown). Public so the
// 16-bit gain-map apply can compute the content peak in the source
// gamut (approach (a) in the NB-1 plan: by luminance invariance this
// equals the post-inverse sRGB peak).
const double* GamutLuma(Primaries primaries);

// HLG OOTF forward (BT.2100): scene-linear RGB -> display-linear RGB
// (SDR white = 1.0) in the target gamut. Public so the 16-bit
// gain-map apply can run the same HLG conversion ComputePeakNits
// runs, without its EETF.
void HlgOotfForward(double sceneR, double sceneG, double sceneB, const double luma[3],
                    double* outR, double* outG, double* outB);

// Linear sRGB code -> 8-bit byte: Clamp01 * 255 + 0.5, round to
// nearest. Public so the gain-map fused path can finish pixels
// exactly the way the per-pixel loop does.
uint8_t ToByte(double srgb01);

// sRGB OETF: linear [0,1] -> sRGB code [0,1] (piecewise).
double SrgbOetf(double linear);

// Convert one float RGB pixel from an HDR signal to 8-bit sRGB.
// `r/g/b` are signal values in [0,1] in the source transfer and gamut.
// `peakNits` is the content peak in nits (for the EETF); pass a value
// <= kSdrWhiteNits to disable the highlight roll-off (identity).
// Output is sRGB bytes (R, G, B).
void ConvertPixel(double r, double g, double b, Transfer transfer, Primaries primaries,
                  double peakNits, uint8_t& outR, uint8_t& outG, uint8_t& outB);

// ConvertPixel with precomputed EETF parameters (no peakNits param).
// Bit-identical to ConvertPixel(..., peakNits, ...) when
// params == Bt2390MakeParams(peakNits). The wrappers use this in their
// per-pixel loops so the EETF preamble runs once per image.
void ConvertPixelWithParams(double r, double g, double b, Transfer transfer, Primaries primaries,
                            const Bt2390Params& params, uint8_t& outR, uint8_t& outG, uint8_t& outB);

// Compute the content peak in nits from an interleaved float RGB signal
// buffer (pixelCount pixels, 3 floats each, values in [0,1], source
// transfer/gamut). Used to drive the content-adaptive EETF.
double ComputePeakNits(const float* srcRgb, size_t pixelCount, Transfer transfer, Primaries primaries);

// ---- Linear-float input (Radiance .hdr / OpenEXR) ----
//
// These take scene-linear float RGB (sRGB primaries), the format
// HDRWrapper (.hdr) and EXRWrapper (.exr) decode to. Linear 1.0 is SDR
// white (kSdrWhiteNits): a pixel at linear 1.0 maps to sRGB 255, and
// the BT.2390 EETF is the identity when the content peak is at or below
// linear 1.0, so SDR-range content passes through the sRGB OETF
// unchanged (up to the sRGB-OETF-vs-2.2-gamma difference from the old
// pow(v, 1/2.2) path, which is at most -9 levels in the deep shadows
// and -3..+2 in the mid-tones, and is the standards-correct curve;
// that is the only deviation while the content peak is at or below
// linear 1.0 (above 1.0 the EETF roll-off adds its own). Linear
// values above 1.0 are HDR: the content peak (in nits, peakLinear *
// kSdrWhiteNits) drives the same BT.2390 EETF as the PQ/HLG path, so
// highlights roll off smoothly to SDR white instead of hard-clipping.
//
// NOTE: this anchors linear 1.0 to SDR white. An earlier draft mapped
// the content peak to kHdrPeakNits (1000 nits) instead; that is wrong
// for SDR-range content, because it re-exposes the whole image (a
// linear 0.18 grey came out at 223 instead of 118). Verified
// numerically in Python before this was written.

// Compute the content peak in scene-linear units from an interleaved
// float RGB buffer (width x height, 3 floats per pixel, srcStrideFloats
// floats between rows). Returns the max linear luminance (sRGB luma
// coefficients). Returns 0.0 when there is no positive value. A single
// very hot pixel (sun, light source) can dominate the peak and darken
// the rest; v1 uses the max for consistency with ComputePeakNits
// (PQ/HLG). No percentile/exposure control in v1.
double ComputePeakLinear(const float* srcRgb, int width, int height, size_t srcStrideFloats);

// Convert one scene-linear float RGB pixel (sRGB primaries) to 8-bit
// sRGB. `peakLinear` is the content peak from ComputePeakLinear.
// Negative/NaN inputs map to 0. Output is sRGB bytes (R, G, B).
void ConvertLinearPixel(double r, double g, double b, double peakLinear,
                        uint8_t& outR, uint8_t& outG, uint8_t& outB);

// ConvertLinearPixel with precomputed EETF parameters. Bit-identical to
// ConvertLinearPixel(..., peakLinear, ...) when params ==
// Bt2390MakeParams(peakLinear * kSdrWhiteNits, clamped to >= SDR
// white). ConvertLinearImage/ToBgra use this so the EETF preamble runs
// once per image.
void ConvertLinearPixelWithParams(double r, double g, double b, double peakLinear,
                                  const Bt2390Params& params, uint8_t& outR, uint8_t& outG, uint8_t& outB);

// Convert a whole image of interleaved float RGB (width x height x 3
// floats, scene-linear, sRGB primaries) to 8-bit sRGB BGR (width x
// height x 3 bytes, B,G,R, the JPEGView pixel order). Computes the
// content peak internally. `srcStrideFloats` is the float stride between
// rows (usually width*3); `dstStrideBytes` the byte stride between rows
// (padded).
void ConvertLinearImage(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                        uint8_t* dstBgr, size_t dstStrideBytes);

// Convert a whole image of interleaved float RGB (width x height x 3
// floats, scene-linear, sRGB primaries) to 8-bit sRGB BGRA (width x
// height x 4 bytes, B,G,R,A, the JPEGView pixel order with alpha).
// Computes the content peak internally. `srcStrideFloats` is the float
// stride between rows (usually width*3); `dstStrideBytes` the byte
// stride between rows (padded). `planarAlphaOrNull` is an optional
// per-pixel alpha plane: when non-NULL, the alpha byte of each pixel
// is read from it (`alphaStride` is the byte stride between
// consecutive pixels in the plane, usually 1; a row is width *
// alphaStride bytes, so an interleaved source can pass a pointer
// into its own BGRA buffer with a stride of 4); when NULL, every
// alpha byte is 0xff (opaque). Every byte of the BGRA output is
// written (H-01/N-3: the old ConvertLinearImage path left bytes
// uninitialized for 4-channel PSDs). This is the variant PSDWrapper
// uses for HDR PSDs, and Batch 4 reuses it for the AVIF/HEIF
// gain-map repack.
void ConvertLinearImageToBgra(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                              uint8_t* dstBgra, size_t dstStrideBytes,
                              const uint8_t* planarAlphaOrNull, size_t alphaStride);

}  // namespace HdrToneMap
