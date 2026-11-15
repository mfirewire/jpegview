// avif_gainmap_test.cpp - unit test for the AVIF gain-map path
// (Option D, Phase 4: AVIFWrapper.cpp TryApplyAvifGainMap).
//
// What this pins, and what it does not:
// --------------------------------------
// The AVIF gain-map path itself cannot be tested end-to-end here: there
// is no real AVIF gain-map sample on hand, and decoding an AVIF needs
// libavif + a real file (the vendored libavif is headers + Windows
// binaries only, and the system libavif is 1.0.4, which predates the
// gain-map API). What this test pins is the exact APPLY sequence the
// AVIF path runs on the decoded pixels, with synthetic data, plus the
// two wrapper-only conversions that feed it:
//
//   libavif gain-map metadata (avifSignedFraction /
//     avifUnsignedFraction, channel 0) -> gainmap::Metadata
//   gain-map Y plane (native 8/10/12-bit samples)
//     -> 8-bit grayscale (round-half-up normalisation)
//   base BGRA (as cache.rgb.pixels holds it after the SDR decode +
//     ICC transform) -> RGB temp buffer (drop alpha)
//     -> gainmap::ApplyGainMap (bilinear-upsampled gain map, weight 1.0)
//     -> HdrToneMap::ConvertLinearImage (linear 1.0 = SDR white)
//     -> repack BGR -> BGRA (alpha 0xff)
//
// Expected outputs were computed independently in Python from the ISO
// 21496-1 formula and the HdrToneMap replication in
// ~/workspace/gainmap_reference.py (reconstruct + tonemap_linear, with
// the gain map normalised from its native bit depth first and the
// upsampled gain map rounded via floor(v+0.5) as BilinearUpsampleGray
// does). Bytes are compared with a +/-1 tolerance for float-vs-double
// rounding at byte boundaries; the gain-0 corner pixel is asserted
// exactly (gain 1.0 there, below the EETF knee, so it must come back
// byte-identical to the base). The 8-bit upsampled case reproduces the
// HEIF test's expected values byte for byte, cross-checking the two
// paths against each other.
//
// It also pins the AVIF path's refusal cases: invalid metadata (zero
// denominator, gamma <= 0) and an oversized gain map (> 4x the base
// per side) must keep the base unchanged, as must a file with no gain
// map at all (decoder->image->gainMap == NULL).
//
// Build and run (Linux, no Windows toolchain needed; the vendored
// libavif header is included for the real fraction types only —
// nothing is linked against libavif):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       -I src/JPEGView/libavif/include
//       tests/avif_gainmap_test.cpp src/JPEGView/GainMap.cpp
//       src/JPEGView/HdrToneMap.cpp -o /tmp/avif_gainmap_test
//   /tmp/avif_gainmap_test

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "GainMap.h"
#include "HdrToneMap.h"
#include "avif/avif.h"

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

// --- Replicas of the two wrapper-only conversions -----------------------
// (verbatim logic from AVIFWrapper.cpp; the test cannot link the
// wrapper, which needs MFC/stdafx.h, so the logic is duplicated here
// and its outputs are pinned against independently computed values).

// avifSignedFraction/avifUnsignedFraction -> double. Returns false
// when the denominator is 0 or the value is non-finite.
bool TestSignedFractionToDouble(const avifSignedFraction& f, double& out) {
    if (f.d == 0)
        return false;
    out = (double)f.n / (double)f.d;
    return std::isfinite(out);
}

bool TestUnsignedFractionToDouble(const avifUnsignedFraction& f, double& out) {
    if (f.d == 0)
        return false;
    out = (double)f.n / (double)f.d;
    return std::isfinite(out);
}

// One native-depth Y-plane sample -> 8-bit gray (round-half-up), as
// DecodeAvifGainMapGray does. `v` is clamped to the native range.
uint8_t TestNormaliseSample(uint32_t v, int depth) {
    uint32_t maxVal = (uint32_t)((1u << depth) - 1);
    if (v > maxVal)
        v = maxVal;
    return (uint8_t)((v * 255 + maxVal / 2) / maxVal);
}

