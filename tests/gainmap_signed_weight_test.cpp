// gainmap_signed_weight_test.cpp - NB-1 signed-weight gain-map unification tests.
//
// What this pins (plan v3, section 3.2):
// --------------------------------------
// W0: ComputeGainWeight — the signed-weight formula
//     w = sign(alt-base) * clamp((H-base)/(alt-base), 0, 1), with the
//     degenerate branch (base == alt, or a non-finite / negative
//     headroom) judged BEFORE any division. SDR-base (0/2) and
//     HDR-base (2/0) x H in {0, base, alt, midpoint, out-of-range};
//     base == alt (0/0 and 2/2) is the step function at H = max; NaN
//     display headroom clamps to the base view (0); an invalid
//     headroom returns +1 outright (legacy direction).
// W1: the LUT weight clamp [0,1] -> [-1,1]. BuildGainTables is in an
//     anonymous namespace, so it is observed only indirectly, the same
//     way TestApplyGainMapLutExact does: the fused entry points
//     (ApplyGainMapToBgr/ToBgra) vs the old two-step path (ApplyGainMap
//     + ConvertLinearImage) at w = -1 are byte-identical when the EETF
//     identity holds (every post-gain logBoost >= 0, peak <= 1); the
//     Seine parameter set at w = -1 (gain byte 0 + linear 1.0 ->
//     ~1.198 > 1, slow EETF roll-off, identity does NOT hold) is
//     compared against the Python reference
//     (~/workspace/gainmap_reference.py) within +-1 per channel, on
//     both paths.
// W2: Apple fallback (HDRGainMapHeadroom 3.847906 -> alternate =
//     log2(ratio) ~= 1.944, base stays 0, appleHeadroomFallback set);
//     hdrgm without capacity -> base == alt == 0; the Mozilla 61-byte
//     ISO body (no URN, from the real AVIF tmap item) parses to the
//     section 3.1 values; the synthetic HDR-base JXL
//     (tests/samples/trattore-gainmap-hdrbase.jxl, base 0/1 -> 2/1 and
//     alt 2/1 -> 0/1) parses to base 2.0 / alt 0.0.
// W3: ApplyGainMapToBgra16 (the new 16-bit path) on synthetic inputs
//     — a uniform PQ base at the Mozilla signal level (47802, linear
//     ~4.01) with a two-level gain map at w = -1 -> (137, 188), the
//     SDR endpoint; an HLG base (signal -> linear via
//     HlgSignalToSceneLinear + OOTF, peak 2.0015 so the slow EETF
//     runs); useBaseColorSpace does not affect a single-channel
//     (grey) result; a non-opaque alpha plane (0 / 1 / 32768 / 65534
//     / 65535 -> 0 / 0 / 128 / 255 / 255, round-half-up); and the
//     rotation commutativity property (rotate-then-apply ==
//     apply-then-rotate, quarter turns, via the real
//     RotateGrayQuarterTurns for the gain map).
// W4(a): decoded pixel vectors — the Mozilla pair (64x32, full) and
//     the Seine 128x128 centre crop, from tests/samples/:
//     (a1) HDR-base w = -1 -> (137, 188) +-2 against
//     mozilla-sdr-endpoint.png (Bugzilla 2076123: the SDR target
//     shows two greys #898989/#bcbcbc); (a2) the same outputs
//     against the Python reference +-1 (8 sampled Seine pixels);
//     (a3)/(a4) the twin comparisons, RGB mean <= 6 / p99 <= 20
//     (empirical: the twins are two separate renders of one scene,
//     so a per-pixel bound is unusable — max 38 — while mean/p99
//     separates the correct w = -1 (mean ~4) from w = 0 (~18) and
//     w = +1 (~50+)).
//     The Seine vectors are a 128x128 crop (x 136..264, y 86..214),
//     not the full 400x300 image, to keep the repo small; the
//     Mozilla vectors are the full 64x32 image. Vectors were extracted
//     with ~/workspace/.scratch-gainmap-hdrbase/ (C extractor,
//     vendored libavif 1.4.2); the PNG endpoint was verified by
//     direct pixel read (left #898989, right #bcbcbc, split at x=32).
//     The four .avif sources and the endpoint PNG are tied to
//     these vectors by tests/check_gainmap_vectors.py (CI step in
//     the signed-weight job): ffmpeg re-extracts base + gain from
//     each AVIF and the greys from the PNG and compares them
//     against the committed vectors (bases at 8-bit, tolerance 1
//     LSB; gains tolerance 1; endpoint halves exact). An
//     extraction error in a vector can no longer silently pass CI.
//
// This test does NOT cover the Windows-only wiring (the wrapper
// extraction hoists, the reader-side alternate blending, the
// JPEGImage toggle generalisation): those are covered by the
// Windows CI build and the device test
// (JPEGView-NB-1-带符号权重-真机验证.md).
//
// Build and run (Linux):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/gainmap_signed_weight_test.cpp
//       src/JPEGView/GainMap.cpp src/JPEGView/HdrToneMap.cpp
//       -o /tmp/gainmap_signed_weight_test
//   /tmp/gainmap_signed_weight_test [tests/samples]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "GainMap.h"
#include "HdrToneMap.h"

static int g_checks = 0;
static int g_failures = 0;

static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

static bool Near(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

// |a - b| <= tol per channel (BGRA order).
static bool Near4(const uint8_t* px, int b, int g, int r, int a, int tol) {
    return std::abs((int)px[0] - b) <= tol && std::abs((int)px[1] - g) <= tol &&
           std::abs((int)px[2] - r) <= tol && std::abs((int)px[3] - a) <= tol;
}

static bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)n);
    bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// uint16 BGRA (little-endian host order, as stored in the vector
// files) -> float RGB signal in [0,1], the input
// ApplyGainMapToBgra16 takes.
static void Bgra16ToSignalRgb(const uint16_t* bgra, int w, int h,
                              std::vector<float>& rgb) {
    rgb.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        rgb[i * 3 + 0] = (float)bgra[i * 4 + 2] / 65535.0f;  // R
        rgb[i * 3 + 1] = (float)bgra[i * 4 + 1] / 65535.0f;  // G
        rgb[i * 3 + 2] = (float)bgra[i * 4 + 0] / 65535.0f;  // B
    }
}

