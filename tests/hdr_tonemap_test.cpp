// hdr_tonemap_test.cpp - unit test for the shared HDR tone-mapping helper
// (HdrToneMap.h/.cpp, Option A of the HDR evaluation plan).
//
// What this pins, and what it does not:
// --------------------------------------
// The helper's transfer functions are exact standard formulas (ST 2084 PQ,
// ARIB STD-B67 HLG, BT.2390 EETF, sRGB OETF, the BT.2020/P3 <-> sRGB gamut
// matrices). A wrong constant (e.g. PQ m2 = 2523/32*128 instead of 2523/32)
// is internally consistent - the code compiles and runs, and the picture
// comes out uniformly wrong - so it cannot be caught by reading the code.
// These tests pin the CONSTANTS against values computed independently in
// Python from the standard formulas (see the evaluation plan), and pin the
// PIPELINE against the kennylevinsen test chart: SDR white encoded as PQ
// in BT.2020 must come out as sRGB white, and an 18% grey patch must come
// out at sRGB 118.
//
// This test is pure math: it needs no image files and no Windows
// toolchain. The end-to-end check against the actual test images
// (tests/samples/test_pq.png etc. vs test_reference.jpg) is the manual
// step in the evaluation plan (mean abs diff 0.03, max 1) - run it with
// the hdr_test harness in the task scratch dir, or on the device with
// the real JPEGView build.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/hdr_tonemap_test.cpp src/JPEGView/HdrToneMap.cpp
//       -o /tmp/hdr_tonemap_test
//   /tmp/hdr_tonemap_test

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#include "HdrToneMap.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

bool Near(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

void TestPqConstants() {
    // PQ signal for known absolute luminances, computed independently in
    // Python from the ST 2084 OETF (peak 10000 nits). These pin m1/m2/c1/
    // c2/c3: a wrong m2 (e.g. 2523/32*128) makes 100 nits come out as
    // ~0.97 instead of 0.508.
    Check(Near(HdrToneMap::PqNitsToSignal(0.0), 0.0, 1e-9), "PQ 0 nits -> 0");
    Check(Near(HdrToneMap::PqNitsToSignal(100.0), 0.508078, 1e-6), "PQ 100 nits -> 0.508078");
    Check(Near(HdrToneMap::PqNitsToSignal(203.0), 0.580689, 1e-6), "PQ 203 nits (SDR white) -> 0.580689");
    Check(Near(HdrToneMap::PqNitsToSignal(1000.0), 0.751827, 1e-6), "PQ 1000 nits -> 0.751827");
    Check(Near(HdrToneMap::PqNitsToSignal(10000.0), 1.0, 1e-9), "PQ 10000 nits -> 1");
    // P2-C1: Inf and huge values saturate to 1.0; NaN maps to 0
    // (pre-existing behavior, asserted here, not as a new-guard case).
    Check(Near(HdrToneMap::PqNitsToSignal(HUGE_VAL), 1.0, 1e-12), "PQ Inf -> 1");
    Check(Near(HdrToneMap::PqNitsToSignal(1e308), 1.0, 1e-12), "PQ 1e308 -> 1");
    Check(Near(HdrToneMap::PqNitsToSignal(std::nan("")), 0.0, 1e-12), "PQ NaN -> 0 (pre-existing)");
    // Round-trip: signal -> nits -> signal must be identity.
    for (double s = 0.0; s <= 1.0; s += 0.05) {
        double n = HdrToneMap::PqSignalToNits(s);
        double s2 = HdrToneMap::PqNitsToSignal(n);
        if (!Near(s, s2, 1e-9)) {
            Check(false, "PQ signal->nits->signal round-trip");
            break;
        }
    }
    g_checks++;  // the loop above passed as one check
    // EOTF monotonic: more signal means more nits.
    Check(HdrToneMap::PqSignalToNits(0.75) > HdrToneMap::PqSignalToNits(0.5),
          "PQ EOTF monotonic");
}

void TestHlg() {
    // HLG OETF inverse is the exact inverse of the OETF: feeding the
    // OETF output back must return the scene-linear value.
    for (double v = 0.0; v <= 1.0; v += 0.1) {
        // Forward OETF (ARIB STD-B67), replicated here only to feed the
        // inverse; the inverse itself is what is pinned.
        double e;
        if (v <= 1.0 / 12.0) {
            e = std::sqrt(3.0 * v);
        } else {
            e = 0.17883277 * std::log(12.0 * v - 0.28466892) + 0.55991073;
        }
        double back = HdrToneMap::HlgSignalToSceneLinear(e);
        if (!Near(v, back, 1e-9)) {
            Check(false, "HLG OETF inverse round-trip");
            break;
        }
    }
    g_checks++;
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(0.0), 0.0, 1e-12), "HLG 0 -> 0");
    // 2.4e-8: the log/exp round-trip is not exact in double precision.
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(1.0), 1.0, 1e-6), "HLG 1 -> 1");
}