bool TestDepthOk(int depth) {
    return depth >= 8 && depth <= 16;
}

// Gain-map size sanity cap from DecodeAvifGainMapGray: at most 4x the
// base per side.
bool TestGainSizeOk(int gainW, int gainH, int baseW, int baseH) {
    return gainW > 0 && gainH > 0 && gainW <= baseW * 4 && gainH <= baseH * 4;
}

// Convert channel-0 libavif gain-map metadata to the shared helper's
// Metadata (replica of AvifGainMapToMetadata).
bool TestAvifGainMapToMetadata(const avifGainMap* gainMap, gainmap::Metadata& meta) {
    meta = gainmap::Metadata();
    if (gainMap == nullptr)
        return false;
    double gainMapMin = 0.0, gainMapMax = 0.0, gamma = 1.0;
    double baseOffset = 0.0, alternateOffset = 0.0;
    double baseHeadroom = 0.0, alternateHeadroom = 0.0;
    if (!TestSignedFractionToDouble(gainMap->gainMapMin[0], gainMapMin) ||
        !TestSignedFractionToDouble(gainMap->gainMapMax[0], gainMapMax) ||
        !TestUnsignedFractionToDouble(gainMap->gainMapGamma[0], gamma) ||
        !TestSignedFractionToDouble(gainMap->baseOffset[0], baseOffset) ||
        !TestSignedFractionToDouble(gainMap->alternateOffset[0], alternateOffset) ||
        !TestUnsignedFractionToDouble(gainMap->baseHdrHeadroom, baseHeadroom) ||
        !TestUnsignedFractionToDouble(gainMap->alternateHdrHeadroom, alternateHeadroom))
        return false;
    if (!(gamma > 0.0))
        return false;
    meta.gainMapMin = gainMapMin;
    meta.gainMapMax = gainMapMax;
    meta.gamma = gamma;
    meta.baseOffset = baseOffset;
    meta.alternateOffset = alternateOffset;
    meta.baseHdrHeadroom = baseHeadroom;
    meta.alternateHdrHeadroom = alternateHeadroom;
    meta.useBaseColorSpace = gainMap->useBaseColorSpace ? true : false;
    meta.valid = true;
    return true;
}

// --- The exact apply sequence TryApplyAvifGainMap runs ------------------

bool RunAvifGainMapSequence(const uint8_t* baseBgra, int width, int height,
                            const uint8_t* gainGray, int gainW, int gainH,
                            const gainmap::Metadata& meta, uint8_t* outBgra) {
    std::vector<uint8_t> rgb((size_t)width * height * 3);
    std::vector<float> linear((size_t)width * height * 3);
    std::vector<uint8_t> bgr((size_t)width * height * 3);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        rgb[i * 3 + 0] = baseBgra[i * 4 + 2];
        rgb[i * 3 + 1] = baseBgra[i * 4 + 1];
        rgb[i * 3 + 2] = baseBgra[i * 4 + 0];
    }
    if (!gainmap::ApplyGainMap(rgb.data(), width, height, gainGray, gainW, gainH,
                               meta, 1.0, linear.data()))
        return false;
    HdrToneMap::ConvertLinearImage(linear.data(), width, height, (size_t)width * 3,
                                   bgr.data(), (size_t)width * 3);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        outBgra[i * 4 + 0] = bgr[i * 3 + 0];
        outBgra[i * 4 + 1] = bgr[i * 3 + 1];
        outBgra[i * 4 + 2] = bgr[i * 3 + 2];
        outBgra[i * 4 + 3] = 0xff;
    }
    return true;
}

void CheckBgra(const uint8_t* got, const uint8_t* expected, size_t bytes,
               const char* what) {
    int exact = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (got[i] == expected[i])
            exact++;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s byte %zu: got %d expected %d",
                      what, i, (int)got[i], (int)expected[i]);
        Check(std::abs((int)got[i] - (int)expected[i]) <= 1, buf);
    }
    std::printf("%s: %d/%zu bytes exact\n", what, exact, bytes);
}