// Quarter-turn rotation of a BGRA image, same convention as
// RotateGrayQuarterTurns: q = 1 is 90 deg clockwise,
// dst(tx = srcH-1-y, ty = x) = src(x, y); q = 2 is 180 deg;
// q = 3 is 270 deg clockwise, dst(tx = y, ty = srcW-1-x) = src(x, y).
static void RotateBgraQuarterTurns(const uint8_t* src, int srcW, int srcH,
                                   int quarterTurns, std::vector<uint8_t>& dst,
                                   int& dstW, int& dstH) {
    int q = ((quarterTurns % 4) + 4) % 4;
    dstW = (q % 2 == 1) ? srcH : srcW;
    dstH = (q % 2 == 1) ? srcW : srcH;
    dst.assign((size_t)dstW * dstH * 4, 0);
    for (int y = 0; y < srcH; ++y) {
        for (int x = 0; x < srcW; ++x) {
            int tx = 0, ty = 0;
            if (q == 1) {
                tx = srcH - 1 - y;
                ty = x;
            } else if (q == 2) {
                tx = srcW - 1 - x;
                ty = srcH - 1 - y;
            } else if (q == 3) {
                tx = y;
                ty = srcW - 1 - x;
            } else {
                tx = x;
                ty = y;
            }
            std::memcpy(&dst[((size_t)ty * dstW + tx) * 4],
                        &src[((size_t)y * srcW + x) * 4], 4);
        }
    }
}

// Same, for a uint16 BGRA image (the W3 rotation case rotates the
// 16-bit base before converting it to a float signal).
static void RotateBgra16QuarterTurns(const uint16_t* src, int srcW, int srcH,
                                     int quarterTurns, std::vector<uint16_t>& dst,
                                     int& dstW, int& dstH) {
    int q = ((quarterTurns % 4) + 4) % 4;
    dstW = (q % 2 == 1) ? srcH : srcW;
    dstH = (q % 2 == 1) ? srcW : srcH;
    dst.assign((size_t)dstW * dstH * 4, 0);
    for (int y = 0; y < srcH; ++y) {
        for (int x = 0; x < srcW; ++x) {
            int tx = 0, ty = 0;
            if (q == 1) {
                tx = srcH - 1 - y;
                ty = x;
            } else if (q == 2) {
                tx = srcW - 1 - x;
                ty = srcH - 1 - y;
            } else if (q == 3) {
                tx = y;
                ty = srcW - 1 - x;
            } else {
                tx = x;
                ty = y;
            }
            std::memcpy(&dst[((size_t)ty * dstW + tx) * 4],
                        &src[((size_t)y * srcW + x) * 4], 4 * sizeof(uint16_t));
        }
    }
}

static gainmap::Metadata MozillaMeta() {
    gainmap::Metadata m;
    m.baseHdrHeadroom = 2.0035324096679688;
    m.alternateHdrHeadroom = 0.0;
    m.gainMapMin = 2.956684112548828;
    m.gainMapMax = 3.920794725418091;
    m.gamma = 1.0;
    m.baseOffset = 0.015625;
    m.alternateOffset = 0.015625;
    m.valid = true;
    return m;
}

static gainmap::Metadata SeineMeta() {
    gainmap::Metadata m;
    m.baseHdrHeadroom = 1.3;
    m.alternateHdrHeadroom = 0.0;
    m.gainMapMin = -0.256907;
    m.gainMapMax = 1.277177;
    m.gamma = 0.953784;
    m.baseOffset = 0.015625;
    m.alternateOffset = 0.015625;
    m.valid = true;
    return m;
}

// W0: ComputeGainWeight.
void TestComputeGainWeight() {
    using namespace gainmap;
    struct Case {
        double base, alt, h, want;
        const char* what;
    };
    // NB-1 direction check: for an SDR base (0/2) the SDR display
    // (H = 0) needs NO gain (w = 0, the base view) and the HDR
    // display (H = 2) needs full gain (w = +1, the alternate view);
    // for an HDR base (2/0) it is the mirror image (H = 0 -> w = -1,
    // H = 2 -> w = 0). A pre-fix reader that clamped a negative
    // weight to 0 would return 0 for every HDR-base H < 2 case.
    static const Case kCases[] = {
        // SDR base (0/2).
        {0.0, 2.0, 0.0, 0.0, "W0: SDR base, H=0 (base view) -> 0"},
        {0.0, 2.0, 1.0, 0.5, "W0: SDR base, H=1 (midpoint) -> +0.5"},
        {0.0, 2.0, 2.0, 1.0, "W0: SDR base, H=2 (alt view) -> +1"},
        {0.0, 2.0, 3.0, 1.0, "W0: SDR base, H=3 (out of range) -> +1 clamped"},
        {0.0, 2.0, -0.5, 0.0, "W0: SDR base, H=-0.5 (out of range) -> 0 clamped"},
        {0.0, 2.0, std::nan(""), 0.0, "W0: SDR base, H=NaN -> 0 (base view)"},
        {0.0, 2.0, HUGE_VAL, 1.0, "W0: SDR base, H=+Inf -> +1 clamped"},
        {0.0, 2.0, -HUGE_VAL, 0.0, "W0: SDR base, H=-Inf -> 0 clamped"},
        // HDR base (2/0) — the NB-1 direction.
        {2.0, 0.0, 0.0, -1.0, "W0: HDR base, H=0 (alt view) -> -1"},
        {2.0, 0.0, 1.0, -0.5, "W0: HDR base, H=1 (midpoint) -> -0.5"},
        {2.0, 0.0, 2.0, 0.0, "W0: HDR base, H=2 (base view) -> 0"},
        {2.0, 0.0, 3.0, 0.0, "W0: HDR base, H=3 (out of range) -> 0 clamped"},
        {2.0, 0.0, -0.5, -1.0, "W0: HDR base, H=-0.5 (out of range) -> -1 clamped"},
        {2.0, 0.0, std::nan(""), 0.0, "W0: HDR base, H=NaN -> 0 (base view)"},
        {2.0, 0.0, HUGE_VAL, 0.0, "W0: HDR base, H=+Inf -> 0 clamped"},
        {2.0, 0.0, -HUGE_VAL, -1.0, "W0: HDR base, H=-Inf -> -1 clamped"},
        // Degenerate base == alt: the step function at H = max, the
        // formula's limit as alt -> base (judged before any division,
        // so it never divides by zero).
        {0.0, 0.0, 0.0, 1.0, "W0: base==alt==0, H=0 -> +1 (step at 0)"},
        {0.0, 0.0, 1.0, 1.0, "W0: base==alt==0, H=1 -> +1"},
        {0.0, 0.0, -1.0, 0.0, "W0: base==alt==0, H=-1 -> 0"},
        {0.0, 0.0, std::nan(""), 0.0, "W0: base==alt==0, H=NaN -> 0"},
        {2.0, 2.0, 2.0, 1.0, "W0: base==alt==2, H=2 -> +1 (step at 2)"},
        {2.0, 2.0, 3.0, 1.0, "W0: base==alt==2, H=3 -> +1"},
        {2.0, 2.0, 1.0, 0.0, "W0: base==alt==2, H=1 -> 0"},
        {2.0, 2.0, 1.9999, 0.0, "W0: base==alt==2, H=1.9999 -> 0"},
        // Invalid headroom -> +1 outright (legacy direction).
        {std::nan(""), 2.0, 1.0, 1.0, "W0: base=NaN -> +1"},
        {0.0, HUGE_VAL, 1.0, 1.0, "W0: alt=+Inf -> +1"},
        {HUGE_VAL, 0.0, 1.0, 1.0, "W0: base=+Inf -> +1"},
        {-1.0, 2.0, 1.0, 1.0, "W0: base=-1 (negative) -> +1"},
        {0.0, -0.5, 1.0, 1.0, "W0: alt=-0.5 (negative) -> +1"},
        {2.0, -1.0, 0.0, 1.0, "W0: base=2, alt=-1 (negative) -> +1"},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        const Case& c = kCases[i];
        gainmap::Metadata m;
        m.baseHdrHeadroom = c.base;
        m.alternateHdrHeadroom = c.alt;
        m.valid = true;
        Check(Near(gainmap::ComputeGainWeight(m, c.h), c.want, 1e-12), c.what);
    }
}

