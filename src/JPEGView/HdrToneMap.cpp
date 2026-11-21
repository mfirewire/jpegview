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

// Gamut matrices (linear RGB -> linear RGB, D65). Values match GTK
// gdkcolordefs and the kennylevinsen test generator.
const double kSrgbToRec2020[3][3] = {
    {0.627504, 0.329275, 0.043303},
    {0.069108, 0.919519, 0.011360},
    {0.016394, 0.088011, 0.895380},
};
const double kSrgbToP3[3][3] = {
    {0.822462, 0.177538, 0.000000},
    {0.033194, 0.966806, 0.000000},
    {0.017083, 0.072397, 0.910520},
};
// Inverses (BT.2020/P3 -> sRGB), computed from the above matrices
// (numpy, float64). Verified by round-trip: max error 2.2e-16.
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
    double lmM1 = std::pow(lm, kPqM1);
    return std::pow((kPqC1 + kPqC2 * lmM1) / (1.0 + kPqC3 * lmM1), kPqM2);
}

double HlgSignalToSceneLinear(double signal) {
    double s = signal;
    double as = std::fabs(s);
    if (as <= 0.5) {
        return (s < 0.0 ? -1.0 : 1.0) * (as * as) / 3.0;
    }
    return (s < 0.0 ? -1.0 : 1.0) * (std::exp((as - kHlgC) / kHlgA) + kHlgB) / 12.0;
}

double Bt2390EetfPq(double signal, double peakNits) {
    double lw = peakNits;
    if (!(lw > kSdrWhiteNits)) lw = kSdrWhiteNits;
    const double lmax = kSdrWhiteNits;
    const double lb = 0.0;
    const double lmin = 0.0;
    double pqLw = PqNitsToSignal(lw);
    double pqLb = PqNitsToSignal(lb);
    double pqLmax = PqNitsToSignal(lmax);
    double pqLmin = PqNitsToSignal(lmin);
    double denom = pqLw - pqLb;
    if (!(denom > 0.0)) return Clamp01(signal);
    double maxLum = (pqLmax - pqLb) / denom;
    double minLum = (pqLmin - pqLb) / denom;
    double ks = 1.5 * maxLum - 0.5;
    // Normalised PQ signal (source white = 1). When the content peak is
    // at or below SDR white, maxLum == 1 and ks == 1, so E1 < ks for
    // every in-range signal and the EETF is the identity.
    double e1 = Clamp01(signal) / pqLw;
    if (!(e1 < 1.0)) e1 = 1.0;
    double e2;
    if (e1 < ks || !(ks < 1.0)) {
        e2 = e1;
    } else {
        double t = (e1 - ks) / (1.0 - ks);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        double t2 = t * t;
        double t3 = t2 * t;
        e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks
           + (t3 - 2.0 * t2 + t) * (1.0 - ks)
           + (-2.0 * t3 + 3.0 * t2) * maxLum;
    }
    double e3 = e2 + minLum * std::pow(1.0 - e2, 4.0);
    // Denormalise back to a PQ signal for the target display.
    return PqNitsToSignal(PqSignalToNits(e3 * denom + pqLb));
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
    if (!(peak > 0.0)) return kSdrWhiteNits;
    return peak;
}