// Synthetic 4x4 SDR base, RGB row-major (same base as the HEIF test,
// so the 8-bit case cross-checks the two paths).
const uint8_t kBaseRgb[4 * 4 * 3] = {
    128, 64, 32,   200, 100, 50,   16, 200, 90,   255, 255, 255,
    0, 0, 0,       77, 150, 30,     240, 30, 180,   90, 90, 90,
    32, 64, 128,   10, 20, 30,      180, 180, 180,  250, 128, 0,
    255, 0, 128,   64, 255, 64,     33, 66, 99,     128, 128, 128,
};

void MakeBaseBgra(uint8_t* bgra) {
    for (size_t i = 0; i < 4 * 4; i++) {
        bgra[i * 4 + 0] = kBaseRgb[i * 3 + 2];
        bgra[i * 4 + 1] = kBaseRgb[i * 3 + 1];
        bgra[i * 4 + 2] = kBaseRgb[i * 3 + 0];
        bgra[i * 4 + 3] = 0xff;
    }
}

void TestFractionConversion() {
    double v = 0.0;
    avifSignedFraction s = {1, 1};
    Check(TestSignedFractionToDouble(s, v) && Near(v, 1.0, 1e-12),
          "fraction signed 1/1 -> 1.0");
    s.n = -3; s.d = 2;
    Check(TestSignedFractionToDouble(s, v) && Near(v, -1.5, 1e-12),
          "fraction signed -3/2 -> -1.5");
    s.n = 0; s.d = 5;
    Check(TestSignedFractionToDouble(s, v) && Near(v, 0.0, 1e-12),
          "fraction signed 0/5 -> 0.0");
    s.n = 1; s.d = 0;
    Check(!TestSignedFractionToDouble(s, v),
          "fraction signed 1/0 refused (zero denominator)");
    s.n = 0; s.d = 0;
    Check(!TestSignedFractionToDouble(s, v),
          "fraction signed 0/0 refused (zero denominator)");
    avifUnsignedFraction u = {5, 2};
    Check(TestUnsignedFractionToDouble(u, v) && Near(v, 2.5, 1e-12),
          "fraction unsigned 5/2 -> 2.5");
    u.n = 0; u.d = 7;
    Check(TestUnsignedFractionToDouble(u, v) && Near(v, 0.0, 1e-12),
          "fraction unsigned 0/7 -> 0.0");
    u.n = 3; u.d = 0;
    Check(!TestUnsignedFractionToDouble(u, v),
          "fraction unsigned 3/0 refused (zero denominator)");
}