// R-10: the lazy 8-bit alternate must be refused exactly when the
// accurate 16-bit alternate was attempted and lost AND the base is
// HDR. The flag itself defaults to false and rides through a move
// (it lives on ExtractedGainMap, so it clears when the gain map is
// dropped and never leaks across images).
void TestShouldRefuseLazyAlternate() {
    struct Case {
        double base, alt;
        bool lost;
        bool want;
        const char* what;
    };
    static const Case kCases[] = {
        {2.0, 0.0, true, true, "R-10: HDR base, lost -> refuse"},
        {2.0, 0.0, false, false, "R-10: HDR base, never attempted -> lazy allowed"},
        {0.0, 2.0, true, false, "R-10: SDR base, lost -> lazy allowed (forward gain is exact)"},
        {0.0, 0.0, true, false, "R-10: degenerate base==alt, lost -> lazy allowed"},
        {2.0, 2.0, true, false, "R-10: degenerate base==alt==2, lost -> lazy allowed"},
        {1.3, 0.0, true, true, "R-10: Seine headroom 1.3, lost -> refuse"},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        const Case& c = kCases[i];
        gainmap::Metadata m;
        m.baseHdrHeadroom = c.base;
        m.alternateHdrHeadroom = c.alt;
        m.valid = true;
        Check(gainmap::ShouldRefuseLazyAlternate(m, c.lost) == c.want, c.what);
    }
    // Flag: default false, survives a move, cleared by a fresh struct.
    gainmap::ExtractedGainMap g;
    Check(g.accurateAlternateLost == false, "R-10: flag defaults to false");
    g.accurateAlternateLost = true;
    gainmap::ExtractedGainMap moved = std::move(g);
    Check(moved.accurateAlternateLost == true, "R-10: flag survives a move");
    gainmap::ExtractedGainMap fresh;
    Check(fresh.accurateAlternateLost == false, "R-10: fresh struct clears the flag");
}

// W1: fused vs non-fused at w = -1, and the Seine set vs the Python
// reference.
void TestFusedVsNonFusedNegativeWeight() {
    using namespace gainmap;
    // (a) Byte-exact fused == non-fused when the EETF identity holds
    // (every post-gain logBoost >= 0, so peak <= 1). Same 256x256
    // sweep as TestFusedGainMapExact in gainmap_test.cpp; the two
    // sets below both satisfy the condition at w = -1 (boost <= 1,
    // so post-gain linear <= SDR linear <= 1).
    {
        const int kW = 256, kH = 256;
        std::vector<uint8_t> baseRgb((size_t)kW * kH * 3);
        std::vector<uint8_t> gain((size_t)kW * kH);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                size_t i = (size_t)y * kW + x;
                gain[i] = (uint8_t)x;
                baseRgb[i * 3 + 0] = (uint8_t)y;
                baseRgb[i * 3 + 1] = (uint8_t)((x + y) & 0xff);
                baseRgb[i * 3 + 2] = (uint8_t)((7 * x + 13 * y) & 0xff);
            }
        }
        std::vector<uint8_t> baseBgr((size_t)kW * kH * 3);
        for (size_t i = 0; i < (size_t)kW * kH; ++i) {
            baseBgr[i * 3 + 0] = baseRgb[i * 3 + 2];
            baseBgr[i * 3 + 1] = baseRgb[i * 3 + 1];
            baseBgr[i * 3 + 2] = baseRgb[i * 3 + 0];
        }
        struct Params {
            double mn, mx, g, bo, ao;
            const char* what;
        };
        static const Params kSets[] = {
            {0.0, 1.0, 1.0, 0.0, 0.0, "W1: min 0 / max 1, w=-1"},
            {0.0, 3.0, 1.0, 0.0, 0.0, "W1: min 0 / max 3, w=-1"},
        };
        std::vector<float> linear((size_t)kW * kH * 3);
        std::vector<uint8_t> oldBgr((size_t)kW * kH * 3);
        std::vector<uint8_t> newBgr((size_t)kW * kH * 3);
        for (size_t s = 0; s < sizeof(kSets) / sizeof(kSets[0]); ++s) {
            const Params& p = kSets[s];
            Metadata m;
            m.gainMapMin = p.mn;
            m.gainMapMax = p.mx;
            m.gamma = p.g;
            m.baseOffset = p.bo;
            m.alternateOffset = p.ao;
            m.valid = true;
            bool ranOld = ApplyGainMap(baseRgb.data(), kW, kH, gain.data(), kW, kH,
                                       m, -1.0, linear.data());
            if (ranOld) {
                HdrToneMap::ConvertLinearImage(linear.data(), kW, kH, (size_t)kW * 3,
                                               oldBgr.data(), (size_t)kW * 3);
            }
            bool ranNew = ApplyGainMapToBgr(baseBgr.data(), kW, kH, (size_t)kW * 3,
                                            gain.data(), kW, kH, m, -1.0,
                                            newBgr.data(), (size_t)kW * 3);
            char what[128];
            std::snprintf(what, sizeof(what), "%s: both paths run", p.what);
            Check(ranOld && ranNew, what);
            std::snprintf(what, sizeof(what), "%s: fused == non-fused byte-exact",
                          p.what);
            Check(ranOld && ranNew && oldBgr == newBgr, what);
        }
    }
    // (b) The Seine parameter set at w = -1: gain byte 0 + linear 1.0
    // gives ~1.198 > 1, so the slow EETF roll-off runs and the
    // identity does NOT hold — compare both paths against the Python
    // reference (gainmap_reference.py, nb1 path) within +-1 per
    // channel. Base/gain are RGB row-major; expected bytes are the
    // Python reference output.
    {
        static const uint8_t kBaseRgb[4][4][3] = {
            {{255, 255, 255}, {128, 128, 128}, {0, 0, 0}, {255, 0, 0}},
            {{0, 255, 0}, {0, 0, 255}, {64, 128, 192}, {192, 128, 64}},
            {{255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {32, 64, 96}},
            {{96, 64, 32}, {10, 20, 30}, {200, 100, 50}, {1, 2, 3}},
        };
        static const uint8_t kGain[4][4] = {
            {0, 128, 255, 0}, {255, 0, 128, 64}, {32, 96, 160, 224}, {255, 255, 255, 0},
        };
        // Expected RGB, Python reference, peak_linear 1.19795.
        static const uint8_t kWant[4][4][3] = {
            {{255, 255, 255}, {108, 108, 108}, {0, 0, 0}, {255, 10, 10}},
            {{0, 170, 0}, {10, 10, 255}, {50, 108, 164}, {186, 124, 61}},
            {{254, 254, 3}, {233, 0, 233}, {0, 206, 206}, {0, 34, 61}},
            {{56, 29, 0}, {0, 0, 0}, {132, 59, 13}, {11, 12, 13}},
        };
        Metadata m = SeineMeta();
        // Non-fused path: ApplyGainMap (RGB) + ConvertLinearImage.
        std::vector<float> linear(4 * 4 * 3);
        std::vector<uint8_t> oldBgr(4 * 4 * 3);
        bool ranOld = ApplyGainMap(&kBaseRgb[0][0][0], 4, 4, &kGain[0][0], 4, 4,
                                   m, -1.0, linear.data());
        if (ranOld) {
            HdrToneMap::ConvertLinearImage(linear.data(), 4, 4, 12, oldBgr.data(), 12);
        }
        Check(ranOld, "W1: Seine set, non-fused path runs");
        // Fused path: ApplyGainMapToBgr (BGR).
        std::vector<uint8_t> baseBgr(4 * 4 * 3), newBgr(4 * 4 * 3);
        const uint8_t* flatRgb = &kBaseRgb[0][0][0];
        for (int i = 0; i < 16; ++i) {
            baseBgr[i * 3 + 0] = flatRgb[i * 3 + 2];
            baseBgr[i * 3 + 1] = flatRgb[i * 3 + 1];
            baseBgr[i * 3 + 2] = flatRgb[i * 3 + 0];
        }
        bool ranNew = ApplyGainMapToBgr(baseBgr.data(), 4, 4, 12, &kGain[0][0], 4, 4,
                                        m, -1.0, newBgr.data(), 12);
        Check(ranNew, "W1: Seine set, fused path runs");
        if (ranOld && ranNew) {
            bool oldOk = true, newOk = true;
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const uint8_t* o = &oldBgr[((size_t)y * 4 + x) * 3];
                    const uint8_t* n = &newBgr[((size_t)y * 4 + x) * 3];
                    // Expected is RGB; the outputs are BGR.
                    if (std::abs((int)o[0] - kWant[y][x][2]) > 1 ||
                        std::abs((int)o[1] - kWant[y][x][1]) > 1 ||
                        std::abs((int)o[2] - kWant[y][x][0]) > 1) {
                        oldOk = false;
                    }
                    if (std::abs((int)n[0] - kWant[y][x][2]) > 1 ||
                        std::abs((int)n[1] - kWant[y][x][1]) > 1 ||
                        std::abs((int)n[2] - kWant[y][x][0]) > 1) {
                        newOk = false;
                    }
                }
            }
            Check(oldOk, "W1: Seine set w=-1, non-fused matches Python reference (+-1)");
            Check(newOk, "W1: Seine set w=-1, fused matches Python reference (+-1)");
        }
    }
}

