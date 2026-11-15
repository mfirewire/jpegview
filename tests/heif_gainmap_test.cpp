// heif_gainmap_test.cpp - unit test for the HEIF gain-map path
// (Option D, Phase 3: HEIFWrapper.cpp TryApplyHeifGainMap).
//
// What this pins, and what it does not:
// --------------------------------------
// The HEIF gain-map path itself cannot be tested end-to-end here: there
// is no real iPhone HEIF gain-map sample on hand, and decoding a HEIF
// needs libheif + a real file. What this test pins is the exact APPLY
// sequence the HEIF path runs on the decoded pixels, with synthetic
// data:
//
//   base BGRA (as pPixelData holds it after the SDR decode)
//     -> RGB temp buffer (drop alpha)
//     -> gainmap::ApplyGainMap (bilinear-upsampled gain map, weight 1.0)
//     -> HdrToneMap::ConvertLinearImage (linear 1.0 = SDR white)
//     -> repack BGR -> BGRA (alpha 0xff)
//
// Expected outputs were computed independently in Python from the ISO
// 21496-1 formula and the HdrToneMap replication in
// ~/workspace/gainmap_reference.py (reconstruct + tonemap_linear, with
// the upsampled gain map rounded via lround as BilinearUpsampleGray
// does). Bytes are compared with a +/-1 tolerance for float-vs-double
// rounding at byte boundaries; the gain-0 corner pixel is asserted
// exactly (gain 1.0 there, below the EETF knee, so it must come back
// byte-identical to the base).
//
// It also checks the two XMP inputs the HEIF path feeds into
// gainmap::ParseXmpMetadata (a hdrgm packet and an Apple HDRGainMap
// packet) — the parser itself is covered in depth by gainmap_test.cpp;
// here only the values the HEIF path depends on are pinned.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/heif_gainmap_test.cpp src/JPEGView/GainMap.cpp
//       src/JPEGView/HdrToneMap.cpp -o /tmp/heif_gainmap_test
//   /tmp/heif_gainmap_test

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "GainMap.h"
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

// Run the exact sequence TryApplyHeifGainMap runs, on synthetic buffers:
// `baseBgra` is width*height*4 (B,G,R,A), `gainGray` is gainW*gainH.
// Output is width*height*4 BGRA with alpha 0xff. Returns false when
// ApplyGainMap refuses the input (mirrors the HEIF path's "keep the
// base" outcome).
bool RunHeifGainMapSequence(const uint8_t* baseBgra, int width, int height,
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

// Synthetic 4x4 SDR base, RGB row-major.
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

gainmap::Metadata MakeMeta() {
    gainmap::Metadata meta;
    meta.gainMapMin = 0.0;
    meta.gainMapMax = 1.0;
    meta.gamma = 1.0;
    meta.baseOffset = 0.0;
    meta.alternateOffset = 0.0;
    meta.valid = true;
    return meta;
}

void TestUpsampledGainMap() {
    // 2x2 gain map, bilinear-upsampled to 4x4 by ApplyGainMap.
    const uint8_t gain[2 * 2] = {0, 128, 192, 255};
    const uint8_t expected[4 * 4 * 4] = {
        32, 64, 128, 255,  52, 104, 208, 255,  102, 225, 19, 255,  255, 255, 255, 255,
        0, 0, 0, 255,      34, 165, 85, 255,   211, 37, 255, 255,  111, 111, 111, 255,
        153, 78, 40, 255,  39, 27, 15, 255,    230, 230, 230, 255,  0, 169, 255, 255,
        162, 0, 255, 255,  85, 255, 85, 255,    134, 91, 48, 255,   176, 176, 176, 255,
    };
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta = MakeMeta();
    Check(RunHeifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, meta, out),
          "upsampled gain: sequence runs");
    CheckBgra(out, expected, sizeof(expected), "upsampled gain");
    // Gain-0 corner pixel (0,0): gain 1.0, below the EETF knee — the
    // base pixel must come back byte-identical.
    Check(out[0] == 32 && out[1] == 64 && out[2] == 128 && out[3] == 0xff,
          "upsampled gain: gain-0 corner pixel is byte-identical to base");
    // The brightest corner (3,3): gain 2.0 on mid grey 128 -> 176.
    Check(out[60] == 176 && out[61] == 176 && out[62] == 176,
          "upsampled gain: gain-2.0 corner pixel is 176 grey");
}