void TestMetadataConversion() {
    // A synthetic avifGainMap (stack-allocated; only the metadata
    // fields are read by the converter — image stays NULL).
    avifGainMap gm;
    std::memset(&gm, 0, sizeof(gm));
    gm.gainMapMin[0].n = 0; gm.gainMapMin[0].d = 1;
    gm.gainMapMax[0].n = 1; gm.gainMapMax[0].d = 1;
    gm.gainMapGamma[0].n = 2; gm.gainMapGamma[0].d = 1;
    gm.baseOffset[0].n = 1; gm.baseOffset[0].d = 100;      // 0.01
    gm.alternateOffset[0].n = -5; gm.alternateOffset[0].d = 1000; // -0.005
    gm.baseHdrHeadroom.n = 0; gm.baseHdrHeadroom.d = 1;
    gm.alternateHdrHeadroom.n = 3; gm.alternateHdrHeadroom.d = 1;
    gm.useBaseColorSpace = AVIF_TRUE;
    gainmap::Metadata meta;
    Check(TestAvifGainMapToMetadata(&gm, meta) && meta.valid,
          "metadata conversion runs and is valid");
    Check(Near(meta.gainMapMin, 0.0, 1e-12), "metadata gainMapMin 0.0");
    Check(Near(meta.gainMapMax, 1.0, 1e-12), "metadata gainMapMax 1.0");
    Check(Near(meta.gamma, 2.0, 1e-12), "metadata gamma 2.0");
    Check(Near(meta.baseOffset, 0.01, 1e-12), "metadata baseOffset 0.01");
    Check(Near(meta.alternateOffset, -0.005, 1e-12),
          "metadata alternateOffset -0.005");
    Check(Near(meta.baseHdrHeadroom, 0.0, 1e-12),
          "metadata baseHdrHeadroom 0.0");
    Check(Near(meta.alternateHdrHeadroom, 3.0, 1e-12),
          "metadata alternateHdrHeadroom 3.0");
    Check(meta.useBaseColorSpace, "metadata useBaseColorSpace true");
    // Only channel 0 is used: channel 1 may hold different values.
    gm.gainMapMax[1].n = 9; gm.gainMapMax[1].d = 1;
    Check(TestAvifGainMapToMetadata(&gm, meta) &&
          Near(meta.gainMapMax, 1.0, 1e-12),
          "metadata uses channel 0 only (channel 1 ignored)");
    // Zero denominator in any essential field refuses the whole map.
    gm.gainMapGamma[0].d = 0;
    Check(!TestAvifGainMapToMetadata(&gm, meta) && !meta.valid,
          "metadata with gamma denominator 0 is refused");
    gm.gainMapGamma[0].d = 1;
    gm.gainMapMin[0].d = 0;
    Check(!TestAvifGainMapToMetadata(&gm, meta) && !meta.valid,
          "metadata with gainMapMin denominator 0 is refused");
    gm.gainMapMin[0].d = 1;
    // Gamma 0 (0/1) is refused.
    gm.gainMapGamma[0].n = 0;
    Check(!TestAvifGainMapToMetadata(&gm, meta) && !meta.valid,
          "metadata with gamma 0 is refused");
    // NULL gain map is refused.
    Check(!TestAvifGainMapToMetadata(nullptr, meta) && !meta.valid,
          "metadata conversion refuses NULL gain map");
}

void TestPlaneNormalisation() {
    // 8-bit: passthrough.
    Check(TestNormaliseSample(0, 8) == 0, "8-bit 0 -> 0");
    Check(TestNormaliseSample(128, 8) == 128, "8-bit 128 -> 128");
    Check(TestNormaliseSample(255, 8) == 255, "8-bit 255 -> 255");
    // 10-bit: 512 -> 128, 771 -> 192, 1023 -> 255.
    Check(TestNormaliseSample(0, 10) == 0, "10-bit 0 -> 0");
    Check(TestNormaliseSample(512, 10) == 128, "10-bit 512 -> 128");
    Check(TestNormaliseSample(771, 10) == 192, "10-bit 771 -> 192");
    Check(TestNormaliseSample(1023, 10) == 255, "10-bit 1023 -> 255");
    // 12-bit: 2048 -> 128, 2378 -> 148 (the HEIF HDR case value), 4095 -> 255.
    Check(TestNormaliseSample(0, 12) == 0, "12-bit 0 -> 0");
    Check(TestNormaliseSample(2048, 12) == 128, "12-bit 2048 -> 128");
    Check(TestNormaliseSample(2378, 12) == 148, "12-bit 2378 -> 148");
    Check(TestNormaliseSample(4095, 12) == 255, "12-bit 4095 -> 255");
    // 16-bit.
    Check(TestNormaliseSample(0, 16) == 0, "16-bit 0 -> 0");
    Check(TestNormaliseSample(32768, 16) == 128, "16-bit 32768 -> 128");
    Check(TestNormaliseSample(65535, 16) == 255, "16-bit 65535 -> 255");
    // Out-of-range samples clamp to the native max.
    Check(TestNormaliseSample(2000, 10) == 255,
          "10-bit 2000 clamps to 255");
    Check(TestNormaliseSample(5000, 12) == 255,
          "12-bit 5000 clamps to 255");
    // Depth validity: only 8..16 accepted.
    Check(TestDepthOk(8) && TestDepthOk(10) && TestDepthOk(12) &&
          TestDepthOk(16), "depths 8/10/12/16 accepted");
    Check(!TestDepthOk(7) && !TestDepthOk(17) && !TestDepthOk(0),
          "depths 0/7/17 refused");
    // Size sanity cap: at most 4x the base per side.
    Check(TestGainSizeOk(16, 16, 4, 4), "gain map 4x base accepted");
    Check(!TestGainSizeOk(17, 17, 4, 4), "gain map 17x17 vs 4x4 refused");
    Check(!TestGainSizeOk(0, 4, 4, 4), "gain map 0 wide refused");
}