// W2: Apple fallback, hdrgm without capacity, the Mozilla ISO body,
// and the synthetic HDR-base JXL.
void TestAppleFallbackAndIso() {
    using namespace gainmap;
    // Apple fallback: the single Headroom number is both the max log
    // boost and the alternate headroom (plan section 2.6); the base
    // stays 0.0. 3.847906 is the real iPhone 15 Pro HEIC value.
    {
        Metadata m;
        Check(ParseXmpMetadata(std::string("<HDRGainMapHeadroom>3.847906</"
                                           "HDRGainMapHeadroom>"),
                               m) &&
                  m.valid,
              "W2: Apple Headroom XMP parses");
        Check(Near(m.alternateHdrHeadroom, std::log2(3.847906), 1e-12),
              "W2: Apple alternateHdrHeadroom == log2(3.847906)");
        Check(Near(m.alternateHdrHeadroom, 1.944, 0.001),
              "W2: Apple alternateHdrHeadroom ~= 1.944");
        Check(m.baseHdrHeadroom == 0.0, "W2: Apple baseHdrHeadroom stays 0.0");
        Check(m.appleHeadroomFallback, "W2: Apple appleHeadroomFallback set");
        Check(Near(m.gainMapMax, std::log2(3.847906), 1e-12),
              "W2: Apple gainMapMax == log2(3.847906)");
        Check(m.gainMapMin == 0.0 && m.gamma == 1.0,
              "W2: Apple gainMapMin 0, gamma 1.0");
    }
    // hdrgm without capacity: base == alt == 0 (degenerate). The
    // positive control (capacity present) takes the given values.
    {
        Metadata m;
        Check(ParseXmpMetadata(std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                                           "<hdrgm:GainMapMax>1.0</hdrgm:GainMapMax>"
                                           "<hdrgm:Gamma>1.0</hdrgm:Gamma>"),
                               m) &&
                  m.valid,
              "W2: hdrgm without capacity parses");
        Check(m.baseHdrHeadroom == 0.0 && m.alternateHdrHeadroom == 0.0,
              "W2: hdrgm without capacity -> base==alt==0");
        Metadata m2;
        Check(ParseXmpMetadata(std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                                           "<hdrgm:GainMapMax>1.0</hdrgm:GainMapMax>"
                                           "<hdrgm:Gamma>1.0</hdrgm:Gamma>"
                                           "<hdrgm:HDRCapacityMin>0.0</hdrgm:HDRCapacityMin>"
                                           "<hdrgm:HDRCapacityMax>2.300448</hdrgm:HDRCapacityMax>"),
                               m2) &&
                  m2.valid,
              "W2: hdrgm with capacity parses");
        Check(m2.baseHdrHeadroom == 0.0 &&
                  Near(m2.alternateHdrHeadroom, 2.300448, 1e-12),
              "W2: hdrgm with capacity -> base 0, alt 2.300448");
    }
    // The Mozilla 61-byte ISO body (no URN), taken verbatim from the
    // tmap item of mozilla-hdr-base-gainmap.avif (1 version byte +
    // 61-byte body). Parses to the section 3.1 values.
    {
        static const uint8_t kMozillaIsoBody[] = {
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0xcf, 0x00, 0x02, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0b, 0xd3,
            0xa5, 0x00, 0x04, 0x00, 0x00, 0x00, 0xfa, 0xee, 0x4d, 0x00, 0x40, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x40,
        };
        Metadata m;
        Check(ParseIso21496MetadataNoUrn(kMozillaIsoBody, sizeof(kMozillaIsoBody), m) &&
                  m.valid,
              "W2: Mozilla 61-byte ISO body parses");
        Check(Near(m.baseHdrHeadroom, 262607.0 / 131072.0, 1e-12),
              "W2: Mozilla baseHdrHeadroom 2.0035324");
        Check(m.alternateHdrHeadroom == 0.0, "W2: Mozilla alternateHdrHeadroom 0.0");
        Check(Near(m.gainMapMin, 775077.0 / 262144.0, 1e-12),
              "W2: Mozilla gainMapMin 2.9566841");
        Check(Near(m.gainMapMax, 16445005.0 / 4194304.0, 1e-12),
              "W2: Mozilla gainMapMax 3.9207947");
        Check(m.gamma == 1.0, "W2: Mozilla gamma 1.0");
        Check(m.baseOffset == 0.015625 && m.alternateOffset == 0.015625,
              "W2: Mozilla offsets 0.015625");
        Check(!m.multiChannel && !m.useBaseColorSpace,
              "W2: Mozilla flags 0 (single channel, base colour space)");
    }
}