void TestSrgbOetf() {
    // sRGB OETF (IEC 61966-2-1) at known points, computed in Python.
    Check(Near(HdrToneMap::SrgbOetf(0.0), 0.0, 1e-12), "sRGB 0 -> 0");
    Check(Near(HdrToneMap::SrgbOetf(1.0), 1.0, 1e-12), "sRGB 1 -> 1");
    Check(Near(HdrToneMap::SrgbOetf(0.18), 0.461356, 1e-6), "sRGB 0.18 -> 0.461356");
    Check(Near(HdrToneMap::SrgbOetf(0.5), 0.735357, 1e-6), "sRGB 0.5 -> 0.735357");
}

void TestEetfIdentityAtSdrPeak() {
    // BT.2390 EETF, content-adaptive: when the content peak is at or
    // below SDR white (203 nits) the knee is at maxLum = 1.0, so the
    // EETF must be the identity - the test chart's white must not be
    // compressed. (A literal LW=1000 EETF would give 203 nits -> 0.78
    // instead of 1.0; that is why the adaptive form is used.)
    // Only signals in the valid range for a 203-nit peak are tested:
    // a PQ signal above PqNitsToSignal(203) would be > 203 nits, which
    // contradicts the peak, and the EETF clamps it to SDR white.
    double pqSdrWhite = HdrToneMap::PqNitsToSignal(203.0);
    for (double s = 0.0; s <= pqSdrWhite; s += 0.01) {
        double out = HdrToneMap::Bt2390EetfPq(s, 203.0);
        if (!Near(s, out, 1e-9)) {
            Check(false, "EETF identity when peak <= SDR white");
            break;
        }
    }
    g_checks++;
    // With a real HDR peak (1000 nits) the EETF must compress the
    // highlights but stay monotonic and never exceed 1.0.
    double prev = -1.0;
    bool ok = true;
    for (double s = 0.0; s <= 1.0; s += 0.01) {
        double out = HdrToneMap::Bt2390EetfPq(s, 1000.0);
        if (!(out >= prev - 1e-12) || !(out <= 1.0 + 1e-12)) { ok = false; break; }
        prev = out;
    }
    Check(ok, "EETF monotonic and <= 1 at 1000-nit peak");
    // The content peak (1000 nits) maps to the display peak (203 nits,
    // SDR white): that is what tone mapping does. It must NOT pass
    // through unchanged (that would be no tone mapping at all).
    double e = HdrToneMap::Bt2390EetfPq(HdrToneMap::PqNitsToSignal(1000.0), 1000.0);
    Check(Near(HdrToneMap::PqSignalToNits(e), 203.0, 1.0), "EETF maps 1000-nit peak to SDR white");
    // 203 nits under a 1000-nit peak must be compressed below 1.0
    // (it is no longer the peak) but stay well above the SDR level.
    double e203 = HdrToneMap::Bt2390EetfPq(HdrToneMap::PqNitsToSignal(203.0), 1000.0);
    double n203 = HdrToneMap::PqSignalToNits(e203) / 203.0;
    Check(n203 < 1.0 && n203 > 0.5, "EETF compresses 203 nits under 1000-nit peak");
}

