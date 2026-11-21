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
    // P3-C5: single Check(ok) after the loop (the old pattern -
    // Check(false) inside + unconditional g_checks++ after -
    // counted one failed loop as two checks).
    bool roundTripOk = true;
    for (double s = 0.0; s <= 1.0; s += 0.05) {
        double n = HdrToneMap::PqSignalToNits(s);
        double s2 = HdrToneMap::PqNitsToSignal(n);
        if (!Near(s, s2, 1e-9)) {
            roundTripOk = false;
            break;
        }
    }
    Check(roundTripOk, "PQ signal->nits->signal round-trip");
    // EOTF monotonic: more signal means more nits.
    Check(HdrToneMap::PqSignalToNits(0.75) > HdrToneMap::PqSignalToNits(0.5),
          "PQ EOTF monotonic");
}

void TestHlg() {
    // HLG OETF inverse is the exact inverse of the OETF: feeding the
    // OETF output back must return the scene-linear value.
    // P3-C5: single Check(ok) after the loop (see TestPqConstants).
    bool hlgRoundTripOk = true;
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
            hlgRoundTripOk = false;
            break;
        }
    }
    Check(hlgRoundTripOk, "HLG OETF inverse round-trip");
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(0.0), 0.0, 1e-12), "HLG 0 -> 0");
    // 2.4e-8: the log/exp round-trip is not exact in double precision.
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(1.0), 1.0, 1e-6), "HLG 1 -> 1");
    // P2-C10: the signal is clamped to [0,1] - assert the clamped VALUE
    // (~1.0), not just finiteness. Before the clamp Hlg(2) was 261.88
    // and Hlg(128) was Inf; Hlg(-0.5) was -0.0833 and Hlg(NaN) was NaN.
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(2.0), 1.0, 1e-6),
          "HLG 2.0 clamps to ~1.0");
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(128.0), 1.0, 1e-6),
          "HLG 128 clamps to ~1.0");
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(-0.5), 0.0, 1e-12),
          "HLG -0.5 clamps to 0");
    Check(Near(HdrToneMap::HlgSignalToSceneLinear(std::nan("")), 0.0, 1e-12),
          "HLG NaN clamps to 0");
}