// W2 (file part): the synthetic HDR-base JXL — trattore with BOTH
// rationals edited (base 0/1 -> 2/1, alt 2/1 -> 0/1). Editing the
// base alone would give the degenerate base == alt == 2.0 case and
// invalidate the test.
void TestJxlHdrBaseMetadata(const std::string& dir) {
    using namespace gainmap;
    std::vector<uint8_t> file;
    Check(ReadFile(dir + "/trattore-gainmap-hdrbase.jxl", file),
          "W2: synthetic HDR-base JXL readable");
    if (file.empty()) {
        return;
    }
    Check(file.size() == 5824u, "W2: synthetic HDR-base JXL is 5824 bytes (unchanged size)");
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    Check(FindJxlGainMapBox(file.data(), file.size(), payload, payloadSize),
          "W2: jhgm box found in synthetic HDR-base JXL");
    if (payload == nullptr) {
        return;
    }
    const uint8_t* meta = nullptr;
    size_t metaSize = 0;
    const uint8_t* code = nullptr;
    size_t codeSize = 0;
    Check(ParseJxlGainMapBundle(payload, payloadSize, meta, metaSize, code, codeSize),
          "W2: synthetic jhgm bundle parses");
    if (meta == nullptr) {
        return;
    }
    Check(metaSize == 141, "W2: synthetic metadata is 141 bytes");
    Metadata m;
    Check(ParseIso21496MetadataNoUrn(meta, metaSize, m) && m.valid,
          "W2: synthetic ISO metadata parses");
    Check(m.baseHdrHeadroom == 2.0, "W2: synthetic JXL baseHdrHeadroom 2.0 (HDR base)");
    Check(m.alternateHdrHeadroom == 0.0, "W2: synthetic JXL alternateHdrHeadroom 0.0");
    Check(m.gainMapMin == 0.0 && m.gainMapMax == 2.0,
          "W2: synthetic JXL gainMapMin 0, gainMapMax 2.0");
    Check(m.gamma == 1.0 && m.baseOffset == 0.0 && m.alternateOffset == 0.0,
          "W2: synthetic JXL gamma 1.0, offsets 0");
}