void TestConvertPixel() {
    using namespace HdrToneMap;
    // SDR white in BT.2020, encoded as PQ, must come out as sRGB white.
    // (The kennylevinsen chart's white patch is exactly this.)
    double sigWhite = PqNitsToSignal(203.0);
    uint8_t outR = 0, outG = 0, outB = 0;
    ConvertPixel(sigWhite, sigWhite, sigWhite, Transfer::kPq, Primaries::kBt2020, 203.0, outR, outG, outB);
    Check(outR == 255 && outG == 255 && outB == 255,
          "PQ BT.2020 SDR white -> sRGB 255,255,255");
    // Black stays black.
    ConvertPixel(0.0, 0.0, 0.0, Transfer::kPq, Primaries::kBt2020, 203.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "PQ black -> 0,0,0");
    // 18% grey in BT.2020 PQ (signal ~0.4109) must come out at sRGB
    // 118 (0.461356 * 255 = 117.6, rounds to 118).
    ConvertPixel(0.410904, 0.410896, 0.410877, Transfer::kPq, Primaries::kBt2020, 203.0, outR, outG, outB);
    Check(outR >= 116 && outR <= 120 &&
          outG >= 116 && outG <= 120 &&
          outB >= 116 && outB <= 120,
          "PQ BT.2020 18% grey -> sRGB ~118");
    // HLG SDR white (BT.2020) -> sRGB white. The white patch's HLG
    // signal is 0.75 (the max HLG signal in the test images).
    ConvertPixel(0.75, 0.75, 0.75, Transfer::kHlg, Primaries::kBt2020, 203.0, outR, outG, outB);
    Check(outR >= 250 && outG >= 250 && outB >= 250,
          "HLG BT.2020 white -> sRGB ~255");
    // A saturated BT.2020 red (linear 1,0,0 in BT.2020) must come out
    // as a saturated sRGB red, not clipped to white or shifted in hue.
    // BT.2020 red -> sRGB is out of gamut; the helper desaturates
    // toward white, so the result must be reddish (R high, G/B low).
    // Encode BT.2020 linear red (1,0,0) as PQ at 203 nits: the PQ
    // signal is per-channel, so R = PQ(203), G = B = PQ(0) = 0.
    ConvertPixel(sigWhite, 0.0, 0.0, Transfer::kPq, Primaries::kBt2020, 203.0, outR, outG, outB);
    Check(outR > 200 && outG < 120 && outB < 120,
          "PQ BT.2020 red stays red after gamut mapping");
}

void TestDetection() {
    using namespace HdrToneMap;
    // IsHdr takes a Cicp; build one per transfer code.
    auto cicpFor = [](int transfer) {
        Cicp c;
        c.transfer = transfer;
        return c;
    };
    Check(IsHdr(cicpFor(16)), "PQ is HDR");
    Check(IsHdr(cicpFor(18)), "HLG is HDR");
    Check(!IsHdr(cicpFor(13)), "sRGB (13) is not HDR");
    Check(!IsHdr(cicpFor(0)), "unspecified is not HDR");
    Check(TransferFromCode(16) == Transfer::kPq, "CICP 16 -> PQ");
    Check(TransferFromCode(18) == Transfer::kHlg, "CICP 18 -> HLG");
    Check(TransferFromCode(13) == Transfer::kSrgb, "CICP 13 -> sRGB");
    Check(PrimariesFromCode(9) == Primaries::kBt2020, "CICP 9 -> BT.2020");
    Check(PrimariesFromCode(12) == Primaries::kP3, "CICP 12 -> P3");
    Check(PrimariesFromCode(1) == Primaries::kBt709, "CICP 1 -> BT.709");
    // IsHdr is the ONLY gate for the new code path: an SDR transfer
    // must never reach the tone mapper.
    Check(!IsHdr(cicpFor(13)), "transfer 13 not HDR (gate)");
    Check(!IsHdr(cicpFor(0)), "transfer 0 not HDR (gate)");
}