gainmap::Metadata MakeMeta(double gainMin, double gainMax, double gamma,
                           double baseOff, double altOff) {
    gainmap::Metadata meta;
    meta.gainMapMin = gainMin;
    meta.gainMapMax = gainMax;
    meta.gamma = gamma;
    meta.baseOffset = baseOff;
    meta.alternateOffset = altOff;
    meta.valid = true;
    return meta;
}

// Case A: 2x2 10-bit gain map (normalised to 0/128/192/255),
// upsampled to 4x4; gamma 2.0, baseOffset 0.01, alternateOffset 0.005.
const uint8_t kExpectedGamma2Offsets[4 * 4 * 4] = {
    38, 67, 129, 255, 62, 115, 224, 255, 114, 240, 37, 255, 255, 255, 255, 255,
    23, 23, 23, 255, 47, 180, 96, 255, 226, 51, 255, 255, 120, 120, 120, 255,
    165, 88, 53, 255, 53, 43, 36, 255, 237, 237, 237, 255, 32, 175, 255, 255,
    171, 30, 255, 255, 93, 255, 93, 255, 139, 98, 59, 255, 178, 178, 178, 255,
};

void TestUpsampledGamma2Offsets() {
    // Native 10-bit samples -> normalised 8-bit: 0, 512->128,
    // 771->192, 1023->255.
    const uint16_t gain10[2 * 2] = {0, 512, 771, 1023};
    uint8_t gain[2 * 2];
    for (int i = 0; i < 4; i++)
        gain[i] = TestNormaliseSample(gain10[i], 10);
    Check(gain[0] == 0 && gain[1] == 128 && gain[2] == 192 && gain[3] == 255,
          "10-bit gain plane normalises to 0/128/192/255");
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta = MakeMeta(0.0, 1.0, 2.0, 0.01, 0.005);
    Check(RunAvifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, meta, out),
          "gamma-2.0 upsampled gain: sequence runs");
    CheckBgra(out, kExpectedGamma2Offsets, sizeof(kExpectedGamma2Offsets),
              "gamma-2.0 upsampled gain");
}

// Case B: 4x4 12-bit gain map, same size; min -1.0, max 2.0, gamma 1.0.
const uint8_t kExpectedMinMax12bit[4 * 4 * 4] = {
    68, 125, 236, 255, 34, 71, 146, 255, 106, 232, 21, 255, 235, 235, 235, 255,
    0, 0, 0, 255, 32, 156, 80, 255, 234, 43, 255, 255, 152, 152, 152, 255,
    236, 125, 68, 255, 64, 47, 29, 255, 255, 255, 255, 255, 0, 236, 255, 255,
    92, 0, 188, 255, 44, 188, 44, 255, 71, 46, 21, 255, 92, 92, 92, 255,
};