// W3: ApplyGainMapToBgra16 on synthetic inputs.
void TestApply16BitSynthetic() {
    using namespace gainmap;
    // (a) Uniform PQ base at the Mozilla signal level (47802, linear
    // ~4.01) + two-level gain (left 255 -> logBoost = gainMax 3.9208,
    // right 0 -> logBoost = gainMin 2.9567), w = -1 -> the SDR
    // endpoint (137, 188), peak_linear 0.5029 (fast path). w = +1 on
    // this base saturates everything to white (peak 60.95), so it
    // carries no information and is not asserted.
    {
        const int kW = 8, kH = 4;
        std::vector<uint16_t> base((size_t)kW * kH * 4, 47802);
        for (size_t i = 0; i < (size_t)kW * kH; ++i) {
            base[i * 4 + 3] = 65535;
        }
        std::vector<uint8_t> gain((size_t)kW * kH, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW / 2; ++x) {
                gain[(size_t)y * kW + x] = 255;
            }
        }
        std::vector<float> sig;
        Bgra16ToSignalRgb(base.data(), kW, kH, sig);
        std::vector<uint8_t> out((size_t)kW * kH * 4);
        Metadata m = MozillaMeta();
        Check(ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kPq,
                                   HdrToneMap::Primaries::kBt709, gain.data(), kW, kH,
                                   m, -1.0, nullptr, 0, out.data(), (size_t)kW * 4),
              "W3: synthetic PQ 16-bit w=-1 runs");
        bool ok = true;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const uint8_t* px = &out[((size_t)y * kW + x) * 4];
                if (x < kW / 2) {
                    if (!Near4(px, 137, 137, 137, 255, 1)) ok = false;
                } else {
                    if (!Near4(px, 188, 188, 188, 255, 1)) ok = false;
                }
            }
        }
        Check(ok, "W3: synthetic PQ w=-1 -> (137, 188) (+-1, Python reference)");
    }
    // (b) HLG base: two signal levels (0.5, 0.75) x two gain levels
    // (255, 0), modest gain range (min 0 / max 1), w = +1. The peak
    // is 2.0015, so the slow EETF runs (the 0.75-signal / gain-255
    // pixel saturates through the roll-off, not the fast path).
    {
        const int kW = 4, kH = 4;
        std::vector<uint16_t> base((size_t)kW * kH * 4, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                uint16_t s = (x < 2) ? 32767 : 49151;  // 0.5 / 0.75
                base[((size_t)y * kW + x) * 4 + 0] = s;
                base[((size_t)y * kW + x) * 4 + 1] = s;
                base[((size_t)y * kW + x) * 4 + 2] = s;
                base[((size_t)y * kW + x) * 4 + 3] = 65535;
            }
        }
        std::vector<uint8_t> gain((size_t)kW * kH, 0);
        for (int y = 0; y < kH; ++y) {
            gain[(size_t)y * kW + 0] = 255;
            gain[(size_t)y * kW + 2] = 255;
        }
        std::vector<float> sig;
        Bgra16ToSignalRgb(base.data(), kW, kH, sig);
        std::vector<uint8_t> out((size_t)kW * kH * 4);
        Metadata m;
        m.gainMapMin = 0.0;
        m.gainMapMax = 1.0;
        m.gamma = 1.0;
        m.baseOffset = 0.0;
        m.alternateOffset = 0.0;
        m.valid = true;
        Check(ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kHlg,
                                   HdrToneMap::Primaries::kBt709, gain.data(), kW, kH,
                                   m, 1.0, nullptr, 0, out.data(), (size_t)kW * 4),
              "W3: synthetic HLG 16-bit w=+1 runs");
        // Expected row (Python reference): (187, 137, 255, 244).
        static const int kWant[4] = {187, 137, 255, 244};
        bool ok = true;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const uint8_t* px = &out[((size_t)y * kW + x) * 4];
                if (!Near4(px, kWant[x], kWant[x], kWant[x], 255, 1)) ok = false;
            }
        }
        Check(ok, "W3: synthetic HLG w=+1 matches Python reference (+-1)");
    }
    // (c) useBaseColorSpace does not affect a single-channel (grey)
    // result: the same grey input, flag flipped, identical output.
    // BT.2020 primaries make the flag meaningful (GamutInverse
    // non-NULL); grey is preserved by any D65 gamut conversion and
    // the luma coefficients sum to 1, so the peak is the same too.
    {
        const int kW = 4, kH = 4;
        std::vector<uint16_t> base((size_t)kW * kH * 4, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                uint16_t s = (uint16_t)(10000 + x * 8000 + y * 3000);
                base[((size_t)y * kW + x) * 4 + 0] = s;
                base[((size_t)y * kW + x) * 4 + 1] = s;
                base[((size_t)y * kW + x) * 4 + 2] = s;
                base[((size_t)y * kW + x) * 4 + 3] = 65535;
            }
        }
        std::vector<uint8_t> gain((size_t)kW * kH, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                gain[(size_t)y * kW + x] = (uint8_t)((x * 60 + y * 25) & 0xff);
            }
        }
        std::vector<float> sig;
        Bgra16ToSignalRgb(base.data(), kW, kH, sig);
        std::vector<uint8_t> outA((size_t)kW * kH * 4), outB((size_t)kW * kH * 4);
        Metadata mA = MozillaMeta();
        mA.useBaseColorSpace = false;
        Metadata mB = MozillaMeta();
        mB.useBaseColorSpace = true;
        bool ranA = ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kPq,
                                         HdrToneMap::Primaries::kBt2020, gain.data(),
                                         kW, kH, mA, -1.0, nullptr, 0, outA.data(),
                                         (size_t)kW * 4);
        bool ranB = ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kPq,
                                         HdrToneMap::Primaries::kBt2020, gain.data(),
                                         kW, kH, mB, -1.0, nullptr, 0, outB.data(),
                                         (size_t)kW * 4);
        Check(ranA && ranB, "W3: useBaseColorSpace flip runs both ways");
        Check(ranA && ranB && outA == outB,
              "W3: useBaseColorSpace does not affect a grey result");
    }
    // (d) Non-opaque alpha: the gain applies to RGB regardless of
    // alpha, and the output alpha is round(alpha16*255/alphaMaxVal)
    // (round-half-up, uint64 arithmetic). Boundaries 0 and 65535 plus
    // mid values; NULL alpha -> opaque.
    {
        const int kW = 5, kH = 1;
        std::vector<uint16_t> base((size_t)kW * kH * 4, 47802);
        for (size_t i = 0; i < (size_t)kW * kH; ++i) {
            base[i * 4 + 3] = 65535;
        }
        static const uint16_t kAlpha[5] = {0, 1, 32768, 65534, 65535};
        static const uint8_t kWantAlpha[5] = {0, 0, 128, 255, 255};
        std::vector<uint8_t> gain((size_t)kW * kH, 255);
        std::vector<float> sig;
        Bgra16ToSignalRgb(base.data(), kW, kH, sig);
        std::vector<uint8_t> out((size_t)kW * kH * 4);
        Metadata m = MozillaMeta();
        Check(ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kPq,
                                   HdrToneMap::Primaries::kBt709, gain.data(), kW, kH,
                                   m, -1.0, kAlpha, 65535, out.data(), (size_t)kW * 4),
              "W3: non-opaque alpha 16-bit w=-1 runs");
        bool ok = true;
        for (int x = 0; x < kW; ++x) {
            const uint8_t* px = &out[(size_t)x * 4];
            if (!Near4(px, 137, 137, 137, kWantAlpha[x], 1)) ok = false;
        }
        Check(ok, "W3: non-opaque alpha RGB (137) + alpha (0/0/128/255/255)");
        std::vector<uint8_t> outOpaque((size_t)kW * kH * 4);
        Check(ApplyGainMapToBgra16(sig.data(), kW, kH, HdrToneMap::Transfer::kPq,
                                   HdrToneMap::Primaries::kBt709, gain.data(), kW, kH,
                                   m, -1.0, nullptr, 0, outOpaque.data(), (size_t)kW * 4),
              "W3: NULL alpha 16-bit w=-1 runs");
        bool opaqueOk = true;
        for (int x = 0; x < kW; ++x) {
            if (outOpaque[(size_t)x * 4 + 3] != 255) opaqueOk = false;
        }
        Check(opaqueOk, "W3: NULL alpha -> opaque (255)");
    }
    // (e) Rotation commutativity (scheme A, plan section 2.4 P2-8):
    // rotate-then-apply == apply-then-rotate for quarter turns.
    // The gain map is rotated with the real RotateGrayQuarterTurns;
    // the base / output with the test-local BGRA rotators (same
    // convention). A non-square 4x6 image makes the direction
    // observable; per-channel gradients make a channel swap visible.
    {
        const int kW = 4, kH = 6;
        std::vector<uint16_t> base((size_t)kW * kH * 4, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                size_t i = (size_t)y * kW + x;
                base[i * 4 + 0] = (uint16_t)(20000 + (x + y) * 1500);  // B
                base[i * 4 + 1] = (uint16_t)(20000 + y * 2000);        // G
                base[i * 4 + 2] = (uint16_t)(20000 + x * 3000);        // R
                base[i * 4 + 3] = 65535;
            }
        }
        std::vector<uint8_t> gain((size_t)kW * kH, 0);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                gain[(size_t)y * kW + x] = (uint8_t)((x * 40 + y * 10) & 0xff);
            }
        }
        gain[1 * kW + 2] = 255;
        gain[5 * kW + 3] = 0;
        Metadata m = MozillaMeta();
        for (int q = 1; q <= 3; ++q) {
            // Path A: apply, then rotate the output.
            std::vector<float> sig;
            Bgra16ToSignalRgb(base.data(), kW, kH, sig);
            std::vector<uint8_t> outA((size_t)kW * kH * 4);
            bool ranA = ApplyGainMapToBgra16(sig.data(), kW, kH,
                                             HdrToneMap::Transfer::kPq,
                                             HdrToneMap::Primaries::kBt709, gain.data(),
                                             kW, kH, m, -1.0, nullptr, 0,
                                             outA.data(), (size_t)kW * 4);
            std::vector<uint8_t> outARot;
            int rW = 0, rH = 0;
            RotateBgraQuarterTurns(outA.data(), kW, kH, q, outARot, rW, rH);
            // Path B: rotate base + gain, then apply.
            std::vector<uint16_t> baseRot;
            int bW = 0, bH = 0;
            RotateBgra16QuarterTurns(base.data(), kW, kH, q, baseRot, bW, bH);
            std::vector<uint8_t> gainRot;
            int gW = 0, gH = 0;
            bool rotGain = RotateGrayQuarterTurns(gain.data(), kW, kH, q, gainRot, gW, gH);
            std::vector<float> sigRot;
            Bgra16ToSignalRgb(baseRot.data(), bW, bH, sigRot);
            std::vector<uint8_t> outB((size_t)bW * bH * 4);
            bool ranB = rotGain && gW == bW && gH == bH &&
                        ApplyGainMapToBgra16(sigRot.data(), bW, bH,
                                             HdrToneMap::Transfer::kPq,
                                             HdrToneMap::Primaries::kBt709, gainRot.data(),
                                             gW, gH, m, -1.0, nullptr, 0,
                                             outB.data(), (size_t)bW * 4);
            char what[128];
            std::snprintf(what, sizeof(what),
                          "W3: rotation q=%d runs (apply + rotate-gain dims %dx%d)",
                          q, gW, gH);
            Check(ranA && ranB, what);
            std::snprintf(what, sizeof(what),
                          "W3: rotation q=%d commutativity (rotate-then-apply == "
                          "apply-then-rotate)",
                          q);
            Check(ranA && ranB && outARot == outB, what);
        }
    }
}