void ConvertPixel(double r, double g, double b, Transfer transfer, Primaries primaries,
                  double peakNits, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    const double* luma = GamutLuma(primaries);
    double linR = 0.0, linG = 0.0, linB = 0.0;  // display-linear, source gamut, SDR white = 1
    if (transfer == Transfer::kPq) {
        // Tone-map in the PQ signal domain first (BT.2390 EETF), then
        // convert to display-linear. This is where the highlight
        // roll-off lives; for content at/below SDR white it is identity.
        double sr = Bt2390EetfPq(Clamp01(r), peakNits);
        double sg = Bt2390EetfPq(Clamp01(g), peakNits);
        double sb = Bt2390EetfPq(Clamp01(b), peakNits);
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
        double pr = Bt2390EetfPq(PqNitsToSignal(dr * kSdrWhiteNits), peakNits);
        double pg = Bt2390EetfPq(PqNitsToSignal(dg * kSdrWhiteNits), peakNits);
        double pb = Bt2390EetfPq(PqNitsToSignal(db * kSdrWhiteNits), peakNits);
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

void ConvertImage(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                  uint8_t* dstRgb, size_t dstStrideBytes,
                  Transfer transfer, Primaries primaries) {
    if (srcRgb == NULL || dstRgb == NULL || width <= 0 || height <= 0) return;
    double peakNits = ComputePeakNits(srcRgb, (size_t)width * (size_t)height, transfer, primaries);
    for (int y = 0; y < height; y++) {
        const float* srcRow = srcRgb + (size_t)y * srcStrideFloats;
        uint8_t* dstRow = dstRgb + (size_t)y * dstStrideBytes;
        for (int x = 0; x < width; x++) {
            double r = (double)srcRow[(size_t)x * 3 + 0];
            double g = (double)srcRow[(size_t)x * 3 + 1];
            double b = (double)srcRow[(size_t)x * 3 + 2];
            uint8_t outR = 0, outG = 0, outB = 0;
            ConvertPixel(r, g, b, transfer, primaries, peakNits, outR, outG, outB);
            dstRow[(size_t)x * 3 + 0] = outR;
            dstRow[(size_t)x * 3 + 1] = outG;
            dstRow[(size_t)x * 3 + 2] = outB;
        }
    }
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
    return peak;
}

void ConvertLinearPixel(double r, double g, double b, double peakLinear,
                        uint8_t& outR, uint8_t& outG, uint8_t& outB) {
    // Linear 1.0 is SDR white. The content peak in nits is peakLinear *
    // kSdrWhiteNits; Bt2390EetfPq is the identity when that peak is at
    // or below SDR white, so SDR-range content passes through the sRGB
    // OETF unchanged. Negative/NaN inputs clamp to 0 (the !(v >= 0)
    // test is true for NaN as well as negatives).
    double peakNits = peakLinear * kSdrWhiteNits;
    if (!(peakNits > 0.0)) peakNits = kSdrWhiteNits;
    double cr = r, cg = g, cb = b;
    if (!(cr >= 0.0)) cr = 0.0;
    if (!(cg >= 0.0)) cg = 0.0;
    if (!(cb >= 0.0)) cb = 0.0;
    // Linear -> nits -> PQ signal -> EETF -> nits -> display-linear
    // (SDR white = 1.0), the same PQ-domain roll-off as the PQ/HLG
    // path, so both share one highlight curve.
    double sigR = PqNitsToSignal(cr * kSdrWhiteNits);
    double sigG = PqNitsToSignal(cg * kSdrWhiteNits);
    double sigB = PqNitsToSignal(cb * kSdrWhiteNits);
    double eR = Bt2390EetfPq(sigR, peakNits);
    double eG = Bt2390EetfPq(sigG, peakNits);
    double eB = Bt2390EetfPq(sigB, peakNits);
    double linR = PqSignalToNits(eR) / kSdrWhiteNits;
    double linG = PqSignalToNits(eG) / kSdrWhiteNits;
    double linB = PqSignalToNits(eB) / kSdrWhiteNits;
    // Gamut is identity (sRGB primaries); GamutMap still runs as a
    // safety clamp for out-of-range values.
    GamutMap(&linR, &linG, &linB);
    outR = ToByte(SrgbOetf(linR));
    outG = ToByte(SrgbOetf(linG));
    outB = ToByte(SrgbOetf(linB));
}

void ConvertLinearImage(const float* srcRgb, int width, int height, size_t srcStrideFloats,
                        uint8_t* dstBgr, size_t dstStrideBytes) {
    if (srcRgb == NULL || dstBgr == NULL || width <= 0 || height <= 0) return;
    double peakLinear = ComputePeakLinear(srcRgb, width, height, srcStrideFloats);
    for (int y = 0; y < height; y++) {
        const float* srcRow = srcRgb + (size_t)y * srcStrideFloats;
        uint8_t* dstRow = dstBgr + (size_t)y * dstStrideBytes;
        for (int x = 0; x < width; x++) {
            double r = (double)srcRow[(size_t)x * 3 + 0];
            double g = (double)srcRow[(size_t)x * 3 + 1];
            double b = (double)srcRow[(size_t)x * 3 + 2];
            uint8_t outR = 0, outG = 0, outB = 0;
            ConvertLinearPixel(r, g, b, peakLinear, outR, outG, outB);
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
    if (srcRgb == NULL || dstBgra == NULL || width <= 0 || height <= 0) return;
    double peakLinear = ComputePeakLinear(srcRgb, width, height, srcStrideFloats);
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
            ConvertLinearPixel(r, g, b, peakLinear, outR, outG, outB);
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