void TestSameSizeGainMap() {
    // 4x4 gain map, no upsampling.
    const uint8_t gain[4 * 4] = {
        255, 0, 128, 64,
        32, 96, 160, 224,
        255, 255, 255, 255,
        0, 0, 0, 0,
    };
    const uint8_t expected[4 * 4 * 4] = {
        47, 90, 176, 255,   50, 100, 200, 255,  106, 233, 21, 255,  255, 255, 255, 255,
        0, 0, 0, 255,       35, 169, 87, 255,   219, 39, 255, 255,  120, 120, 120, 255,
        176, 90, 47, 255,   45, 31, 18, 255,    245, 245, 245, 255,  0, 176, 255, 255,
        128, 0, 252, 255,   64, 252, 64, 255,   99, 66, 33, 255,    128, 128, 128, 255,
    };
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta = MakeMeta();
    Check(RunHeifGainMapSequence(baseBgra, 4, 4, gain, 4, 4, meta, out),
          "same-size gain: sequence runs");
    CheckBgra(out, expected, sizeof(expected), "same-size gain");
    // Bottom row has gain 0 everywhere: those pixels are the base,
    // tone-mapped with the content peak — the last pixel (128 grey)
    // stays 128 grey.
    Check(out[60] == 128 && out[61] == 128 && out[62] == 128,
          "same-size gain: gain-0 grey pixel stays 128");
}

void TestInvalidMetadataRefused() {
    // The HEIF path keeps the base when metadata is invalid; ApplyGainMap
    // must refuse to run with valid == false.
    const uint8_t gain[2 * 2] = {0, 128, 192, 255};
    uint8_t baseBgra[4 * 4 * 4];
    uint8_t out[4 * 4 * 4];
    MakeBaseBgra(baseBgra);
    gainmap::Metadata meta; // valid == false
    Check(!RunHeifGainMapSequence(baseBgra, 4, 4, gain, 2, 2, meta, out),
          "invalid metadata: sequence refuses to run");
}

// Synthetic hdrgm XMP packet, plain single-value form — the shape the
// HEIF path reads from a "mime" metadata block (application/rdf+xml).
const char* kXmpHdrgm =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" "
    "xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
    "<hdrgm:GainMapMax>1.0</hdrgm:GainMapMax>"
    "<hdrgm:Gamma>1.0</hdrgm:Gamma>"
    "<hdrgm:OffsetSDR>0.0</hdrgm:OffsetSDR>"
    "<hdrgm:OffsetHDR>0.0</hdrgm:OffsetHDR>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

// Synthetic Apple HDRGainMap XMP packet (mapping UNVERIFIED — see
// GainMap.h; no Apple sample was on hand).
const char* kXmpApple =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<HDRGainMapHeadroom>2.0</HDRGainMapHeadroom>"
    "<HDRGainMapGamma>1.0</HDRGainMapGamma>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

void TestXmpInputs() {
    using namespace gainmap;
    Metadata m;
    Check(ParseXmpMetadata(std::string(kXmpHdrgm), m) && m.valid,
          "XMP hdrgm (HEIF path input) parses");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "XMP hdrgm gainMapMin 0.0");
    Check(Near(m.gainMapMax, 1.0, 1e-12), "XMP hdrgm gainMapMax 1.0");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP hdrgm gamma 1.0");
    Check(!m.appleHeadroomFallback, "XMP hdrgm is not Apple fallback");
    Check(ParseXmpMetadata(std::string(kXmpApple), m) && m.valid,
          "XMP Apple (HEIF path input) parses");
    Check(m.appleHeadroomFallback, "XMP Apple sets appleHeadroomFallback");
    Check(Near(m.gainMapMax, 2.0, 1e-12), "XMP Apple gainMapMax = headroom 2.0");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "XMP Apple gainMapMin 0.0");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP Apple gamma 1.0");
    // A packet with neither scheme parses to nothing.
    Metadata bad;
    Check(!ParseXmpMetadata(std::string("<x:xmpmeta></x:xmpmeta>"), bad)
          && !bad.valid, "XMP empty packet does not parse");
}

}  // namespace

int main() {
    TestUpsampledGainMap();
    TestSameSizeGainMap();
    TestInvalidMetadataRefused();
    TestXmpInputs();
    std::printf("heif_gainmap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