void TestComputePeakNits() {
    using namespace HdrToneMap;
    // A 2x1 image: one SDR-white PQ pixel, one black. Peak must be 203.
    float img[6] = {
        (float)PqNitsToSignal(203.0), (float)PqNitsToSignal(203.0), (float)PqNitsToSignal(203.0),
        0.0f, 0.0f, 0.0f,
    };
    double peak = ComputePeakNits(img, 2, Transfer::kPq, Primaries::kBt2020);
    Check(Near(peak, 203.0, 0.5), "ComputePeakNits finds 203 for SDR-white chart");
    // All-black image: peak falls back to SDR white (never 0, which
    // would make the EETF divide by zero).
    float black[3] = { 0.0f, 0.0f, 0.0f };
    Check(Near(ComputePeakNits(black, 1, Transfer::kPq, Primaries::kBt2020), 203.0, 1e-9),
          "ComputePeakNits black image falls back to 203");
}

void TestConvertLinearPixel() {
    using namespace HdrToneMap;
    uint8_t outR = 0, outG = 0, outB = 0;
    // Linear 1.0 is SDR white: with a peak of 1.0 it must come out 255.
    ConvertLinearPixel(1.0, 1.0, 1.0, 1.0, outR, outG, outB);
    Check(outR == 255 && outG == 255 && outB == 255,
          "linear 1.0 (peak 1.0) -> sRGB 255");
    // Black stays black.
    ConvertLinearPixel(0.0, 0.0, 0.0, 1.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "linear black -> 0");
    // 18% grey, peak 1.0: sRGB OETF(0.18) = 0.4614, * 255 = 117.6 -> 118.
    // (The old pow(v, 1/2.2) path gave 117; the sRGB OETF is the
    // standards-correct curve, so a +/-2 band is used here.)
    ConvertLinearPixel(0.18, 0.18, 0.18, 1.0, outR, outG, outB);
    Check(outR >= 116 && outR <= 120 &&
          outG >= 116 && outG <= 120 &&
          outB >= 116 && outB <= 120,
          "linear 18% grey (peak 1.0) -> sRGB ~118");
    // Mid-tone, peak 1.0: sRGB OETF(0.5) = 0.7354, * 255 = 187.5 -> 188.
    ConvertLinearPixel(0.5, 0.5, 0.5, 1.0, outR, outG, outB);
    Check(outR >= 186 && outR <= 190,
          "linear 0.5 (peak 1.0) -> sRGB ~188");
    // HDR roll-off: with a peak of 16.0, linear 1.0 must NOT clip to
    // 255 (the old pow() path gave 255) - it rolls off to ~211.
    ConvertLinearPixel(1.0, 1.0, 1.0, 16.0, outR, outG, outB);
    Check(outR > 180 && outR < 255,
          "linear 1.0 (peak 16) rolls off, does not clip");
    // Linear 4.0 with a peak of 16.0: ~249, still below white.
    ConvertLinearPixel(4.0, 4.0, 4.0, 16.0, outR, outG, outB);
    Check(outR > 230 && outR < 255,
          "linear 4.0 (peak 16) rolls off, does not clip");
    // The peak itself maps to white.
    ConvertLinearPixel(16.0, 16.0, 16.0, 16.0, outR, outG, outB);
    Check(outR == 255 && outG == 255 && outB == 255,
          "linear peak (16) -> sRGB 255");
    // Negative and NaN inputs map to 0 (the !(v >= 0) guard).
    ConvertLinearPixel(-1.0, -1.0, -1.0, 1.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "linear negative -> 0");
    double nan = std::nan("");
    ConvertLinearPixel(nan, nan, nan, 1.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "linear NaN -> 0");
}