void TestSrgbOetf() {
    // sRGB OETF (IEC 61966-2-1) at known points, computed in Python.
    Check(Near(HdrToneMap::SrgbOetf(0.0), 0.0, 1e-12), "sRGB 0 -> 0");
    Check(Near(HdrToneMap::SrgbOetf(1.0), 1.0, 1e-12), "sRGB 1 -> 1");
    Check(Near(HdrToneMap::SrgbOetf(0.18), 0.461356, 1e-6), "sRGB 0.18 -> 0.461356");
    Check(Near(HdrToneMap::SrgbOetf(0.5), 0.735357, 1e-6), "sRGB 0.5 -> 0.735357");
    // P3-C5: the sRGB OETF join point (IEC 61966-2-1). The linear
    // segment (12.92*v) meets the power segment at v = 0.0031308,
    // where both give 0.040449936 (12.92 * 0.0031308). The -9
    // deep-shadow deviation in the .hdr/EXR path hangs on this
    // junction, so pin the value, the continuity, and the byte.
    Check(Near(HdrToneMap::SrgbOetf(0.0031308), 0.040449936, 1e-9),
          "sRGB join point 0.0031308 -> 0.040449936");
    Check(Near(HdrToneMap::SrgbOetf(0.0031308 - 1e-9),
               HdrToneMap::SrgbOetf(0.0031308 + 1e-9), 1e-6),
          "sRGB OETF continuous across the join point");
    // Out-of-range inputs: negative clamps to 0; above 1.0 the power
    // branch keeps going (1.353... at 2.0) - the byte clamp happens
    // downstream in ToByte/GamutMap, not here.
    Check(Near(HdrToneMap::SrgbOetf(-1.0), 0.0, 1e-12), "sRGB OETF -1 -> 0");
    Check(Near(HdrToneMap::SrgbOetf(2.0), 1.353256046, 1e-9),
          "sRGB OETF 2.0 -> 1.353256046");
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
    // P3-C5: single Check(ok) after the loop (see TestPqConstants).
    bool identityOk = true;
    for (double s = 0.0; s <= pqSdrWhite; s += 0.01) {
        double out = HdrToneMap::Bt2390EetfPq(s, 203.0);
        if (!Near(s, out, 1e-9)) {
            identityOk = false;
            break;
        }
    }
    Check(identityOk, "EETF identity when peak <= SDR white");
    // A signal above PQ(203) at an SDR peak is capped to PQ(203)
    // (the fast path returns min(Clamp01(signal), PQ(203))).
    double pqCap = HdrToneMap::PqNitsToSignal(203.0);
    Check(Near(HdrToneMap::Bt2390EetfPq(0.9, 203.0), pqCap, 1e-12),
          "EETF caps signal above PQ(203) at SDR peak");
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
    // P2-C10: an HLG super-white red (2.0) is clamped to 1.0 and renders
    // pure red (Bt709 has an identity gamut, so no gamut mapping).
    ConvertPixel(2.0, 0.0, 0.0, Transfer::kHlg, Primaries::kBt709, 203.0, outR, outG, outB);
    Check(outR == 255 && outG == 0 && outB == 0,
          "HLG super-white red (2,0,0) -> sRGB (255,0,0)");
    // P2-C10: a mid-grey HLG pixel next to a super-white pixel must not
    // be darkened. Before the clamp the super-white pixel raised the
    // peak to ~586k nits and the grey came out nearly black; with the
    // clamp the peak is ~734 nits and the grey stays mid-grey (~137).
    float hlgImg[6] = { 0.5f, 0.5f, 0.5f, 2.0f, 0.0f, 0.0f };
    double hlgPeak = ComputePeakNits(hlgImg, 2, Transfer::kHlg, Primaries::kBt709);
    Check(Near(hlgPeak, 733.69, 0.5), "HLG peak with super-white pixel is ~734");
    ConvertPixel(0.5, 0.5, 0.5, Transfer::kHlg, Primaries::kBt709, hlgPeak, outR, outG, outB);
    Check(outR >= 130 && outR <= 145 && outG >= 130 && outG <= 145 && outB >= 130 && outB <= 145,
          "HLG grey next to super-white stays mid-grey");
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
    Check(PrimariesFromCode(11) == Primaries::kP3Dci, "CICP 11 -> DCI-P3");
    Check(PrimariesFromCode(12) == Primaries::kP3, "CICP 12 -> P3");
    Check(PrimariesFromCode(1) == Primaries::kBt709, "CICP 1 -> BT.709");
    // IsHdr is the ONLY gate for the new code path: an SDR transfer
    // must never reach the tone mapper.
    Check(!IsHdr(cicpFor(13)), "transfer 13 not HDR (gate)");
    Check(!IsHdr(cicpFor(0)), "transfer 0 not HDR (gate)");
}