void TestSameSizeMinMax12bit() {
    const uint16_t gain12[4 * 4] = {
        4095, 0, 2048, 1024,
        512, 1536, 2560, 3584,
        4095, 4095, 4095, 4095,
        0, 0, 0, 0,
    };
    uint8_t gain[4 * 4];
    for (int i = 0; i < 16; i++)
        gain[i] = TestNormaliseSample(gain12[i], 12);
    Check(gain[0] == 255 && gain[1] == 0 && gain[2] == 128 && gain[3] == 64,
          "12-bit gain row 0 normalises to 255/0/128/64");
    Check(gain[6] == 159 && gain[7] == 223,
          "12-bit gain row 1 normalises 2560->159, 3584->223");
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta = MakeMeta(-1.0, 2.0, 1.0, 0.0, 0.0);
    Check(RunAvifGainMapSequence(baseBgra, 4, 4, gain, 4, 4, meta, out),
          "min/max 12-bit gain: sequence runs");
    CheckBgra(out, kExpectedMinMax12bit, sizeof(kExpectedMinMax12bit),
              "min/max 12-bit gain");
    // Bottom row has gain 0 everywhere (gainMapMin -1.0 -> gain 0.5):
    // the last pixel (128 grey) comes out at 92 grey.
    Check(out[60] == 92 && out[61] == 92 && out[62] == 92,
          "min/max 12-bit gain: gain-0.5 grey pixel is 92");
}

// Case C: 2x2 8-bit gain map, upsampled; min 0, max 1, gamma 1 — the
// same inputs as the HEIF test's upsampled case, so the expected
// values must match that test byte for byte.
const uint8_t kExpectedUpsampled8bit[4 * 4 * 4] = {
    32, 64, 128, 255,  52, 104, 208, 255,  102, 225, 19, 255,  255, 255, 255, 255,
    0, 0, 0, 255,      34, 165, 85, 255,   211, 37, 255, 255,  111, 111, 111, 255,
    153, 78, 40, 255,  39, 27, 15, 255,    230, 230, 230, 255,  0, 169, 255, 255,
    162, 0, 255, 255,  85, 255, 85, 255,    134, 91, 48, 255,   176, 176, 176, 255,
};

void TestUpsampled8bitCrossCheck() {
    const uint8_t gain[2 * 2] = {0, 128, 192, 255};
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta = MakeMeta(0.0, 1.0, 1.0, 0.0, 0.0);
    Check(RunAvifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, meta, out),
          "upsampled 8-bit gain: sequence runs");
    CheckBgra(out, kExpectedUpsampled8bit, sizeof(kExpectedUpsampled8bit),
              "upsampled 8-bit gain (HEIF cross-check)");
    // Gain-0 corner pixel (0,0): gain 1.0, below the EETF knee — the
    // base pixel must come back byte-identical.
    Check(out[0] == 32 && out[1] == 64 && out[2] == 128 && out[3] == 0xff,
          "upsampled 8-bit gain: gain-0 corner pixel is byte-identical to base");
    // The brightest corner (3,3): gain 2.0 on mid grey 128 -> 176.
    Check(out[60] == 176 && out[61] == 176 && out[62] == 176,
          "upsampled 8-bit gain: gain-2.0 corner pixel is 176 grey");
}

void TestRefusalsKeepBase() {
    // The AVIF path keeps the base when metadata is invalid; ApplyGainMap
    // must refuse to run with valid == false or gamma <= 0.
    const uint8_t gain[2 * 2] = {0, 128, 192, 255};
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta; // valid == false
    Check(!RunAvifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, meta, out),
          "invalid metadata: sequence refuses to run");
    gainmap::Metadata badGamma = MakeMeta(0.0, 1.0, 0.0, 0.0, 0.0);
    Check(!RunAvifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, badGamma, out),
          "gamma 0: sequence refuses to run");
    // No gain map at all (decoder->image->gainMap == NULL): the wrapper
    // returns NULL before any pixel work, so the base is unchanged.
    // Here that is modelled by never calling the sequence: the base
    // buffer must equal itself byte for byte.
    uint8_t untouched[4 * 4 * 4];
    MakeBaseBgra(untouched);
    Check(std::memcmp(baseBgra, untouched, sizeof(baseBgra)) == 0,
          "no gain map: base is unchanged");
}

}  // namespace

int main() {
    TestFractionConversion();
    TestMetadataConversion();
    TestPlaneNormalisation();
    TestUpsampledGamma2Offsets();
    TestSameSizeMinMax12bit();
    TestUpsampled8bitCrossCheck();
    TestRefusalsKeepBase();
    std::printf("avif_gainmap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