void TestComputePeakLinear() {
    using namespace HdrToneMap;
    // A 2x1 image: one 0.5-grey pixel, one black. Peak luminance is 0.5.
    float img[6] = { 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f };
    double peak = ComputePeakLinear(img, 2, 1, 6);
    Check(Near(peak, 0.5, 1e-9), "ComputePeakLinear finds 0.5 for grey chart");
    // All-black image: peak is 0 (the caller maps it to SDR white).
    float black[3] = { 0.0f, 0.0f, 0.0f };
    Check(Near(ComputePeakLinear(black, 1, 1, 3), 0.0, 1e-12),
          "ComputePeakLinear black image -> 0");
    // A NaN channel must not poison the peak.
    float withNan[6] = { 0.25f, 0.25f, 0.25f, (float)std::nan(""), 0.0f, 0.0f };
    Check(Near(ComputePeakLinear(withNan, 2, 1, 6), 0.25, 1e-9),
          "ComputePeakLinear ignores NaN channel");
}

void TestConvertLinearImage() {
    using namespace HdrToneMap;
    // A 2x1 image: white (linear 1.0) and 18% grey. Peak is 1.0, so the
    // output must be sRGB white and ~118. Output is BGR (JPEGView order).
    float img[6] = { 1.0f, 1.0f, 1.0f, 0.18f, 0.18f, 0.18f };
    uint8_t out[6] = { 0, 0, 0, 0, 0, 0 };
    ConvertLinearImage(img, 2, 1, 6, out, 6);
    Check(out[0] == 255 && out[1] == 255 && out[2] == 255,
          "ConvertLinearImage white pixel -> BGR 255,255,255");
    Check(out[3] >= 116 && out[3] <= 120 &&
          out[4] >= 116 && out[4] <= 120 &&
          out[5] >= 116 && out[5] <= 120,
          "ConvertLinearImage grey pixel -> BGR ~118");
}

void TestConvertLinearImageToBgra() {
    using namespace HdrToneMap;
    // A 2x1 image: white (linear 1.0) and 18% grey, with a planar
    // alpha of 128 and 255. Peak is 1.0, so the colour bytes must
    // match ConvertLinearImage (white 255, grey ~118) and the alpha
    // bytes must come from the plane. Output is BGRA (JPEGView order).
    // The output is pre-filled with a sentinel so a missed byte
    // (H-01/N-3: the old packed-BGR path left bytes uninitialized
    // for 4-channel PSDs) fails the checks instead of passing silently.
    float img[6] = { 1.0f, 1.0f, 1.0f, 0.18f, 0.18f, 0.18f };
    uint8_t alpha[2] = { 128, 255 };
    uint8_t out[8];
    std::memset(out, 0xCD, sizeof(out));
    ConvertLinearImageToBgra(img, 2, 1, 6, out, 8, alpha, 1);
    Check(out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 128,
          "ConvertLinearImageToBgra white pixel -> BGRA 255,255,255,128");
    Check(out[4] >= 116 && out[4] <= 120 &&
          out[5] >= 116 && out[5] <= 120 &&
          out[6] >= 116 && out[6] <= 120 && out[7] == 255,
          "ConvertLinearImageToBgra grey pixel -> BGRA ~118,255");
    // NULL alpha: every alpha byte is 0xff (opaque) and the colour
    // bytes are unchanged; the sentinel must be gone from all bytes.
    std::memset(out, 0xCD, sizeof(out));
    ConvertLinearImageToBgra(img, 2, 1, 6, out, 8, NULL, 0);
    Check(out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 0xff,
          "ConvertLinearImageToBgra NULL alpha -> white pixel alpha 0xff");
    Check(out[4] >= 116 && out[4] <= 120 &&
          out[5] >= 116 && out[5] <= 120 &&
          out[6] >= 116 && out[6] <= 120 && out[7] == 0xff,
          "ConvertLinearImageToBgra NULL alpha -> grey pixel alpha 0xff");
}

}  // namespace

int main() {
    TestPqConstants();
    TestHlg();
    TestSrgbOetf();
    TestEetfIdentityAtSdrPeak();
    TestConvertPixel();
    TestDetection();
    TestComputePeakNits();
    TestConvertLinearPixel();
    TestComputePeakLinear();
    TestConvertLinearImage();
    TestConvertLinearImageToBgra();
    std::printf("hdr_tonemap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