void TestDciP3Primaries() {
    using namespace HdrToneMap;
    // P2-B1/D7: CICP 11 is DCI-P3 (DCI white 0.314, 0.351), not Display
    // P3 (D65). A 50% grey PQ pixel under the DCI matrix comes out
    // green-shifted (178,192,175); the D65 matrix would give neutral
    // (188,188,188) - up to 13 levels wrong. Values pinned from the
    // real helper (probe_dci); ranges allow +-3 for float rounding.
    uint8_t outR = 0, outG = 0, outB = 0;
    double sigGrey = PqNitsToSignal(101.5);  // 50% of SDR white (203)
    ConvertPixel(sigGrey, sigGrey, sigGrey, Transfer::kPq, Primaries::kP3Dci, 203.0, outR, outG, outB);
    Check(outR >= 175 && outR <= 181 && outG >= 189 && outG <= 195 &&
          outB >= 172 && outB <= 178,
          "PQ DCI-P3 50% grey -> ~(178,192,175)");
    Check(outG > outR && outG > outB,
          "PQ DCI-P3 50% grey is green-shifted, not neutral");
    ConvertPixel(sigGrey, sigGrey, sigGrey, Transfer::kPq, Primaries::kP3, 203.0, outR, outG, outB);
    Check(outR >= 185 && outR <= 191 && outG >= 185 && outG <= 191 &&
          outB >= 185 && outB <= 191,
          "PQ Display P3 50% grey stays neutral ~(188,188,188)");
    // A half-red pixel differs by ~7 levels between the two matrices.
    ConvertPixel(sigGrey, 0.0, 0.0, Transfer::kPq, Primaries::kP3Dci, 203.0, outR, outG, outB);
    Check(outR >= 183 && outR <= 189 && outG == 0 && outB >= 20 && outB <= 26,
          "PQ DCI-P3 half red -> ~(186,0,23)");
    ConvertPixel(sigGrey, 0.0, 0.0, Transfer::kPq, Primaries::kP3, 203.0, outR, outG, outB);
    Check(outR >= 190 && outR <= 196 && outG == 0 && outB >= 22 && outB <= 28,
          "PQ Display P3 half red -> ~(193,0,25)");
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
    // P2-C2 + P2-C10: a single HLG super-white pixel (128,0,0) no
    // longer produces a non-finite peak - the C10 clamp makes
    // Hlg(128) ~1.0, so the peak is a finite ~734 nits (the OOTF
    // boosts a full-brightness red on a 1000-nit display), not the
    // 203-nit fallback. (Before C10, Hlg(128) was Inf and the C2
    // fallback returned 203; the fallback itself is still in place
    // as defense, and the linear-path Inf test still exercises it.)
    float hlgInf[3] = { 128.0f, 0.0f, 0.0f };
    Check(Near(ComputePeakNits(hlgInf, 1, Transfer::kHlg, Primaries::kBt709), 733.69, 0.5),
          "ComputePeakNits HLG super-white peak is finite ~734");
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
    // HDR roll-off, pinned (P3-C5): with a peak of 16.0 the old
    // pow() path clipped everything >= 1.0 to 255; the BT.2390
    // roll-off gives 211/235/249/254 for linear 1/2/4/8. Values
    // re-confirmed on the post-Batch-2 code (fast path + hoisting)
    // and identical to the pre-change probe: the fast path does not
    // apply above peak 1.0, and hoisting is byte-identical (see
    // TestHoistingEquivalence). The +-1 band absorbs cross-libm pow
    // rounding (the Linux probe gives the exact values; pre-rounding
    // margins to the .5 boundary are >= 0.03); a wrong constant
    // would move them by far more than 1 level.
    ConvertLinearPixel(1.0, 1.0, 1.0, 16.0, outR, outG, outB);
    Check(outR >= 210 && outR <= 212 && outG >= 210 && outG <= 212 &&
          outB >= 210 && outB <= 212,
          "linear 1.0 (peak 16) rolls off to 211");
    ConvertLinearPixel(2.0, 2.0, 2.0, 16.0, outR, outG, outB);
    Check(outR >= 234 && outR <= 236 && outG >= 234 && outG <= 236 &&
          outB >= 234 && outB <= 236,
          "linear 2.0 (peak 16) rolls off to 235");
    ConvertLinearPixel(4.0, 4.0, 4.0, 16.0, outR, outG, outB);
    Check(outR >= 248 && outR <= 250 && outG >= 248 && outG <= 250 &&
          outB >= 248 && outB <= 250,
          "linear 4.0 (peak 16) rolls off to 249");
    ConvertLinearPixel(8.0, 8.0, 8.0, 16.0, outR, outG, outB);
    Check(outR >= 253 && outR <= 254 && outG >= 253 && outG <= 254 &&
          outB >= 253 && outB <= 254,
          "linear 8.0 (peak 16) rolls off to 254");
    // Mid-tone under the same peak: 0.5 rolls off to 177 (not 188).
    ConvertLinearPixel(0.5, 0.5, 0.5, 16.0, outR, outG, outB);
    Check(outR >= 176 && outR <= 178 && outG >= 176 && outG <= 178 &&
          outB >= 176 && outB <= 178,
          "linear 0.5 (peak 16) rolls off to 177");
    // The peak itself maps to white.
    ConvertLinearPixel(16.0, 16.0, 16.0, 16.0, outR, outG, outB);
    Check(outR == 255 && outG == 255 && outB == 255,
          "linear peak (16) -> sRGB 255");
    // sRGB join point as a byte (P3-C5): linear 0.0031308 at SDR
    // peak takes the fast path (peak 1.0, channels in [0,1]) and the
    // linear OETF branch, giving ToByte(0.040449936) = 10.
    ConvertLinearPixel(0.0031308, 0.0031308, 0.0031308, 1.0, outR, outG, outB);
    Check(outR == 10 && outG == 10 && outB == 10,
          "linear 0.0031308 (peak 1.0) -> sRGB 10 (join point)");
    // Colored roll-off (P3-C5): channels roll off independently - a
    // red pixel keeps its hue (G/B stay 0) and the red channel
    // follows the same 211/235/249/254 curve as grey.
    ConvertLinearPixel(1.0, 0.0, 0.0, 16.0, outR, outG, outB);
    Check(outR >= 210 && outR <= 212 && outG == 0 && outB == 0,
          "linear red 1.0 (peak 16) rolls off to (211,0,0)");
    ConvertLinearPixel(4.0, 0.0, 0.0, 16.0, outR, outG, outB);
    Check(outR >= 248 && outR <= 250 && outG == 0 && outB == 0,
          "linear red 4.0 (peak 16) rolls off to (249,0,0)");
    ConvertLinearPixel(1.0, 0.5, 0.0, 16.0, outR, outG, outB);
    Check(outR >= 210 && outR <= 212 && outG >= 176 && outG <= 178 && outB == 0,
          "linear (1,0.5,0) (peak 16) rolls off to (211,177,0)");
    // Non-finite / degenerate peakLinear (P3-C5): NaN, negative and
    // zero peaks fall back to SDR white (PeakNitsFromLinear), so a
    // grey pixel comes out exactly as at peak 1.0; an Inf peak
    // behaves like a 10000-nit peak (Bt2390MakeParams caps via
    // Pq(Inf) = 1). Each case is compared against a same-test
    // reference, so the pin is on the fallback behaviour, not the
    // absolute byte.
    uint8_t refR = 0, refG = 0, refB = 0;
    ConvertLinearPixel(0.5, 0.5, 0.5, 1.0, refR, refG, refB);
    ConvertLinearPixel(0.5, 0.5, 0.5, std::nan(""), outR, outG, outB);
    Check(outR == refR && outG == refG && outB == refB,
          "linear 0.5 (peak NaN) == peak 1.0 (SDR fallback)");
    ConvertLinearPixel(0.5, 0.5, 0.5, -1.0, outR, outG, outB);
    Check(outR == refR && outG == refG && outB == refB,
          "linear 0.5 (peak -1) == peak 1.0 (SDR fallback)");
    ConvertLinearPixel(0.5, 0.5, 0.5, 0.0, outR, outG, outB);
    Check(outR == refR && outG == refG && outB == refB,
          "linear 0.5 (peak 0) == peak 1.0 (SDR fallback)");
    uint8_t ref10kR = 0, ref10kG = 0, ref10kB = 0;
    ConvertLinearPixel(0.5, 0.5, 0.5, 10000.0 / 203.0, ref10kR, ref10kG, ref10kB);
    ConvertLinearPixel(0.5, 0.5, 0.5, HUGE_VAL, outR, outG, outB);
    Check(outR == ref10kR && outG == ref10kG && outB == ref10kB,
          "linear 0.5 (peak Inf) == peak 10000 nits");
    // Negative and NaN inputs map to 0 (the !(v >= 0) guard).
    ConvertLinearPixel(-1.0, -1.0, -1.0, 1.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "linear negative -> 0");
    // P2-C2: an Inf pixel renders white (needs C1+C2 together: C1
    // stops PqNitsToSignal(Inf) from returning NaN, C2 stops the Inf
    // peak from poisoning the EETF). Before the fixes it came out black.
    double inf = HUGE_VAL;
    ConvertLinearPixel(inf, inf, inf, 1.0, outR, outG, outB);
    Check(outR == 255 && outG == 255 && outB == 255,
          "linear Inf pixel (peak 1.0) -> sRGB 255");
    double nan = std::nan("");
    ConvertLinearPixel(nan, nan, nan, 1.0, outR, outG, outB);
    Check(outR == 0 && outG == 0 && outB == 0, "linear NaN -> 0");
    // P2-C3 fast path: a color pixel at SDR peak takes the fast path
    // (peak 1.0, channels in [0,1]) and must stay pure red.
    ConvertLinearPixel(1.0, 0.0, 0.0, 1.0, outR, outG, outB);
    Check(outR == 255 && outG == 0 && outB == 0,
          "linear (1,0,0) (peak 1.0) -> sRGB (255,0,0)");
    // P2-C3 regression guard: a (2,0,0) pixel in an image whose peak
    // is 0.4252 (2*0.2126) must NOT take the fast path - the 2.0
    // channel is rolled off by the EETF to 1.0. The peak-only fast
    // path gave (255,142,142) here (GamutMap desaturates the
    // un-rolled-off 2.0 toward grey); the correct output is (255,0,0).
    ConvertLinearPixel(2.0, 0.0, 0.0, 0.4252, outR, outG, outB);
    Check(outR == 255 && outG == 0 && outB == 0,
          "linear (2,0,0) (peak 0.4252) -> sRGB (255,0,0)");
    // Same pixel with peak exactly 1.0: still the slow path (channel
    // above 1.0), still (255,0,0).
    ConvertLinearPixel(2.0, 0.0, 0.0, 1.0, outR, outG, outB);
    Check(outR == 255 && outG == 0 && outB == 0,
          "linear (2,0,0) (peak 1.0) -> sRGB (255,0,0)");
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
    // P2-C2: a single +Inf channel falls back to linear 1.0 (SDR
    // white); the black-image 0.0 return above is preserved.
    float withInf[3] = { (float)HUGE_VAL, 0.0f, 0.0f };
    Check(Near(ComputePeakLinear(withInf, 1, 1, 3), 1.0, 1e-12),
          "ComputePeakLinear Inf channel falls back to 1.0");
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
    // P2-C9: a color pixel. ConvertLinearPixel(1,0,0, peak=1.0) -> RGB
    // (255,0,0) -> BGR (0,0,255). Holds at any peak (a single (1,0,0)
    // image has peak luminance 0.2126, still 255).
    float red[3] = { 1.0f, 0.0f, 0.0f };
    uint8_t redOut[3] = { 0, 0, 0 };
    ConvertLinearImage(red, 1, 1, 3, redOut, 3);
    Check(redOut[0] == 0 && redOut[1] == 0 && redOut[2] == 255,
          "ConvertLinearImage red pixel -> BGR 0,0,255");
    // P2-C5: a stride narrower than a row is rejected (no write, no
    // crash). The output is pre-filled with a sentinel; the guard must
    // leave it untouched. (Without the guard the old code would have
    // written a wrong result here.)
    float img2[6] = { 1.0f, 1.0f, 1.0f, 0.18f, 0.18f, 0.18f };
    uint8_t out2[6] = { 0xCD, 0xCD, 0xCD, 0xCD, 0xCD, 0xCD };
    ConvertLinearImage(img2, 2, 1, 3, out2, 6);  // src stride 3 < 2*3
    Check(out2[0] == 0xCD && out2[1] == 0xCD && out2[2] == 0xCD &&
          out2[3] == 0xCD && out2[4] == 0xCD && out2[5] == 0xCD,
          "ConvertLinearImage narrow src stride rejected");
    ConvertLinearImage(img2, 2, 1, 6, out2, 3);  // dst stride 3 < 2*3
    Check(out2[0] == 0xCD && out2[1] == 0xCD && out2[2] == 0xCD &&
          out2[3] == 0xCD && out2[4] == 0xCD && out2[5] == 0xCD,
          "ConvertLinearImage narrow dst stride rejected");
    // P3-C5: padded strides (src stride wider than a row, dst stride
    // wider than a row). Only tight strides were exercised before;
    // the row-offset arithmetic must also work with padding, and the
    // padding bytes must be left untouched. The padding floats are
    // 9.0: if the stride were ignored and the buffer read linearly,
    // the 9.0 would become the peak and the pixels would change.
    float img3[18] = {
        1.0f, 1.0f, 1.0f, 0.18f, 0.18f, 0.18f, 9.0f, 9.0f, 9.0f,  // row 0 + pad
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 9.0f, 9.0f, 9.0f,   // row 1 + pad
    };
    uint8_t out3[18];
    std::memset(out3, 0xCD, sizeof(out3));
    ConvertLinearImage(img3, 2, 2, 9, out3, 9);
    Check(out3[0] == 255 && out3[1] == 255 && out3[2] == 255 &&
          out3[3] >= 116 && out3[3] <= 120 &&
          out3[4] >= 116 && out3[4] <= 120 &&
          out3[5] >= 116 && out3[5] <= 120,
          "ConvertLinearImage padded strides row 0 -> white + grey ~118");
    Check(out3[9] == 0 && out3[10] == 0 && out3[11] == 255 &&
          out3[12] == 0 && out3[13] == 0 && out3[14] == 0,
          "ConvertLinearImage padded strides row 1 -> red + black");
    Check(out3[6] == 0xCD && out3[7] == 0xCD && out3[8] == 0xCD &&
          out3[15] == 0xCD && out3[16] == 0xCD && out3[17] == 0xCD,
          "ConvertLinearImage padded strides leave padding untouched");
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
    // P2-B4: interleaved alpha (the AVIF/HEIF gain-map repack): the
    // alpha source is a BGRA buffer's own byte 3, stride 4 - the row
    // stride is width * alphaStride, so each pixel reads its own
    // alpha byte, not the next pixel's.
    uint8_t bgraSrc[8] = { 0, 0, 0, 128, 0, 0, 0, 64 };
    std::memset(out, 0xCD, sizeof(out));
    ConvertLinearImageToBgra(img, 2, 1, 6, out, 8, bgraSrc + 3, 4);
    Check(out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 128,
          "ConvertLinearImageToBgra interleaved alpha -> white pixel alpha 128");
    Check(out[4] >= 116 && out[4] <= 120 &&
          out[5] >= 116 && out[5] <= 120 &&
          out[6] >= 116 && out[6] <= 120 && out[7] == 64,
          "ConvertLinearImageToBgra interleaved alpha -> grey pixel alpha 64");
    // P2-C5: a dst stride narrower than a BGRA row (width*4) is rejected.
    uint8_t outB[8];
    std::memset(outB, 0xCD, sizeof(outB));
    ConvertLinearImageToBgra(img, 2, 1, 6, outB, 4, NULL, 0);  // 4 < 2*4
    Check(outB[0] == 0xCD && outB[3] == 0xCD && outB[7] == 0xCD,
          "ConvertLinearImageToBgra narrow dst stride rejected");
    // P3-C5: padded strides, as in TestConvertLinearImage above:
    // src stride 9 floats (3 pad), dst stride 12 bytes (4 pad),
    // alpha plane 4 bytes (128, 255, 64, 0). Padding must stay 0xCD.
    float imgP[18] = {
        1.0f, 1.0f, 1.0f, 0.18f, 0.18f, 0.18f, 9.0f, 9.0f, 9.0f,
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 9.0f, 9.0f, 9.0f,
    };
    uint8_t alphaP[4] = { 128, 255, 64, 0 };
    uint8_t outP[24];
    std::memset(outP, 0xCD, sizeof(outP));
    ConvertLinearImageToBgra(imgP, 2, 2, 9, outP, 12, alphaP, 1);
    Check(outP[0] == 255 && outP[1] == 255 && outP[2] == 255 && outP[3] == 128 &&
          outP[4] >= 116 && outP[4] <= 120 &&
          outP[5] >= 116 && outP[5] <= 120 &&
          outP[6] >= 116 && outP[6] <= 120 && outP[7] == 255,
          "ConvertLinearImageToBgra padded strides row 0 -> white/128 + grey/255");
    Check(outP[12] == 0 && outP[13] == 0 && outP[14] == 255 && outP[15] == 64 &&
          outP[16] == 0 && outP[17] == 0 && outP[18] == 0 && outP[19] == 0,
          "ConvertLinearImageToBgra padded strides row 1 -> red/64 + black/0");
    Check(outP[8] == 0xCD && outP[9] == 0xCD && outP[10] == 0xCD && outP[11] == 0xCD &&
          outP[20] == 0xCD && outP[21] == 0xCD && outP[22] == 0xCD && outP[23] == 0xCD,
          "ConvertLinearImageToBgra padded strides leave padding untouched");
}

}  // namespace