// W4(a): decoded pixel vectors from tests/samples/.
void TestRealVectors(const std::string& dir) {
    using namespace gainmap;
    // (a1) Mozilla pair, full 64x32. HDR-base w = -1 -> the two
    // greys of mozilla-sdr-endpoint.png (+-2; the Python reference
    // gives exactly (137, 188)). The SDR-base pair through the 8-bit
    // path at w = +1 saturates to white on both halves.
    {
        std::vector<uint8_t> hdrFile, sdrFile, hdrGainFile, sdrGainFile;
        Check(ReadFile(dir + "/mozilla-hdr-base16.bgra", hdrFile) &&
                  hdrFile.size() == 64u * 32u * 4u * 2u,
              "W4: Mozilla HDR base16 vector readable (64x32)");
        Check(ReadFile(dir + "/mozilla-hdr-gain8.raw", hdrGainFile) &&
                  hdrGainFile.size() == 64u * 32u,
              "W4: Mozilla HDR gain8 vector readable");
        Check(ReadFile(dir + "/mozilla-sdr-base16.bgra", sdrFile) &&
                  sdrFile.size() == 64u * 32u * 4u * 2u,
              "W4: Mozilla SDR base16 vector readable (64x32)");
        Check(ReadFile(dir + "/mozilla-sdr-gain8.raw", sdrGainFile) &&
                  sdrGainFile.size() == 64u * 32u,
              "W4: Mozilla SDR gain8 vector readable");
        if (hdrFile.size() == 64u * 32u * 4u * 2u &&
            hdrGainFile.size() == 64u * 32u) {
            std::vector<float> sig;
            Bgra16ToSignalRgb(reinterpret_cast<const uint16_t*>(hdrFile.data()),
                              64, 32, sig);
            std::vector<uint8_t> out(64u * 32u * 4u);
            Metadata m = MozillaMeta();
            Check(ApplyGainMapToBgra16(sig.data(), 64, 32, HdrToneMap::Transfer::kPq,
                                       HdrToneMap::Primaries::kBt709, hdrGainFile.data(),
                                       64, 32, m, -1.0, nullptr, 0, out.data(),
                                       64u * 4u),
                  "W4(a1): Mozilla HDR-base w=-1 runs");
            bool ok = true;
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const uint8_t* px = &out[((size_t)y * 64 + x) * 4];
                    if (x < 32) {
                        if (!Near4(px, 137, 137, 137, 255, 2)) ok = false;
                    } else {
                        if (!Near4(px, 188, 188, 188, 255, 2)) ok = false;
                    }
                }
            }
            Check(ok, "W4(a1): Mozilla HDR-base w=-1 -> (137, 188) +-2 (endpoint PNG)");
        }
        if (sdrFile.size() == 64u * 32u * 4u * 2u &&
            sdrGainFile.size() == 64u * 32u) {
            // SDR base through the 8-bit path at w = +1.
            std::vector<uint8_t> base8(64u * 32u * 4u);
            const uint16_t* sdr16 =
                reinterpret_cast<const uint16_t*>(sdrFile.data());
            for (size_t i = 0; i < 64u * 32u; ++i) {
                base8[i * 4 + 0] = (uint8_t)(sdr16[i * 4 + 0] >> 8);
                base8[i * 4 + 1] = (uint8_t)(sdr16[i * 4 + 1] >> 8);
                base8[i * 4 + 2] = (uint8_t)(sdr16[i * 4 + 2] >> 8);
                base8[i * 4 + 3] = 255;
            }
            std::vector<uint8_t> out(64u * 32u * 4u);
            Metadata m = MozillaMeta();
            Check(ApplyGainMapToBgra(base8.data(), 64, 32, 64u * 4u,
                                     sdrGainFile.data(), 64, 32, m, 1.0, out.data(),
                                     64u * 4u),
                  "W4(a1): Mozilla SDR-base 8-bit w=+1 runs");
            bool white = true;
            for (size_t i = 0; i < 64u * 32u; ++i) {
                if (out[i * 4 + 0] != 255 || out[i * 4 + 1] != 255 ||
                    out[i * 4 + 2] != 255) {
                    white = false;
                }
            }
            Check(white, "W4(a1): Mozilla SDR-base 8-bit w=+1 saturates to white");
        }
    }
    // (a2)/(a3)/(a4) Seine 128x128 centre crop.
    {
        std::vector<uint8_t> hdrFile, sdrFile, hdrGainFile, sdrGainFile;
        Check(ReadFile(dir + "/seine-hdr-base16-crop128.bgra", hdrFile) &&
                  hdrFile.size() == 128u * 128u * 4u * 2u,
              "W4: Seine HDR crop128 base16 readable");
        Check(ReadFile(dir + "/seine-hdr-gain8-crop128.raw", hdrGainFile) &&
                  hdrGainFile.size() == 128u * 128u,
              "W4: Seine HDR crop128 gain8 readable");
        Check(ReadFile(dir + "/seine-sdr-base16-crop128.bgra", sdrFile) &&
                  sdrFile.size() == 128u * 128u * 4u * 2u,
              "W4: Seine SDR crop128 base16 readable");
        Check(ReadFile(dir + "/seine-sdr-gain8-crop128.raw", sdrGainFile) &&
                  sdrGainFile.size() == 128u * 128u,
              "W4: Seine SDR crop128 gain8 readable");
        if (hdrFile.size() != 128u * 128u * 4u * 2u ||
            hdrGainFile.size() != 128u * 128u || sdrFile.size() != 128u * 128u * 4u * 2u ||
            sdrGainFile.size() != 128u * 128u) {
            return;
        }
        Metadata m = SeineMeta();
        // (a2) + (a3): HDR base, 16-bit path, w = -1.
        std::vector<float> hdrSig;
        Bgra16ToSignalRgb(reinterpret_cast<const uint16_t*>(hdrFile.data()), 128, 128,
                          hdrSig);
        std::vector<uint8_t> outHdr(128u * 128u * 4u);
        Check(ApplyGainMapToBgra16(hdrSig.data(), 128, 128, HdrToneMap::Transfer::kPq,
                                   HdrToneMap::Primaries::kBt709, hdrGainFile.data(),
                                   128, 128, m, -1.0, nullptr, 0, outHdr.data(),
                                   128u * 4u),
              "W4(a2): Seine crop HDR-base w=-1 runs");
        // (a2) 8 sampled pixels against the Python reference (+-1).
        static const int kSample[8][6] = {
            // {x, y, B, G, R, A}
            {0, 0, 228, 224, 220, 255},     {127, 0, 225, 223, 218, 255},
            {0, 127, 184, 184, 174, 255},   {127, 127, 82, 89, 101, 255},
            {64, 64, 43, 41, 31, 255},      {96, 32, 222, 219, 212, 255},
            {32, 96, 187, 185, 176, 255},   {100, 10, 229, 225, 220, 255},
        };
        bool samplesOk = true;
        for (int i = 0; i < 8; ++i) {
            const int x = kSample[i][0], y = kSample[i][1];
            const uint8_t* px = &outHdr[((size_t)y * 128 + x) * 4];
            if (!Near4(px, kSample[i][2], kSample[i][3], kSample[i][4],
                       kSample[i][5], 1)) {
                samplesOk = false;
            }
        }
        Check(samplesOk, "W4(a2): Seine crop w=-1 samples match Python reference (+-1)");
        // (a3): the same output vs the SDR twin base (>>8, no gain).
        // RGB-only per-channel abs diff, mean <= 6 / p99 <= 20.
        {
            const uint16_t* sdr16 =
                reinterpret_cast<const uint16_t*>(sdrFile.data());
            std::vector<int> diffs;
            diffs.reserve(128u * 128u * 3u);
            double sum = 0.0;
            for (size_t i = 0; i < 128u * 128u; ++i) {
                for (int c = 0; c < 3; ++c) {
                    int d = std::abs((int)outHdr[i * 4 + c] -
                                     (int)(sdr16[i * 4 + c] >> 8));
                    diffs.push_back(d);
                    sum += d;
                }
            }
            double mean = sum / (double)diffs.size();
            std::sort(diffs.begin(), diffs.end());
            size_t p99idx =
                (size_t)std::ceil(0.99 * (double)diffs.size()) - 1;
            int p99 = diffs[p99idx];
            char what[128];
            std::snprintf(what, sizeof(what),
                          "W4(a3): Seine twin mean %.2f <= 6 (HDR w=-1 vs SDR base)",
                          mean);
            Check(mean <= 6.0, what);
            std::snprintf(what, sizeof(what), "W4(a3): Seine twin p99 %d <= 20", p99);
            Check(p99 <= 20, what);
        }
        // (a4): SDR base, 8-bit path, w = +1, vs the HDR base
        // tone-mapped with no gain (16-bit path, w = 0).
        std::vector<uint8_t> base8(128u * 128u * 4u);
        {
            const uint16_t* sdr16 =
                reinterpret_cast<const uint16_t*>(sdrFile.data());
            for (size_t i = 0; i < 128u * 128u; ++i) {
                base8[i * 4 + 0] = (uint8_t)(sdr16[i * 4 + 0] >> 8);
                base8[i * 4 + 1] = (uint8_t)(sdr16[i * 4 + 1] >> 8);
                base8[i * 4 + 2] = (uint8_t)(sdr16[i * 4 + 2] >> 8);
                base8[i * 4 + 3] = 255;
            }
        }
        std::vector<uint8_t> outSdr(128u * 128u * 4u);
        Check(ApplyGainMapToBgra(base8.data(), 128, 128, 128u * 4u,
                                 sdrGainFile.data(), 128, 128, m, 1.0, outSdr.data(),
                                 128u * 4u),
              "W4(a4): Seine crop SDR-base 8-bit w=+1 runs");
        std::vector<uint8_t> zeroGain(128u * 128u, 0);
        std::vector<uint8_t> outHdrNoGain(128u * 128u * 4u);
        Check(ApplyGainMapToBgra16(hdrSig.data(), 128, 128, HdrToneMap::Transfer::kPq,
                                   HdrToneMap::Primaries::kBt709, zeroGain.data(),
                                   128, 128, m, 0.0, nullptr, 0, outHdrNoGain.data(),
                                   128u * 4u),
              "W4(a4): Seine crop HDR-base w=0 (no gain) runs");
        {
            std::vector<int> diffs;
            diffs.reserve(128u * 128u * 3u);
            double sum = 0.0;
            for (size_t i = 0; i < 128u * 128u; ++i) {
                for (int c = 0; c < 3; ++c) {
                    int d = std::abs((int)outSdr[i * 4 + c] -
                                     (int)outHdrNoGain[i * 4 + c]);
                    diffs.push_back(d);
                    sum += d;
                }
            }
            double mean = sum / (double)diffs.size();
            std::sort(diffs.begin(), diffs.end());
            size_t p99idx =
                (size_t)std::ceil(0.99 * (double)diffs.size()) - 1;
            int p99 = diffs[p99idx];
            char what[128];
            std::snprintf(what, sizeof(what),
                          "W4(a4): Seine twin mean %.2f <= 6 (SDR w=+1 vs HDR no-gain)",
                          mean);
            Check(mean <= 6.0, what);
            std::snprintf(what, sizeof(what), "W4(a4): Seine twin p99 %d <= 20", p99);
            Check(p99 <= 20, what);
        }
    }
}

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    TestComputeGainWeight();
    TestShouldRefuseLazyAlternate();
    TestFusedVsNonFusedNegativeWeight();
    TestAppleFallbackAndIso();
    TestJxlHdrBaseMetadata(dir);
    TestApply16BitSynthetic();
    TestRealVectors(dir);

    std::printf("gainmap_signed_weight_test: %d checks, %d failures\n", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
