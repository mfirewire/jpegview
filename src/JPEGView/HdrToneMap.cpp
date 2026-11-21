// HdrToneMap.cpp — see HdrToneMap.h for the design and the verification
// note. All transfer constants are the exact standard values (PQ m1/m2/
// c1/c2/c3 are the ST 2084 fractions; HLG a/b/c are the ARIB STD-B67
// constants; the gamut matrices match GTK gdkcolordefs and the
// kennylevinsen test generator).
//
// Pure C++ (no MFC/Windows): it compiles on Linux as well as in the MSVC
// project, so the math can be verified against the kennylevinsen test
// images without a Windows run. It intentionally does NOT include
// stdafx.h (precompiled headers are disabled for this file in the
// vcxproj); it only needs <algorithm> and <cmath>.
#include "HdrToneMap.h"

#include <algorithm>
#include <cmath>

namespace HdrToneMap {

namespace {

// PQ (SMPTE ST 2084) constants.
const double kPqM1 = 2610.0 / 16384.0;   // 0.1593017578125
const double kPqM2 = 2523.0 / 32.0;  // 78.84375
const double kPqC1 = 3424.0 / 4096.0;   // 0.8359375
const double kPqC2 = 2413.0 / 128.0;    // 18.8515625
const double kPqC3 = 2392.0 / 128.0;    // 18.6875
const double kPqPeakNits = 10000.0;

// HLG (ARIB STD-B67 / BT.2100) OETF constants.
const double kHlgA = 0.17883277;
const double kHlgB = 0.28466892;
const double kHlgC = 0.55991073;
// HLG OOTF system gamma for a kHdrPeakNits display (BT.2100).
const double kHlgSystemGamma = 1.2;

// Inverses (BT.2020/P3 -> sRGB), computed from the standard forward
// matrices (numpy, float64). Values match GTK gdkcolordefs and the
// kennylevinsen test generator. Verified by round-trip: max error 2.2e-16.
// (P3-C2: the forward matrices were deleted here; only the inverses
// have readers, via GamutInverse.)
const double kRec2020ToSrgb[3][3] = {
    { 1.660226, -0.587547, -0.072839},
    {-0.124553,  1.132926, -0.008350},
    {-0.018155, -0.100603,  1.118999},
};
// P3 -> sRGB (inverse of kSrgbToP3).
const double kP3ToSrgb[3][3] = {
    { 1.224940, -0.224940,  0.000000},
    {-0.042057,  1.042057,  0.000000},
    {-0.019638, -0.078635,  1.098274},
};

// Luma coefficients (Y row of the RGB-to-XYZ matrix).
const double kRec2020Luma[3] = {0.2627, 0.6780, 0.0593};
const double kP3Luma[3] = {0.2290, 0.6917, 0.0793};
const double kSrgbLuma[3] = {0.2126, 0.7152, 0.0722};

const double (*GamutInverse(Primaries primaries))[3] {
    switch (primaries) {
        case Primaries::kBt2020: return kRec2020ToSrgb;
        case Primaries::kP3: return kP3ToSrgb;
        default: return NULL;  // BT.709 / sRGB: identity, no matrix needed.
    }
}

const double* GamutLuma(Primaries primaries) {
    switch (primaries) {
        case Primaries::kBt2020: return kRec2020Luma;
        case Primaries::kP3: return kP3Luma;
        default: return kSrgbLuma;
    }
}

double Clamp01(double v) {
    if (!(v >= 0.0)) return 0.0;  // also catches NaN
    if (v > 1.0) return 1.0;
    return v;
}

// HLG OOTF forward (BT.2100): scene-linear RGB -> display-linear RGB,
// in the target gamut. Display-linear is in units where 1.0 is SDR
// white on a kHdrPeakNits display. Derived as the exact inverse of the
// generator's hlg_inverse_ootf (verified by round-trip in Python):
//   display = scene * luminance_scale * Ys^(gamma-1),
// where Ys is the scene luminance and luminance_scale is
// kHdrPeakNits / kSdrWhiteNits.
void HlgOotfForward(double sceneR, double sceneG, double sceneB, const double luma[3],
                    double* outR, double* outG, double* outB) {
    const double luminanceScale = kHdrPeakNits / kSdrWhiteNits;
    double ys = sceneR * luma[0] + sceneG * luma[1] + sceneB * luma[2];
    if (!(ys > 0.0)) {
        *outR = 0.0;
        *outG = 0.0;
        *outB = 0.0;
        return;
    }
    double scale = luminanceScale * std::pow(ys, kHlgSystemGamma - 1.0);
    *outR = sceneR * scale;
    *outG = sceneG * scale;
    *outB = sceneB * scale;
}

// Gamut mapping: if the linear sRGB colour is out of [0,1]^3, desaturate
// it toward its luminance (grey) until it is in gamut. This preserves
// hue and luminance and never hard-clips a single channel (which would
// shift hue). If the luminance itself is out of [0,1] the colour is
// unrepresentable and is clamped after desaturation.
void GamutMap(double* r, double* g, double* b) {
    double y = (*r) * kSrgbLuma[0] + (*g) * kSrgbLuma[1] + (*b) * kSrgbLuma[2];
    // Largest t in [0,1] such that lerp((r,g,b), (y,y,y), t) is in gamut.
    // For each channel out of range, solve for the t that brings it to the
    // boundary; take the max over channels.
    double t = 0.0;
    const double c[3] = {*r, *g, *b};
    for (int i = 0; i < 3; i++) {
        double ci = c[i];
        if (ci < 0.0) {
            // lerp(ci, y, t) = 0 -> t = -ci / (y - ci)
            double denom = y - ci;
            if (denom > 0.0) t = std::max(t, -ci / denom);
        } else if (ci > 1.0) {
            // lerp(ci, y, t) = 1 -> t = (ci - 1) / (ci - y)
            double denom = ci - y;
            if (denom > 0.0) t = std::max(t, (ci - 1.0) / denom);
        }
    }
    if (t > 0.0) {
        if (t > 1.0) t = 1.0;
        *r = c[0] + (y - c[0]) * t;
        *g = c[1] + (y - c[1]) * t;
        *b = c[2] + (y - c[2]) * t;
    }
    // Safety clamp (only reached when the luminance itself is out of range).
    *r = Clamp01(*r);
    *g = Clamp01(*g);
    *b = Clamp01(*b);
}

uint8_t ToByte(double srgb01) {
    double v = Clamp01(srgb01) * 255.0 + 0.5;
    if (!(v >= 0.0)) return 0;
    if (v > 255.0) return 255;
    return (uint8_t)(int)v;
}

}  // namespace

Transfer TransferFromCode(int code) {
    switch (code) {
        case 13: return Transfer::kSrgb;
        case 16: return Transfer::kPq;
        case 18: return Transfer::kHlg;
        default: return Transfer::kUnknown;
    }
}

Primaries PrimariesFromCode(int code) {
    switch (code) {
        case 1: return Primaries::kBt709;
        case 9: return Primaries::kBt2020;
        case 12: return Primaries::kP3;
        default: return Primaries::kUnknown;
    }
}

bool IsHdr(const Cicp& cicp) {
    Transfer t = TransferFromCode(cicp.transfer);
    return t == Transfer::kPq || t == Transfer::kHlg;
}

double PqSignalToNits(double signal) {
    double s = Clamp01(signal);
    double p = std::pow(s, 1.0 / kPqM2);
    double frac = (p - kPqC1) / (kPqC2 - kPqC3 * p);
    if (!(frac > 0.0)) return 0.0;
    return std::pow(frac, 1.0 / kPqM1) * kPqPeakNits;
}

double PqNitsToSignal(double nits) {
    double lm = nits / kPqPeakNits;
    if (!(lm > 0.0)) return 0.0;
    // P2-C1: Inf and huge values saturate to 1.0 (PQ peak). NaN is
    // already handled by the line above (NaN > 0.0 is false).
    if (lm >= 1.0) return 1.0;
    double lmM1 = std::pow(lm, kPqM1);
    return std::pow((kPqC1 + kPqC2 * lmM1) / (1.0 + kPqC3 * lmM1), kPqM2);
}

double HlgSignalToSceneLinear(double signal) {
    // P2-C10: clamp the signal to [0,1] first. This is pipeline
    // saturation, not "HLG is only defined in [0,1]" - BT.2100 Note 5h
    // allows production signals above 1 (super-white is real). But the
    // inverse OETF's exp diverges there (Hlg(2) is 262, Hlg(128) is
    // Inf), and the EETF saturates to white anyway, so an unclamped
    // super-white pixel only raises the content peak and darkens the
    // whole image (a single Hlg(2) pixel gave a ~800k-nit peak).
    // Both callers (ComputePeakNits, ConvertPixel) pass the signal
    // only through this function, so this one clamp covers them.
    // (The sign ternaries below are now dead - s is never negative -
    // but are left as-is.)
    double s = Clamp01(signal);
    double as = std::fabs(s);
    if (as <= 0.5) {
        return (s < 0.0 ? -1.0 : 1.0) * (as * as) / 3.0;
    }
    return (s < 0.0 ? -1.0 : 1.0) * (std::exp((as - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

// Precompute the BT.2390 EETF parameters for a content peak (nits).
// This is the preamble of Bt2390EetfPq, hoisted out of the per-pixel
// loop (P2-C3). denom is provably > 0 here: lw >= 203 nits, so
// PqNitsToSignal(lw) > 0 (the old `denom > 0` early return was dead).
Bt2390Params Bt2390MakeParams(double peakNits) {
    double lw = peakNits;
    if (!(lw > kSdrWhiteNits)) lw = kSdrWhiteNits;
    double pqLw = PqNitsToSignal(lw);
    double pqLmax = PqNitsToSignal(kSdrWhiteNits);
    // pqLb is 0 (black level), so denom == pqLw (P3-C2).
    double denom = pqLw;
    double maxLum = pqLmax / denom;
    double ks = 1.5 * maxLum - 0.5;
    Bt2390Params params;
    params.pqLw = pqLw;
    params.denom = denom;
    params.maxLum = maxLum;
    params.ks = ks;
    return params;
}

// Inner BT.2390 EETF with precomputed parameters (P2-C3 hoisting).
// Bit-identical to Bt2390EetfPq(signal, peakNits) when
// params == Bt2390MakeParams(peakNits). Internal to this file; the
// public Bt2390EetfPq is a thin wrapper over it.
static double Bt2390EetfPqWithParams(double signal, const Bt2390Params& params) {
    // Identity fast path: ks == 1 (peak <= SDR white, so maxLum == 1)
    // means the EETF is min(Clamp01(signal), pqLw). This is the same
    // fast path Bt2390EetfPq had, now pow-free (pqLw is precomputed).
    if (params.ks >= 1.0) {
        double c = Clamp01(signal);
        return c < params.pqLw ? c : params.pqLw;
    }
    // Normalised PQ signal (source white = 1).
    double e1 = Clamp01(signal) / params.pqLw;
    if (!(e1 < 1.0)) e1 = 1.0;
    double e2;
    if (e1 < params.ks || !(params.ks < 1.0)) {
        e2 = e1;
    } else {
        double t = (e1 - params.ks) / (1.0 - params.ks);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        double t2 = t * t;
        double t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * params.ks
           + (t3 - 2.0 * t2 + t) * (1.0 - params.ks)
           + (-2.0 * t3 + 3.0 * t2) * params.maxLum;
    }
    // Denormalise back to a PQ signal for the target display.
    return PqNitsToSignal(PqSignalToNits(e2 * params.denom));
}

double Bt2390EetfPq(double signal, double peakNits) {
    return Bt2390EetfPqWithParams(signal, Bt2390MakeParams(peakNits));
}

double SrgbOetf(double linear) {
    double v = linear;
    if (v < 0.0) v = 0.0;
    if (v > 0.0031308) {
        return 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    }
    return 12.92 * v;
}

double ComputePeakNits(const float* srcRgb, size_t pixelCount, Transfer transfer, Primaries primaries) {
    if (srcRgb == NULL || pixelCount == 0) return kSdrWhiteNits;
    const double* luma = GamutLuma(primaries);
    double peak = 0.0;
    if (transfer == Transfer::kPq) {
        for (size_t i = 0; i < pixelCount; i++) {
            double r = (double)srcRgb[i * 3 + 0];
            double g = (double)srcRgb[i * 3 + 1];
            double b = (double)srcRgb[i * 3 + 2];
            double n = PqSignalToNits(r);
            if (n > peak) peak = n;
            n = PqSignalToNits(g);
            if (n > peak) peak = n;
            n = PqSignalToNits(b);
            if (n > peak) peak = n;
        }
    } else if (transfer == Transfer::kHlg) {
        for (size_t i = 0; i < pixelCount; i++) {
            double sr = HlgSignalToSceneLinear((double)srcRgb[i * 3 + 0]);
            double sg = HlgSignalToSceneLinear((double)srcRgb[i * 3 + 1]);
            double sb = HlgSignalToSceneLinear((double)srcRgb[i * 3 + 2]);
            double dr = 0.0, dg = 0.0, db = 0.0;
            HlgOotfForward(sr, sg, sb, luma, &dr, &dg, &db);
            // Display-linear is in SDR-white units; peak nits = max * SDR white.
            if (dr > peak) peak = dr;
            if (dg > peak) peak = dg;
            if (db > peak) peak = db;
        }
        peak *= kSdrWhiteNits;
    } else {
        return kSdrWhiteNits;
    }
    // P2-C2: a non-finite peak (Inf from a single HLG super-white pixel)
    // falls back to SDR white instead of poisoning the EETF.
    if (!(peak > 0.0) || !std::isfinite(peak)) return kSdrWhiteNits;
    return peak;
}

void ConvertPixelWithParams(double r, double g, double b, Transfer transfer, Primaries primaries,
                            const Bt2390Params& params, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    const double* luma = GamutLuma(primaries);
    double linR = 0.0, linG = 0.0, linB = 0.0;  // display-linear, source gamut, SDR white = 1
    if (transfer == Transfer::kPq) {
        // Tone-map in the PQ signal domain first (BT.2390 EETF), then
        // convert to display-linear. This is where the highlight
        // roll-off lives; for content at/below SDR white it is identity.
        double sr = Bt2390EetfPqWithParams(Clamp01(r), params);
        double sg = Bt2390EetfPqWithParams(Clamp01(g), params);
        double sb = Bt2390EetfPqWithParams(Clamp01(b), params);
        linR = PqSignalToNits(sr) / kSdrWhiteNits;
        linG = PqSignalToNits(sg) / kSdrWhiteNits;
        linB = PqSignalToNits(sb) / kSdrWhiteNits;
    } else if (transfer == Transfer::kHlg) {
        // HLG signal -> scene-linear -> OOTF -> display-linear, then the
        // same PQ-domain EETF via a round-trip (display-linear -> nits ->
        // PQ signal -> EETF -> nits -> display-linear) so both transfers
        // share one highlight roll-off.
        double sr = HlgSignalToSceneLinear(r);
        double sg = HlgSignalToSceneLinear(g);
        double sb = HlgSignalToSceneLinear(b);
        double dr = 0.0, dg = 0.0, db = 0.0;
        HlgOotfForward(sr, sg, sb, luma, &dr, &dg, &db);
        double pr = Bt2390EetfPqWithParams(PqNitsToSignal(dr * kSdrWhiteNits), params);
        double pg = Bt2390EetfPqWithParams(PqNitsToSignal(dg * kSdrWhiteNits), params);
        double pb = Bt2390EetfPqWithParams(PqNitsToSignal(db * kSdrWhiteNits), params);
        linR = PqSignalToNits(pr) / kSdrWhiteNits;
        linG = PqSignalToNits(pg) / kSdrWhiteNits;
        linB = PqSignalToNits(pb) / kSdrWhiteNits;
    } else {
        // Not an HDR transfer: pass the signal through as linear (the
        // caller gates on IsHdr, so this is only a safety net).
        linR = Clamp01(r);
        linG = Clamp01(g);
        linB = Clamp01(b);
    }
    // Gamut: source primaries -> sRGB.
    const double (*inv)[3] = GamutInverse(primaries);
    double srgbR = linR, srgbG = linG, srgbB = linB;
    if (inv != NULL) {
        srgbR = inv[0][0] * linR + inv[0][1] * linG + inv[0][2] * linB;
        srgbG = inv[1][0] * linR + inv[1][1] * linG + inv[1][2] * linB;
        srgbB = inv[2][0] * linR + inv[2][1] * linG + inv[2][2] * linB;
    }
    GamutMap(&srgbR, &srgbG, &srgbB);
    outR = ToByte(SrgbOetf(srgbR));
    outG = ToByte(SrgbOetf(srgbG));
    outB = ToByte(SrgbOetf(srgbB));
}

void ConvertPixel(double r, double g, double b, Transfer transfer, Primaries primaries,
                  double peakNits, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    ConvertPixelWithParams(r, g, b, transfer, primaries, Bt2390MakeParams(peakNits), outR, outG, outB);
}

double ComputePeakLinear(const float* srcRgb, int width, int height, size_t srcStrideFloats) {
    if (srcRgb == NULL || width <= 0 || height <= 0) return 0.0;
    double peak = 0.0;
    for (int y = 0; y < height; y++) {
        const float* srcRow = srcRgb + (size_t)y * srcStrideFloats;
        for (int x = 0; x < width; x++) {
            double r = (double)srcRow[(size_t)x * 3 + 0];
            double g = (double)srcRow[(size_t)x * 3 + 1];
            double b = (double)srcRow[(size_t)x * 3 + 2];
            // Negative/NaN channels contribute 0 (ConvertLinearPixel
            // clamps them to 0); skip them here so a NaN cannot poison
            // the peak.
            if (!(r >= 0.0)) r = 0.0;
            if (!(g >= 0.0)) g = 0.0;
            if (!(b >= 0.0)) b = 0.0;
            double yLum = r * kSrgbLuma[0] + g * kSrgbLuma[1] + b * kSrgbLuma[2];
            if (yLum > peak) peak = yLum;
        }
    }
    // P2-C2: a non-finite peak (a single +Inf channel) falls back to
    // linear 1.0 (SDR white). The 0.0 return for black images is kept.
    if (!std::isfinite(peak)) return 1.0;
    return peak;
}

// Content peak in nits from a linear peak (SDR white = 1.0), clamped to
// >= SDR white (a non-positive peak means black content).
static double PeakNitsFromLinear(double peakLinear) {
    double peakNits = peakLinear * kSdrWhiteNits;
    if (!(peakNits > 0.0)) peakNits = kSdrWhiteNits;
    return peakNits;
}

// ConvertLinearPixel with precomputed EETF parameters (P2-C3 hoisting).
// Bit-identical to ConvertLinearPixel(..., peakLinear, ...) when
// params == Bt2390MakeParams(PeakNitsFromLinear(peakLinear)).
void ConvertLinearPixelWithParams(double r, double g, double b, double peakLinear,
                                  const Bt2390Params& params, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    // Negative/NaN inputs clamp to 0 (the !(v >= 0) test is true for
    // NaN as well as negatives).
    double cr = r, cg = g, cb = b;
    if (!(cr >= 0.0)) cr = 0.0;
    if (!(cg >= 0.0)) cg = 0.0;
    if (!(cb >= 0.0)) cb = 0.0;
    double linR, linG, linB;
    // P2-C3 fast path: when the content peak is at or below SDR white
    // AND every channel is in [0,1], the PQ round-trip is the identity
    // (the EETF passes the signal through unchanged), so skip it.
    // The channel test is essential: a single channel above 1.0 is
    // rolled off by the EETF, and skipping the round-trip would leave
    // it un-rolled-off - a (2,0,0) pixel in a peak<=1.0 image came out
    // (255,142,142) instead of (255,0,0) with the peak-only condition
    // (GamutMap desaturates the un-rolled-off 2.0 toward grey).
    if (peakLinear <= 1.0 && cr <= 1.0 && cg <= 1.0 && cb <= 1.0) {
        linR = cr;
        linG = cg;
        linB = cb;
    } else {
        // Linear -> nits -> PQ signal -> EETF -> nits -> display-linear
        // (SDR white = 1.0), the same PQ-domain roll-off as the PQ/HLG
        // path, so both share one highlight curve.
        double sigR = PqNitsToSignal(cr * kSdrWhiteNits);
        double sigG = PqNitsToSignal(cg * kSdrWhiteNits);
        double sigB = PqNitsToSignal(cb * kSdrWhiteNits);
        double eR = Bt2390EetfPqWithParams(sigR, params);
        double eG = Bt2390EetfPqWithParams(sigG, params);
        double eB = Bt2390EetfPqWithParams(sigB, params);
        linR = PqSignalToNits(eR) / kSdrWhiteNits;
        linG = PqSignalToNits(eG) / kSdrWhiteNits;
        linB = PqSignalToNits(eB) / kSdrWhiteNits;
    }
    // Gamut is identity (sRGB primaries); GamutMap still runs as a
    // safety clamp for out-of-range values.
    GamutMap(&linR, &linG, &linB);
    outR = ToByte(SrgbOetf(linR));
    outG = ToByte(SrgbOetf(linG));
    outB = ToByte(SrgbOetf(linB));
}

void ConvertLinearPixel(double r, double g, double b, double peakLinear,
                        uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    // Linear 1.0 is SDR white. The content peak in nits is peakLinear *
    // kSdrWhiteNits; Bt2390EetfPq is the identity when that peak is at
    // or below SDR white, so SDR-range content passes through the sRGB
    // OETF unchanged.
    ConvertLinearPixelWithParams(r, g, b, peakLinear,
                                 Bt2390MakeParams(PeakNitsFromLinear(peakLinear)),
                                 outR, outG, outB);
}

void ConvertLinearImage(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                        uint8_t* dstBgr, size_t dstStrideBytes) {
    // P2-C5: reject strides narrower than a row (ComputePeakLinear reads
    // by stride, so this must come before the peak call below). All
    // in-tree callers pass exactly width*3, so the guard never triggers
    // today - pure hardening.
    if (srcRgb == NULL || dstBgr == NULL || width <= 0 || height <= 0 ||
        srcStrideFloats < (size_t)width * 3 || dstStrideBytes < (size_t)width * 3) return;
    double peakLinear = ComputePeakLinear(srcRgb, width, height, srcStrideFloats);
    // P2-C3: precompute the EETF parameters once per image (the
    // per-pixel ConvertLinearPixel call recomputed them every pixel).
    Bt2390Params params = Bt2390MakeParams(PeakNitsFromLinear(peakLinear));
    for (int y = 0; y < height; y++) {
        const float* srcRow = srcRgb + (size_t)y * srcStrideFloats;
        uint8_t* dstRow = dstBgr + (size_t)y * dstStrideBytes;
        for (int x = 0; x < width; x++) {
            double r = (double)srcRow[(size_t)x * 3 + 0];
            double g = (double)srcRow[(size_t)x * 3 + 1];
            double b = (double)srcRow[(size_t)x * 3 + 2];
            uint8_t outR = 0, outG = 0, outB = 0;
            ConvertLinearPixelWithParams(r, g, b, peakLinear, params, outR, outG, outB);
            // JPEGView pixels are BGR.
            dstRow[(size_t)x * 3 + 0] = outB;
            dstRow[(size_t)x * 3 + 1] = outG;
            dstRow[(size_t)x * 3 + 2] = outR;
        }
    }
}

void ConvertLinearImageToBgra(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                              uint8_t* dstBgra, size_t dstStrideBytes,
                              const uint8_t* planarAlphaOrNull, size_t alphaStride) {
    // P2-C5: same stride guard as ConvertLinearImage (dst is BGRA, so
    // width*4 here).
    if (srcRgb == NULL || dstBgra == NULL || width <= 0 || height <= 0 ||
        srcStrideFloats < (size_t)width * 3 || dstStrideBytes < (size_t)width * 4) return;
    double peakLinear = ComputePeakLinear(srcRgb, width, height, srcStrideFloats);
    // P2-C3: precompute the EETF parameters once per image (see above).
    Bt2390Params params = Bt2390MakeParams(PeakNitsFromLinear(peakLinear));
    for (int y = 0; y < height; y++) {
        const float* srcRow = srcRgb + (size_t)y * srcStrideFloats;
        uint8_t* dstRow = dstBgra + (size_t)y * dstStrideBytes;
        const uint8_t* alphaRow = (planarAlphaOrNull != NULL)
            ? planarAlphaOrNull + (size_t)y * (size_t)width * alphaStride : NULL;
        for (int x = 0; x < width; x++) {
            double r = (double)srcRow[(size_t)x * 3 + 0];
            double g = (double)srcRow[(size_t)x * 3 + 1];
            double b = (double)srcRow[(size_t)x * 3 + 2];
            uint8_t outR = 0, outG = 0, outB = 0;
            ConvertLinearPixelWithParams(r, g, b, peakLinear, params, outR, outG, outB);
            // JPEGView pixels are BGR, with alpha last. Every
            // byte of the quad is written (H-01/N-3).
            dstRow[(size_t)x * 4 + 0] = outB;
            dstRow[(size_t)x * 4 + 1] = outG;
            dstRow[(size_t)x * 4 + 2] = outR;
            dstRow[(size_t)x * 4 + 3] = (alphaRow != NULL)
                ? alphaRow[(size_t)x * alphaStride] : (uint8_t)0xff;
        }
    }
}

}  // namespace HdrToneMap