// P2-C3 hoisting: the WithParams variants must be byte-identical to the
// public functions (they share the same code, with the EETF preamble
// precomputed once). Sweeps edge values, including peakNits 1 ulp either
// side of 203 (where maxLum/ks round to exactly 1.0) and Inf/NaN peaks.
void TestHoistingEquivalence() {
    using namespace HdrToneMap;
    double sigs[] = { 0.0, 1e-6, 0.01, 0.3, 0.58068888, 0.6, 0.9, 1.0, 1.2, 2.0,
                      HUGE_VAL, std::nan(""), -0.5 };
    double pnits[] = { 0.0, 100.0, 202.999, 203.0,
                       std::nextafter(203.0, 0.0), std::nextafter(203.0, 10000.0),
                       203.001, 500.0, 1000.0, 10000.0, HUGE_VAL, std::nan(""), -1.0 };
    bool ok = true;
    for (int t = 0; t < 3; t++) {
        Transfer tr = (t == 0) ? Transfer::kPq : (t == 1) ? Transfer::kHlg : Transfer::kSrgb;
        for (double pn : pnits) {
            Bt2390Params params = Bt2390MakeParams(pn);
            for (double s : sigs) {
                uint8_t aR, aG, aB, bR, bG, bB;
                ConvertPixel(s, s * 0.5, 0.0, tr, Primaries::kBt2020, pn, aR, aG, aB);
                ConvertPixelWithParams(s, s * 0.5, 0.0, tr, Primaries::kBt2020, params, bR, bG, bB);
                if (aR != bR || aG != bG || aB != bB) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    Check(ok, "ConvertPixelWithParams == ConvertPixel (all transfers/peaks)");
    // Linear path: peaks around 1.0 (the fast-path boundary) and HDR peaks.
    double peaks[] = { 0.0, 0.2126, 0.5, 1.0,
                       std::nextafter(1.0, 0.0), std::nextafter(1.0, 2.0),
                       2.0, 16.0, HUGE_VAL, std::nan(""), -1.0 };
    double vals[] = { 0.0, 1e-9, 0.001, 0.18, 0.5, 0.999, 1.0, 1.0001, 2.0, 16.0,
                      HUGE_VAL, std::nan(""), -1.0 };
    ok = true;
    for (double pk : peaks) {
        double pn = pk * 203.0;
        if (!(pn > 0.0)) pn = 203.0;
        Bt2390Params params = Bt2390MakeParams(pn);
        for (double r : vals) {
            for (double g : vals) {
                uint8_t aR, aG, aB, bR, bG, bB;
                ConvertLinearPixel(r, g, 0.0, pk, aR, aG, aB);
                ConvertLinearPixelWithParams(r, g, 0.0, pk, params, bR, bG, bB);
                if (aR != bR || aG != bG || aB != bB) { ok = false; break; }
            }
            if (!ok) break;
        }
        if (!ok) break;
    }
    Check(ok, "ConvertLinearPixelWithParams == ConvertLinearPixel (all peaks)");
}

int main() {
    TestPqConstants();
    TestHlg();
    TestSrgbOetf();
    TestEetfIdentityAtSdrPeak();
    TestConvertPixel();
    TestDetection();
    TestDciP3Primaries();
    TestComputePeakNits();
    TestConvertLinearPixel();
    TestComputePeakLinear();
    TestConvertLinearImage();
    TestConvertLinearImageToBgra();
    TestHoistingEquivalence();
    std::printf("hdr_tonemap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
