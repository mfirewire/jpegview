// psd_hdr_test.cpp - unit test for the PSD HDR decode core
// (PsdHdr.h/.cpp: 16/32-bit PSD image-data decode to linear float RGB).
//
// What this pins, and what it does not:
// --------------------------------------
// The core's constants and pipeline steps are pinned against values
// computed independently in Python (see
// tests/samples/make_psd_hdr_samples.py, which also generated the
// fixture files and printed the expected BGR tables below):
//   * the 16-bit sRGB EOTF (SrgbEotf16) at known codes, including a
//     code just above the 0.04045 linear/power threshold;
//   * the 32-bit sanitize (NaN/negative -> 0, +Inf -> 1e6);
//   * the ZIP-prediction delta reversal, including wraparound;
//   * the alpha byte conversion for both depths;
//   * the full decode over all 13 synthetic PSD/PSB fixtures (raw,
//     RLE, ZIP, ZIP-with-prediction; RGB, grey, RGBA; PSD v1 and PSB
//     v2), followed by HdrToneMap::ConvertLinearImage, compared
//     against the Python-reference BGR values (tolerance +/-2, the
//     same band hdr_tonemap_test.cpp uses for mid-tones; the C++
//     linear buffer is float, the Python reference is double).
//   * rejection of unsupported/corrupt input (8-bit, CMYK/Lab, bad
//     compression/version/dimensions, truncated data, a PackBits run
//     crossing a row boundary, a truncated zlib stream).
//   * H-02 (non-sRGB 16-bit PSD): PsdIcc::BuildColorSpace on
//     lcms2-built Adobe RGB / ProPhoto / sRGB / gray profiles (the
//     profiles are built on the fly in the test, no binary ICC blobs),
//     PsdHdr::DecodeImageData with a hand-built ColorSpace (gamma
//     TRC + primaries matrix, verified against values computed
//     independently in Python), and the three H-02 fixtures
//     (Adobe RGB / ProPhoto / Adobe gray, 16-bit, 0x040F resource)
//     decoded end-to-end through BuildColorSpace + DecodeImageData +
//     HdrToneMap::ConvertLinearImage.
//
// The highlight roll-off is pinned explicitly in TestRollOff: with a
// content peak of 16.0, linear 1.0 -> 211, 2.0 -> 235, 4.0 -> 249,
// 8.0 -> 254, 16.0 -> 255 (the same shape as the .hdr/EXR
// verification), and SDR-range mid-tones agree with the direct sRGB
// encoding (0.18 -> 118, 0.5 -> 188 at peak 1.0).
//
// This test is pure math + file bytes: it needs no image files beyond
// the fixtures in tests/samples/ and no Windows toolchain. It does not
// verify the PSDWrapper wiring (a later phase) or any GUI behaviour.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       -I src/JPEGView/libpng-apng/include
//       tests/psd_hdr_test.cpp src/JPEGView/PsdHdr.cpp
//       src/JPEGView/PsdIcc.cpp src/JPEGView/HdrToneMap.cpp
//       -lz -llcms2 -o /tmp/psd_hdr_test
//   /tmp/psd_hdr_test
// The vendored include dir is passed so the test proves the vendored
// zlib 1.3.2 headers compile; it links the system -lz (inflateInit_
// only checks version[0], and the headers match the linked 1.3.2
// zlib.lib on Windows exactly). lcms2 needs its dev package
// (liblcms2-dev: /usr/include/lcms2.h and -llcms2); without it, add
// -I src/JPEGView/lcms2/include and link the runtime library
// directly with -l:liblcms2.so.2 instead of -llcms2.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "HdrToneMap.h"
#include "PsdHdr.h"
#include "PsdIcc.h"
#include "lcms2.h"

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

// ---------------------------------------------------------------- helpers

uint16_t ReadBe16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t ReadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Minimal PSD parser for the fixtures: returns the image-data section
// (bytes after the 2-byte compression field) and the header fields the
// core needs. Exits the test with a failure if the file is malformed.
struct PsdFile {
    std::vector<uint8_t> imageData;
    int version = 0;
    int channels = 0;
    int width = 0;
    int height = 0;
    int bitDepth = 0;
    int colorMode = 0;
    int compression = 0;
};

bool LoadPsd(const char* path, PsdFile* out) {
    FILE* f = std::fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 26 + 4 + 4 + 4 + 2) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> data((size_t)size);
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    if (std::memcmp(data.data(), "8BPS", 4) != 0) {
        return false;
    }
    out->version = ReadBe16(data.data() + 4);
    out->channels = ReadBe16(data.data() + 12);
    out->height = (int)ReadBe32(data.data() + 14);
    out->width = (int)ReadBe32(data.data() + 18);
    out->bitDepth = ReadBe16(data.data() + 22);
    out->colorMode = ReadBe16(data.data() + 24);
    size_t pos = 26;
    for (int i = 0; i < 2; i++) {
        if (pos + 4 > data.size()) {
            return false;
        }
        uint32_t n = ReadBe32(data.data() + pos);
        pos += 4 + n;
        if (pos > data.size()) {
            return false;
        }
    }
    size_t layerLenSize = (out->version == 2) ? (size_t)8 : (size_t)4;
    if (pos + layerLenSize > data.size()) {
        return false;
    }
    size_t layerLen = 0;
    for (size_t i = 0; i < layerLenSize; i++) {
        layerLen = (layerLen << 8) | data[pos + i];
    }
    pos += layerLenSize + layerLen;
    if (pos + 2 > data.size()) {
        return false;
    }
    out->compression = ReadBe16(data.data() + pos);
    pos += 2;
    out->imageData.assign(data.begin() + (long)pos, data.end());
    return true;
}

// Decode one fixture file, tone-map it, and compare the BGR output
// against the expected table (row-major, 3 bytes per pixel).
bool CheckFixture(const char* path, const uint8_t* expected, int expectedBytes) {
    PsdFile psd;
    if (!LoadPsd(path, &psd)) {
        std::printf("FAIL: cannot parse %s\n", path);
        g_failures++;
        g_checks++;
        return false;
    }
    size_t pixels = (size_t)psd.width * (size_t)psd.height;
    std::vector<float> linear(pixels * 3);
    std::vector<uint8_t> bgr(pixels * 3);
    bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                       psd.width, psd.height, psd.channels,
                                       psd.colorMode, psd.bitDepth,
                                       psd.compression, psd.version,
                                       linear.data(), NULL, 0, NULL);
    if (!ok) {
        std::printf("FAIL: DecodeImageData rejected %s\n", path);
        g_failures++;
        g_checks++;
        return false;
    }
    HdrToneMap::ConvertLinearImage(linear.data(), psd.width, psd.height,
                                    (size_t)psd.width * 3, bgr.data(),
                                    (size_t)psd.width * 3);
    bool match = true;
    for (int i = 0; i < expectedBytes; i++) {
        int diff = (int)bgr[(size_t)i] - (int)expected[i];
        if (diff < -2 || diff > 2) {
            match = false;
            std::printf("FAIL: %s byte %d: got %d, expected %d\n",
                        path, i, (int)bgr[(size_t)i], (int)expected[i]);
            break;
        }
    }
    g_checks++;
    if (!match) {
        g_failures++;
    }
    return match;
}

// ---------------------------------------------------------------- pins

void TestSrgbEotf16() {
    using namespace PsdHdr;
    // Python-computed sRGB EOTF values (make_psd_hdr_samples.py).
    Check(Near(SrgbEotf16(0), 0.0, 1e-12), "EOTF16 0 -> 0");
    Check(Near(SrgbEotf16(1), 1.1810388e-06, 1e-12), "EOTF16 1 -> 1.181e-06");
    // 1024/65535 = 0.015625, below the 0.04045 threshold: linear segment.
    Check(Near(SrgbEotf16(1024), 0.0012093838, 1e-10), "EOTF16 1024 -> 0.0012093838");
    // 2652/65535 = 0.040462, just above the threshold (2650.89): power.
    Check(Near(SrgbEotf16(2652), 0.0031321399, 1e-10), "EOTF16 2652 -> 0.0031321399");
    Check(Near(SrgbEotf16(4096), 0.0051557688, 1e-10), "EOTF16 4096 -> 0.0051557688");
    Check(Near(SrgbEotf16(32768), 0.2140482023, 1e-10), "EOTF16 32768 -> 0.2140482023");
    Check(Near(SrgbEotf16(49152), 0.5225393823, 1e-10), "EOTF16 49152 -> 0.5225393823");
    Check(Near(SrgbEotf16(65535), 1.0, 1e-12), "EOTF16 65535 -> 1");
    // Monotonic over the whole range.
    double prev = -1.0;
    bool ok = true;
    for (int v = 0; v <= 65535; v += 257) {
        double cur = SrgbEotf16((uint16_t)v);
        if (!(cur >= prev)) { ok = false; break; }
        prev = cur;
    }
    Check(ok, "EOTF16 monotonic");
}

void TestSanitizeFloat32() {
    using namespace PsdHdr;
    Check(SanitizeFloat32(2.5f) == 2.5f, "sanitize finite passes through");
    Check(SanitizeFloat32(0.0f) == 0.0f, "sanitize 0 passes through");
    Check(SanitizeFloat32(-1.0f) == 0.0f, "sanitize negative -> 0");
    Check(SanitizeFloat32(-1.0e30f) == 0.0f, "sanitize large negative -> 0");
    double nan = std::nan("");
    Check(SanitizeFloat32((float)nan) == 0.0f, "sanitize NaN -> 0");
    float inf = std::numeric_limits<float>::infinity();
    float clamped = SanitizeFloat32(inf);
    Check(clamped > 0.0f && clamped < std::numeric_limits<float>::infinity(),
          "sanitize +Inf -> large finite");
    Check(clamped == 1.0e6f, "sanitize +Inf -> 1e6");
}

void TestReverseDelta() {
    using namespace PsdHdr;
    {
        uint16_t row[4] = { 100, 200, 300, 400 };
        ReverseDelta16(row, 4);
        Check(row[0] == 100 && row[1] == 300 && row[2] == 600 && row[3] == 1000,
              "delta16 cumulative sum");
    }
    {
        // Wraparound: 60000 + 10000 = 70000 mod 65536 = 4464.
        uint16_t row[2] = { 60000, 10000 };
        ReverseDelta16(row, 2);
        Check(row[0] == 60000 && row[1] == 4464, "delta16 wraparound");
    }
    {
        // First sample of a row is stored as-is (delta from zero).
        uint16_t row[1] = { 12345 };
        ReverseDelta16(row, 1);
        Check(row[0] == 12345, "delta16 single sample unchanged");
    }
    {
        uint32_t row[3] = { 5, 7, 9 };
        ReverseDelta32(row, 3);
        Check(row[0] == 5 && row[1] == 12 && row[2] == 21,
              "delta32 cumulative sum");
    }
    {
        // Wraparound on the float bit patterns: 0xFFFFFFF0 + 0x20 = 0x10.
        uint32_t row[2] = { 0xFFFFFFF0u, 0x20u };
        ReverseDelta32(row, 2);
        Check(row[0] == 0xFFFFFFF0u && row[1] == 0x10u, "delta32 wraparound");
    }
}

void TestAlphaBytes() {
    using namespace PsdHdr;
    Check(FloatAlphaToByte(0.0f) == 0, "float alpha 0 -> 0");
    Check(FloatAlphaToByte(1.0f) == 255, "float alpha 1 -> 255");
    Check(FloatAlphaToByte(0.5f) == 128, "float alpha 0.5 -> 128");
    Check(FloatAlphaToByte(0.25f) == 64, "float alpha 0.25 -> 64");
    Check(FloatAlphaToByte(-1.0f) == 0, "float alpha negative -> 0");
    Check(FloatAlphaToByte(2.0f) == 255, "float alpha >1 -> 255");
    double nan = std::nan("");
    Check(FloatAlphaToByte((float)nan) == 0, "float alpha NaN -> 0");
    Check(U16AlphaToByte(0) == 0, "u16 alpha 0 -> 0");
    Check(U16AlphaToByte(65535) == 255, "u16 alpha 65535 -> 255");
    Check(U16AlphaToByte(32768) == 128, "u16 alpha 32768 -> 128");
    Check(U16AlphaToByte(49152) == 191, "u16 alpha 49152 -> 191");
    Check(U16AlphaToByte(16384) == 64, "u16 alpha 16384 -> 64");
}

// ---------------------------------------------------------------- rejection

void TestRejections() {
    using namespace PsdHdr;
    // A minimal valid raw 16-bit 1x1 grey image-data section (2 bytes).
    const uint8_t raw16[2] = { 0x80, 0x00 };
    float out[3] = { -1.0f, -1.0f, -1.0f };
    Check(DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                          16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "valid 1x1 grey raw decodes");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           8, kCompressionRaw, 1, out, NULL, 0, NULL),
          "8-bit rejected (8-bit wrapper owns it)");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeCmyk,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "CMYK rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 3, kColorModeLab,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "Lab rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, 4, 1, out, NULL, 0, NULL),
          "compression 4 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 3, out, NULL, 0, NULL),
          "version 3 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 0, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "width 0 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 2, kColorModeRgb,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "RGB with 2 channels rejected");
    Check(!DecodeImageData(NULL, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "NULL src rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, NULL, NULL, 0, NULL),
          "NULL output rejected");
    // Truncated raw data (1 byte for a 2-byte sample).
    Check(!DecodeImageData(raw16, 1, 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0, NULL),
          "truncated raw rejected");
    // Alpha stride smaller than the width.
    uint8_t alpha[1] = { 0 };
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, alpha, 0, NULL),
          "alpha stride 0 rejected");
    (void)alpha;
    // RLE: a repeat run crossing the row boundary (1x1 grey, 2 bytes
    // per row; header 0xFE = repeat 3 copies of the next byte).
    // Table (1 row x 1 channel, 2 bytes) + 2 PackBits bytes.
    const uint8_t rleCross[4] = { 0x00, 0x02, 0xFE, 0x80 };
    Check(!DecodeImageData(rleCross, sizeof(rleCross), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionRle, 1,
                           out, NULL, 0, NULL),
          "RLE run crossing row boundary rejected");
    // RLE: truncated byte-count table (declares 1 row but no table).
    const uint8_t rleNoTable[1] = { 0x00 };
    Check(!DecodeImageData(rleNoTable, sizeof(rleNoTable), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionRle, 1,
                           out, NULL, 0, NULL),
          "RLE truncated table rejected");
    // ZIP: truncated zlib stream (first bytes of a real stream for
    // 2 zero bytes, cut short).
    const uint8_t zipTrunc[3] = { 0x78, 0x9C, 0x63 };
    Check(!DecodeImageData(zipTrunc, sizeof(zipTrunc), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionZip, 1,
                           out, NULL, 0, NULL),
          "truncated ZIP rejected");
    // ZIP: garbage that is not a zlib stream at all.
    const uint8_t zipGarbage[4] = { 0x00, 0x01, 0x02, 0x03 };
    Check(!DecodeImageData(zipGarbage, sizeof(zipGarbage), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionZip, 1,
                           out, NULL, 0, NULL),
          "garbage ZIP rejected");
}

// ---------------------------------------------------------------- fixtures

// Expected BGR (JPEGView order), row-major, from
// tests/samples/make_psd_hdr_samples.py (Python reference). The 16-bit
// files share one table (peak 1.0); the 32-bit files share another
// (peak 16.0). Row 0 is the grey ramp, row 1 the colour ramp
// (R=level, G=0.18, B=0.5); grey files repeat the grey ramp on row 1.
const uint8_t kExpected16Rgb[5 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
    188, 118, 0,  188, 118, 118,  188, 118, 188,  188, 118, 225,  188, 118, 255,
};
const uint8_t kExpected16Gray[5 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
};
const uint8_t kExpected32Rgb[9 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  177, 177, 177,  198, 198, 198,  211, 211, 211,
    235, 235, 235,  249, 249, 249,  254, 254, 254,  255, 255, 255,
    177, 118, 0,  177, 118, 118,  177, 118, 177,  177, 118, 198,  177, 118, 211,
    177, 118, 235,  177, 118, 249,  177, 118, 254,  177, 118, 255,
};
const uint8_t kExpected32Gray[9 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  177, 177, 177,  198, 198, 198,  211, 211, 211,
    235, 235, 235,  249, 249, 249,  254, 254, 254,  255, 255, 255,
    0, 0, 0,  118, 118, 118,  177, 177, 177,  198, 198, 198,  211, 211, 211,
    235, 235, 235,  249, 249, 249,  254, 254, 254,  255, 255, 255,
};

// H-02 fixtures (16-bit, 5x2, same grey/colour-ramp layout, but the
// stored samples are gamma-encoded in the file's working space and
// the file carries an ICC profile as the 0x040F resource). Expected
// BGR from tests/samples/make_psd_hdr_samples.py (Python reference:
// profile gamma TRC + primaries matrix + HdrToneMap, all double).
// The grey rows match the sRGB table (a grey maps to itself under
// either matrix, rows summing to ~1); the colour rows are where the
// primaries conversion shows: Adobe B at (2,1) is 208, not 188, and
// ProPhoto's wide gamut pushes several channels out of sRGB range
// (clamped to 0/255 by the tone mapper).
const uint8_t kExpectedH02AdobeRgb[5 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
    190, 118, 0,  190, 118, 118,  190, 118, 208,  190, 118, 252,  190, 118, 255,
};
const uint8_t kExpectedH02ProPhoto[5 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
    196, 129, 0,  196, 117, 81,  196, 92, 222,  195, 62, 255,  195, 0, 255,
};
const uint8_t kExpectedH02AdobeGray[5 * 2 * 3] = {
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
    0, 0, 0,  118, 118, 118,  188, 188, 188,  225, 225, 225,  255, 255, 255,
};

void TestFixtures() {
    CheckFixture("tests/samples/psd_hdr_16bit_rgb_raw.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_16bit_rgb_rle.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_16bit_rgb_zip.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_16bit_rgb_zippred.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_16bit_gray_raw.psd",
                 kExpected16Gray, sizeof(kExpected16Gray));
    CheckFixture("tests/samples/psd_hdr_16bit_rgba_raw.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_16bit_rgb_rle_psb.psd",
                 kExpected16Rgb, sizeof(kExpected16Rgb));
    CheckFixture("tests/samples/psd_hdr_32bit_rgb_raw.psd",
                 kExpected32Rgb, sizeof(kExpected32Rgb));
    CheckFixture("tests/samples/psd_hdr_32bit_rgb_rle.psd",
                 kExpected32Rgb, sizeof(kExpected32Rgb));
    CheckFixture("tests/samples/psd_hdr_32bit_rgb_zip.psd",
                 kExpected32Rgb, sizeof(kExpected32Rgb));
    CheckFixture("tests/samples/psd_hdr_32bit_rgb_zippred.psd",
                 kExpected32Rgb, sizeof(kExpected32Rgb));
    CheckFixture("tests/samples/psd_hdr_32bit_gray_raw.psd",
                 kExpected32Gray, sizeof(kExpected32Gray));
    CheckFixture("tests/samples/psd_hdr_32bit_rgba_raw.psd",
                 kExpected32Rgb, sizeof(kExpected32Rgb));
}

// The alpha planes of the RGBA fixtures: 16-bit alpha ramp
// 1.0/0.75/0.5/0.25/0.0 -> 255/191/128/64/0; 32-bit the same.
void TestFixtureAlpha() {
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_16bit_rgba_raw.psd", &psd),
              "parse 16-bit RGBA fixture");
        std::vector<float> linear(5 * 2 * 3);
        uint8_t alpha[5 * 2] = { 0 };
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), alpha, 5, NULL);
        Check(ok, "decode 16-bit RGBA fixture with alpha");
        const uint8_t expected[10] = { 255, 191, 128, 64, 0, 255, 191, 128, 64, 0 };
        Check(std::memcmp(alpha, expected, sizeof(expected)) == 0,
              "16-bit RGBA alpha ramp 255/191/128/64/0");
    }
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_32bit_rgba_raw.psd", &psd),
              "parse 32-bit RGBA fixture");
        std::vector<float> linear(9 * 2 * 3);
        uint8_t alpha[9 * 2] = { 0 };
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), alpha, 9, NULL);
        Check(ok, "decode 32-bit RGBA fixture with alpha");
        const uint8_t expected[18] = { 255, 191, 128, 64, 0, 0, 0, 0, 0,
                                       255, 191, 128, 64, 0, 0, 0, 0, 0 };
        Check(std::memcmp(alpha, expected, sizeof(expected)) == 0,
              "32-bit RGBA alpha ramp 255/191/128/64/0");
    }
}

// P3-D3: wrapper-order simulation test (NOT a wrapper test: the
// wrapper is MFC-coupled and cannot compile in this harness, and
// the header disclaimer above stands). It simulates the PSDWrapper
// HDR order - DecodeImageData -> ConvertLinearImageToBgra - and
// asserts the final BGRA bytes, including a half-transparent
// pixel. The output is pre-filled with a sentinel so a byte the
// variant fails to write (H-01/N-3) fails the test instead of
// passing silently.
void TestWrapperOrderSimulation() {
    const uint8_t kAlphaRamp[10] = { 255, 191, 128, 64, 0,
                                     255, 191, 128, 64, 0 };
    // 4-channel: 16-bit RGBA fixture, planar alpha (stride 1).
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_16bit_rgba_raw.psd", &psd),
              "simulation: parse 16-bit RGBA fixture");
        std::vector<float> linear(5 * 2 * 3);
        uint8_t alpha[5 * 2] = { 0 };
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), alpha, 5, NULL);
        Check(ok, "simulation: decode 16-bit RGBA fixture with alpha");
        uint8_t bgra[5 * 2 * 4];
        std::memset(bgra, 0xCD, sizeof(bgra));
        HdrToneMap::ConvertLinearImageToBgra(linear.data(), 5, 2, 15,
                                             bgra, 20, alpha, 1);
        bool match = true;
        for (int i = 0; i < 10; i++) {
            for (int c = 0; c < 3; c++) {
                int diff = (int)bgra[(size_t)i * 4 + c] - (int)kExpected16Rgb[(size_t)i * 3 + c];
                if (diff < -2 || diff > 2) match = false;
            }
            if (bgra[(size_t)i * 4 + 3] != kAlphaRamp[i]) match = false;
        }
        Check(match, "simulation: RGBA fixture -> BGRA (BGR +/-2, alpha ramp exact)");
        // Half-transparent pixel (index 2, alpha 128): pin its
        // BGRA explicitly, not just through the loop above.
        Check(bgra[2 * 4 + 3] == 128 &&
              std::abs((int)bgra[2 * 4 + 0] - (int)kExpected16Rgb[2 * 3 + 0]) <= 2 &&
              std::abs((int)bgra[2 * 4 + 1] - (int)kExpected16Rgb[2 * 3 + 1]) <= 2 &&
              std::abs((int)bgra[2 * 4 + 2] - (int)kExpected16Rgb[2 * 3 + 2]) <= 2,
              "simulation: half-transparent pixel -> BGRA with alpha 128");
    }
    // 3-channel: 16-bit RGB fixture, NULL alpha -> every alpha byte
    // is 0xff (opaque), the wrapper stores it as 4-channel BGRA.
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_16bit_rgb_raw.psd", &psd),
              "simulation: parse 16-bit RGB fixture");
        std::vector<float> linear(5 * 2 * 3);
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), NULL, 0, NULL);
        Check(ok, "simulation: decode 16-bit RGB fixture");
        uint8_t bgra[5 * 2 * 4];
        std::memset(bgra, 0xCD, sizeof(bgra));
        HdrToneMap::ConvertLinearImageToBgra(linear.data(), 5, 2, 15,
                                             bgra, 20, NULL, 0);
        bool match = true;
        for (int i = 0; i < 10; i++) {
            for (int c = 0; c < 3; c++) {
                int diff = (int)bgra[(size_t)i * 4 + c] - (int)kExpected16Rgb[(size_t)i * 3 + c];
                if (diff < -2 || diff > 2) match = false;
            }
            if (bgra[(size_t)i * 4 + 3] != 0xff) match = false;
        }
        Check(match, "simulation: RGB fixture, NULL alpha -> BGRA with alpha 0xff");
    }
}

// The decoded linear floats themselves (not just the tone-mapped
// bytes): 32-bit samples must come back bit-exact as float32, 16-bit
// samples within 1e-4 of the intended linear level.
void TestLinearValues() {
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_32bit_rgb_raw.psd", &psd),
              "parse 32-bit RGB fixture (linear check)");
        std::vector<float> linear(9 * 2 * 3);
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), NULL, 0, NULL);
        Check(ok, "decode 32-bit RGB fixture (linear check)");
        const float levels[9] = { 0.0f, 0.18f, 0.5f, 0.75f, 1.0f,
                                  2.0f, 4.0f, 8.0f, 16.0f };
        bool match = true;
        for (int x = 0; x < 9; x++) {
            for (int c = 0; c < 3; c++) {
                if (linear[(size_t)x * 3 + (size_t)c] != levels[x]) {
                    match = false;
                }
            }
        }
        Check(match, "32-bit grey row linear values bit-exact");
    }
    {
        PsdFile psd;
        Check(LoadPsd("tests/samples/psd_hdr_16bit_rgb_raw.psd", &psd),
              "parse 16-bit RGB fixture (linear check)");
        std::vector<float> linear(5 * 2 * 3);
        bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                           psd.width, psd.height, psd.channels,
                                           psd.colorMode, psd.bitDepth,
                                           psd.compression, psd.version,
                                           linear.data(), NULL, 0, NULL);
        Check(ok, "decode 16-bit RGB fixture (linear check)");
        const double levels[5] = { 0.0, 0.18, 0.5, 0.75, 1.0 };
        bool match = true;
        for (int x = 0; x < 5; x++) {
            for (int c = 0; c < 3; c++) {
                if (!Near((double)linear[(size_t)x * 3 + (size_t)c], levels[x], 1e-4)) {
                    match = false;
                }
            }
        }
        Check(match, "16-bit grey row linear values within 1e-4 of level");
    }
}

// Highlight roll-off, named explicitly (the fixture tables above also
// assert these values within +/-2): with a content peak of 16.0 the
// grey ramp must roll off instead of clipping, and the SDR-range
// mid-tones must agree with the direct sRGB encoding.
void TestRollOff() {
    PsdFile psd;
    Check(LoadPsd("tests/samples/psd_hdr_32bit_rgb_raw.psd", &psd),
          "parse 32-bit RGB fixture (roll-off)");
    std::vector<float> linear(9 * 2 * 3);
    bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                       psd.width, psd.height, psd.channels,
                                       psd.colorMode, psd.bitDepth,
                                       psd.compression, psd.version,
                                       linear.data(), NULL, 0, NULL);
    Check(ok, "decode 32-bit RGB fixture (roll-off)");
    std::vector<uint8_t> bgr(9 * 2 * 3);
    HdrToneMap::ConvertLinearImage(linear.data(), psd.width, psd.height,
                                    (size_t)psd.width * 3, bgr.data(),
                                    (size_t)psd.width * 3);
    // Grey row, pixel x: BGR triples, R channel at offset x*3+2.
    auto GreyR = [&bgr](int x) { return (int)bgr[(size_t)x * 3 + 2]; };
    Check(GreyR(4) >= 209 && GreyR(4) <= 213, "roll-off: linear 1.0 -> ~211");
    Check(GreyR(5) >= 233 && GreyR(5) <= 237, "roll-off: linear 2.0 -> ~235");
    Check(GreyR(6) >= 247 && GreyR(6) <= 251, "roll-off: linear 4.0 -> ~249");
    Check(GreyR(7) >= 252 && GreyR(7) <= 255, "roll-off: linear 8.0 -> ~254");
    Check(GreyR(8) == 255, "roll-off: linear 16.0 (peak) -> 255");
    Check(GreyR(1) >= 116 && GreyR(1) <= 120, "SDR: linear 0.18 -> ~118");
    // At peak 16 the 0.5 mid-tone is re-exposed darker than the direct
    // sRGB encoding (188): the BT.2390 behaviour, same as .hdr/EXR.
    Check(GreyR(2) >= 175 && GreyR(2) <= 179, "HDR: linear 0.5 -> ~177 at peak 16");
    std::printf("roll-off grey row: 0->%d 0.18->%d 0.5->%d 0.75->%d 1.0->%d "
                "2.0->%d 4.0->%d 8.0->%d 16.0->%d\n",
                GreyR(0), GreyR(1), GreyR(2), GreyR(3), GreyR(4),
                GreyR(5), GreyR(6), GreyR(7), GreyR(8));
}

// ---------------------------------------------------------------- H-02
// Non-sRGB 16-bit PSD: PsdIcc::BuildColorSpace + DecodeImageData with
// a ColorSpace + the H-02 fixtures.
//
// The expected matrices below come from an independent numpy
// computation from the xy primaries (see
// tests/samples/make_psd_hdr_samples.py, ADOBE_MATRIX /
// PROPHOTO_MATRIX, for the reproducible method: Bradford-adapt the
// primaries and the white point to D50, solve M*S = W, matrix =
// (D50 sRGB XYZ->RGB) * M*diag(S)). They are NOT copied from the
// C++ output: the C++ values are 1.398256 / 2.034174 in matrix[0]
// and agree with these to ~3e-4, well inside the 5e-3 band below.
// The wrong D65-matrix route would give 1.488 (Adobe) / 2.142
// (ProPhoto) in matrix[0]; the tests pin the D50 value and
// explicitly reject the D65 one for Adobe.
//
// Cross-check against lcms2's own transform (independent of both
// the numpy matrix and BuildColorSpace): an Adobe RGB encoded red
// of 0.5 maps to sRGB-encoded 0.5877 (linear 0.3043 = 1.398 * 0.5^2.2
// run through the sRGB OETF), verified in the probe that produced
// these numbers; the fixture tables exercise the same conversion
// end-to-end.

const float kAdobeMatrix[9] = {
    1.398348f, -0.398085f, 0.000027f,
    -0.000018f, 0.999936f, 0.000005f,
    -0.000013f, -0.042949f, 1.042820f,
};
const float kProPhotoMatrix[9] = {
    2.034294f, -0.727303f, -0.306703f,
    -0.228838f, 1.231677f, -0.002917f,
    -0.008571f, -0.153280f, 1.161708f,
};

// Build an RGB profile on the fly with lcms2 (same calls as
// tests/samples/make_icc_profiles.c, which writes the .icc files
// embedded in the fixtures). No binary ICC blobs in the test.
cmsHPROFILE MakeRgbProfile(double rx, double ry, double gx, double gy,
                           double bx, double by, double wx, double wy,
                           double gamma) {
    cmsCIExyY white;
    white.x = wx;
    white.y = wy;
    white.Y = 1.0;
    cmsCIExyYTRIPLE prim;
    prim.Red.x = rx;   prim.Red.y = ry;   prim.Red.Y = 1.0;
    prim.Green.x = gx; prim.Green.y = gy; prim.Green.Y = 1.0;
    prim.Blue.x = bx;  prim.Blue.y = by;  prim.Blue.Y = 1.0;
    cmsToneCurve* trc[3];
    for (int i = 0; i < 3; i++) {
        trc[i] = cmsBuildGamma(NULL, gamma);
    }
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &prim, trc);
    for (int i = 0; i < 3; i++) {
        cmsFreeToneCurve(trc[i]);
    }
    return h;
}

cmsHPROFILE MakeGrayProfile(double wx, double wy, double gamma) {
    cmsCIExyY white;
    white.x = wx;
    white.y = wy;
    white.Y = 1.0;
    cmsToneCurve* trc = cmsBuildGamma(NULL, gamma);
    cmsHPROFILE h = cmsCreateGrayProfile(&white, trc);
    cmsFreeToneCurve(trc);
    return h;
}

// An sRGB-TRC gray profile: the sRGB EOTF as a 4097-entry table
// (evaluates to 0.214041 at 0.5, the value BuildColorSpace compares
// against). A tabulated curve is used because the parametric form
// (cmsBuildParametricToneCurve type 3) did not evaluate to the sRGB
// value on this lcms2 (0.2914 at 0.5); the table is exact by
// construction.
cmsHPROFILE MakeSrgbGrayProfile() {
    static float table[4097];
    for (int i = 0; i <= 4096; i++) {
        double c = (double)i / 4096.0;
        table[i] = (float)(c <= 0.04045 ? c / 12.92
                                        : std::pow((c + 0.055) / 1.055, 2.4));
    }
    cmsToneCurve* trc = cmsBuildTabulatedToneCurveFloat(NULL, 4097, table);
    cmsCIExyY white;
    white.x = 0.3127;
    white.y = 0.3290;
    white.Y = 1.0;
    cmsHPROFILE h = cmsCreateGrayProfile(&white, trc);
    cmsFreeToneCurve(trc);
    return h;
}

std::vector<uint8_t> ProfileBytes(cmsHPROFILE h) {
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(h, NULL, &size) || size == 0) {
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> bytes(size);
    if (!cmsSaveProfileToMem(h, bytes.data(), &size)) {
        return std::vector<uint8_t>();
    }
    return bytes;
}

bool IsIdentityMatrix(const float m[9]) {
    for (int i = 0; i < 9; i++) {
        float want = (i % 4 == 0) ? 1.0f : 0.0f;
        if (m[i] != want) {
            return false;
        }
    }
    return true;
}

bool AllTrcNull(const PsdHdr::ColorSpace& cs) {
    return cs.trc[0] == NULL && cs.trc[1] == NULL && cs.trc[2] == NULL;
}

// A rejected profile must return false AND leave the out-parameter
// in the "no colour space" state (no tables, identity matrix).
void ExpectBuildRejected(const uint8_t* data, size_t size, const char* what) {
    PsdHdr::ColorSpace cs;
    std::vector<float> storage;
    Check(!PsdIcc::BuildColorSpace(data, size, cs, storage), what);
    Check(AllTrcNull(cs) && IsIdentityMatrix(cs.matrix), what);
}

void CheckMatrixNear(const float got[9], const float want[9],
                     const char* what) {
    for (int i = 0; i < 9; i++) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s matrix[%d] got %.6f want %.6f",
                      what, i, (double)got[i], (double)want[i]);
        Check(Near((double)got[i], (double)want[i], 5e-3), buf);
    }
}

// Read a whole file into a byte vector (for the raw .icc fixture).
// Returns an empty vector when the file cannot be read.
std::vector<uint8_t> ReadWholeFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (f == NULL) {
        return std::vector<uint8_t>();
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return std::vector<uint8_t>();
    }
    std::vector<uint8_t> data((size_t)size);
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return std::vector<uint8_t>();
    }
    std::fclose(f);
    return data;
}

void TestPsdIccBuildColorSpace() {
    // Adobe RGB (1998), gamma 2.2.
    {
        cmsHPROFILE h = MakeRgbProfile(0.64, 0.33, 0.21, 0.71, 0.15, 0.06,
                                       0.3127, 0.3290, 2.2);
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        Check(!bytes.empty(), "Adobe profile serializes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "Adobe RGB BuildColorSpace returns TRUE");
        Check(cs.trc[0] != NULL, "Adobe trc[0] set");
        Check(cs.trc[1] != NULL, "Adobe trc[1] set");
        Check(cs.trc[2] != NULL, "Adobe trc[2] set");
        // Gamma 2.2 evaluated at 32768/65535: (32768/65535)^2.2.
        Check(Near((double)cs.trc[0][0], 0.0, 1e-6), "Adobe trc[0] == 0");
        Check(Near((double)cs.trc[0][32768], 0.217645, 1e-3),
              "Adobe trc[32768] ~= 0.2176 (gamma 2.2 at 0.5)");
        Check(Near((double)cs.trc[1][32768], 0.217645, 1e-3),
              "Adobe trc[1][32768] ~= 0.2176");
        Check(Near((double)cs.trc[2][32768], 0.217645, 1e-3),
              "Adobe trc[2][32768] ~= 0.2176");
        Check(Near((double)cs.trc[0][65535], 1.0, 1e-4), "Adobe trc[65535] == 1");
        // D50 matrix (numpy): 1.398348. The wrong D65 route gives
        // 1.488 — explicitly rejected, not just "close to 1.398".
        Check(Near((double)cs.matrix[0], 1.398348, 5e-3),
              "Adobe matrix[0] ~= 1.398 (D50, not D65)");
        Check(std::fabs((double)cs.matrix[0] - 1.488) > 0.05,
              "Adobe matrix[0] is NOT 1.488 (wrong D65 value)");
        CheckMatrixNear(cs.matrix, kAdobeMatrix, "Adobe");
        // White (1,1,1) maps to (1,1,1): the matrix preserves white.
        for (int r = 0; r < 3; r++) {
            double v = (double)cs.matrix[r * 3 + 0] + (double)cs.matrix[r * 3 + 1] +
                       (double)cs.matrix[r * 3 + 2];
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "Adobe white row %d sums to 1", r);
            Check(Near(v, 1.0, 5e-3), buf);
        }
    }
    // ProPhoto RGB, gamma 1.8.
    {
        cmsHPROFILE h = MakeRgbProfile(0.7347, 0.2653, 0.1596, 0.8404,
                                       0.0366, 0.0001, 0.3457, 0.3585, 1.8);
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        Check(!bytes.empty(), "ProPhoto profile serializes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "ProPhoto BuildColorSpace returns TRUE");
        Check(cs.trc[0] != NULL && cs.trc[1] != NULL && cs.trc[2] != NULL,
              "ProPhoto trc[0..2] set");
        // Gamma 1.8 at 32768/65535: (32768/65535)^1.8.
        Check(Near((double)cs.trc[0][32768], 0.287182, 1e-3),
              "ProPhoto trc[32768] ~= 0.2872 (gamma 1.8 at 0.5)");
        CheckMatrixNear(cs.matrix, kProPhotoMatrix, "ProPhoto");
        double wsum = (double)cs.matrix[0] + (double)cs.matrix[1] + (double)cs.matrix[2];
        Check(Near(wsum, 1.0, 5e-3), "ProPhoto white row 0 sums to 1");
    }
    // sRGB (RGB): detected as sRGB, FALSE, sRGB path stays byte-identical.
    {
        cmsHPROFILE h = cmsCreate_sRGBProfile();
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        Check(!bytes.empty(), "sRGB profile serializes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(!PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "sRGB profile BuildColorSpace returns FALSE");
        Check(AllTrcNull(cs), "sRGB rejected: trc all NULL");
        Check(IsIdentityMatrix(cs.matrix), "sRGB rejected: matrix identity");
    }
    // Real sRGB IEC 61966-2-1 profile (D65 wtpt): the standard profile
    // extracted from a real 16-bit sRGB PSD (tests/samples/
    // srgb_iec61966-2-1.icc). Its wtpt is D65 (0.9505, 1.0, 1.0891)
    // while its colorants are the D50 sRGB values - the D50-only
    // white-point check mis-detected it as non-sRGB and built a
    // diagonal matrix of 0.827/1.022/1.370 (blue shift; regression
    // reported 2026-11-16 on e642229). This test must fail on the
    // unfixed code (wtpt Z differs from D50 by 0.264 >> 1e-3).
    {
        std::vector<uint8_t> bytes =
            ReadWholeFile("tests/samples/srgb_iec61966-2-1.icc");
        Check(!bytes.empty(), "real sRGB IEC profile fixture loads");
        Check(bytes.size() == 3144, "real sRGB IEC profile is 3144 bytes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(!PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "real sRGB IEC profile BuildColorSpace returns FALSE");
        Check(AllTrcNull(cs), "real sRGB IEC rejected: trc all NULL");
        Check(IsIdentityMatrix(cs.matrix),
              "real sRGB IEC rejected: matrix identity");
    }
    // Real Adobe RGB (1998) profile (D65 wtpt): the standard profile
    // extracted from a real 16-bit Adobe RGB PSD (tests/samples/
    // adobe_rgb_1998.icc). Its wtpt is D65 (0.9505, 1.0, 1.0891)
    // while its colorants are the D50 Adobe primaries - using the
    // wtpt directly in the M*S = W solve mixed D65 with D50 and
    // built a diagonal matrix of 0.827/1.022/1.370 (blue shift;
    // regression reported 2026-11-16 on 42ca46a, same numbers as
    // the sRGB case). The solve must use the D50 white point when
    // wtpt is D65, so this profile must give the same D50 matrix
    // as the lcms2-built Adobe profile above. This test must fail
    // on the unfixed code (red 0.5 comes out ~0.555 instead of
    // 0.5877, gray 0.5 comes out [0.41, 0.51, 0.68]).
    {
        std::vector<uint8_t> bytes =
            ReadWholeFile("tests/samples/adobe_rgb_1998.icc");
        Check(!bytes.empty(), "real Adobe RGB profile fixture loads");
        Check(bytes.size() == 560, "real Adobe RGB profile is 560 bytes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "real Adobe RGB BuildColorSpace returns TRUE");
        Check(cs.trc[0] != NULL && cs.trc[1] != NULL && cs.trc[2] != NULL,
              "real Adobe RGB trc[0..2] set");
        // Gamma 2.1992 at 32768/65535: (32768/65535)^2.1992 ~= 0.2176.
        Check(Near((double)cs.trc[0][32768], 0.217645, 1e-3),
              "real Adobe RGB trc[32768] ~= 0.2176 (gamma 2.1992 at 0.5)");
        // D50 matrix (numpy): 1.398348. The wrong D65-wtpt route gives
        // 1.2345 in matrix[0] - explicitly rejected, not just "close".
        Check(Near((double)cs.matrix[0], 1.398348, 5e-3),
              "real Adobe RGB matrix[0] ~= 1.398 (D50, not D65-wtpt)");
        Check(std::fabs((double)cs.matrix[0] - 1.2345) > 0.05,
              "real Adobe RGB matrix[0] is NOT 1.2345 (wrong D65-wtpt value)");
        CheckMatrixNear(cs.matrix, kAdobeMatrix, "real Adobe RGB");
        // White (1,1,1) maps to (1,1,1): the matrix preserves white.
        for (int r = 0; r < 3; r++) {
            double v = (double)cs.matrix[r * 3 + 0] + (double)cs.matrix[r * 3 + 1] +
                       (double)cs.matrix[r * 3 + 2];
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "real Adobe RGB white row %d sums to 1", r);
            Check(Near(v, 1.0, 5e-3), buf);
        }
        // Neutral gray (0.5,0.5,0.5) stays neutral: max channel
        // spread < 0.01. (The wrong matrix gives [0.41, 0.51, 0.68].)
        {
            double g0 = 0.5 * ((double)cs.matrix[0] + (double)cs.matrix[1] + (double)cs.matrix[2]);
            double g1 = 0.5 * ((double)cs.matrix[3] + (double)cs.matrix[4] + (double)cs.matrix[5]);
            double g2 = 0.5 * ((double)cs.matrix[6] + (double)cs.matrix[7] + (double)cs.matrix[8]);
            double mx = g0 > g1 ? (g0 > g2 ? g0 : g2) : (g1 > g2 ? g1 : g2);
            double mn = g0 < g1 ? (g0 < g2 ? g0 : g2) : (g1 < g2 ? g1 : g2);
            Check(mx - mn < 0.01, "real Adobe RGB gray 0.5 stays neutral");
        }
        // Adobe red 0.5 (encoded) -> sRGB-encoded ~0.5877 (lcms2
        // ground truth): linear 0.2176 through matrix[0] = 0.3043,
        // then the sRGB OETF. (The wrong matrix gives ~0.5551.)
        {
            double lin = (double)cs.trc[0][32768];
            double srgbLin = (double)cs.matrix[0] * lin;
            double srgbEnc = srgbLin <= 0.0031308
                                 ? 12.92 * srgbLin
                                 : 1.055 * std::pow(srgbLin, 1.0 / 2.4) - 0.055;
            Check(Near(srgbEnc, 0.5877, 5e-3),
                  "real Adobe RGB red 0.5 -> sRGB 0.5877");
        }
    }
    // sRGB gray: also FALSE (its TRC is the sRGB EOTF).
    {
        cmsHPROFILE h = MakeSrgbGrayProfile();
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        Check(!bytes.empty(), "sRGB gray profile serializes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(!PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "sRGB gray BuildColorSpace returns FALSE");
        Check(AllTrcNull(cs) && IsIdentityMatrix(cs.matrix),
              "sRGB gray rejected: no tables, identity matrix");
    }
    // Gray, gamma 2.2: TRUE, TRC only (no primaries, no matrix).
    {
        cmsHPROFILE h = MakeGrayProfile(0.3127, 0.3290, 2.2);
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        Check(!bytes.empty(), "gray gamma-2.2 profile serializes");
        PsdHdr::ColorSpace cs;
        std::vector<float> storage;
        Check(PsdIcc::BuildColorSpace(bytes.data(), bytes.size(), cs, storage),
              "gray gamma-2.2 BuildColorSpace returns TRUE");
        Check(cs.trc[0] != NULL, "gray trc[0] set");
        Check(cs.trc[1] == NULL && cs.trc[2] == NULL,
              "gray trc[1]/trc[2] NULL");
        Check(IsIdentityMatrix(cs.matrix), "gray matrix identity");
        Check(Near((double)cs.trc[0][32768], 0.217645, 1e-3),
              "gray trc[32768] ~= 0.2176 (gamma 2.2 at 0.5)");
    }
    // Invalid inputs: NULL, zeros, truncated, garbage, tiny.
    ExpectBuildRejected(NULL, 0, "BuildColorSpace NULL/0 rejected");
    {
        uint8_t zeros[64] = { 0 };
        ExpectBuildRejected(zeros, sizeof(zeros),
                            "BuildColorSpace 64 zero bytes rejected");
    }
    {
        cmsHPROFILE h = MakeRgbProfile(0.64, 0.33, 0.21, 0.71, 0.15, 0.06,
                                       0.3127, 0.3290, 2.2);
        std::vector<uint8_t> bytes = ProfileBytes(h);
        cmsCloseProfile(h);
        ExpectBuildRejected(bytes.data(), 100,
                            "BuildColorSpace truncated profile rejected");
    }
    {
        const char* garbage = "this is not an ICC profile at all, just text";
        ExpectBuildRejected((const uint8_t*)garbage, std::strlen(garbage),
                            "BuildColorSpace garbage bytes rejected");
    }
    {
        uint8_t tiny[4] = { 0x00, 0x00, 0x02, 0x58 };
        ExpectBuildRejected(tiny, sizeof(tiny),
                            "BuildColorSpace 4-byte input rejected");
    }
}

// sRGB EOTF reference, written out (not PsdHdr::SrgbEotf16, so the
// NULL-path expectations below are independent of the code under test).
double SrgbEotfRef(uint16_t v) {
    double s = (double)v / 65535.0;
    if (s <= 0.04045) {
        return s / 12.92;
    }
    return std::pow((s + 0.055) / 1.055, 2.4);
}

void PutBe16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

void PutBe32Float(uint8_t* p, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    p[0] = (uint8_t)(bits >> 24);
    p[1] = (uint8_t)((bits >> 16) & 0xFF);
    p[2] = (uint8_t)((bits >> 8) & 0xFF);
    p[3] = (uint8_t)(bits & 0xFF);
}

void TestPsdHdrColorSpaceSynthetic() {
    using namespace PsdHdr;
    // Hand-built ColorSpace: a gamma-2.2 TRC (built here with pow,
    // the curve's definition) + the Adobe primaries matrix from the
    // independent numpy computation (kAdobeMatrix, NOT C++ output).
    std::vector<float> trc(65536);
    for (int v = 0; v < 65536; v++) {
        trc[(size_t)v] = (float)std::pow((double)v / 65535.0, 2.2);
    }
    ColorSpace cs;
    cs.trc[0] = trc.data();
    cs.trc[1] = trc.data();
    cs.trc[2] = trc.data();
    for (int i = 0; i < 9; i++) {
        cs.matrix[i] = kAdobeMatrix[i];
    }

    // 2x2 16-bit RGB raw, planar (R, G, B planes, 2 rows each).
    // Pixel codes (row-major): (0,0,0), (32768,16384,49152),
    // (65535,32768,0), (47824,30058,57502).
    const uint16_t rPlane[4] = { 0, 32768, 65535, 47824 };
    const uint16_t gPlane[4] = { 0, 16384, 32768, 30058 };
    const uint16_t bPlane[4] = { 0, 49152, 0, 57502 };
    uint8_t raw[24];
    for (int i = 0; i < 4; i++) {
        PutBe16(raw + i * 2, rPlane[i]);
        PutBe16(raw + 8 + i * 2, gPlane[i]);
        PutBe16(raw + 16 + i * 2, bPlane[i]);
    }
    float outCs[12] = { 0.0f };
    Check(DecodeImageData(raw, sizeof(raw), 2, 2, 3, kColorModeRgb, 16,
                          kCompressionRaw, 1, outCs, NULL, 0, &cs),
          "synthetic ColorSpace 16-bit 2x2 decodes");
    float outNull[12] = { 0.0f };
    Check(DecodeImageData(raw, sizeof(raw), 2, 2, 3, kColorModeRgb, 16,
                          kCompressionRaw, 1, outNull, NULL, 0, NULL),
          "synthetic NULL 16-bit 2x2 decodes");
    for (int px = 0; px < 4; px++) {
        double lin[3];
        const uint16_t codes[3] = { rPlane[px], gPlane[px], bPlane[px] };
        for (int c = 0; c < 3; c++) {
            lin[c] = std::pow((double)codes[c] / 65535.0, 2.2);
        }
        double want[3];
        for (int r = 0; r < 3; r++) {
            want[r] = (double)kAdobeMatrix[r * 3 + 0] * lin[0] +
                      (double)kAdobeMatrix[r * 3 + 1] * lin[1] +
                      (double)kAdobeMatrix[r * 3 + 2] * lin[2];
        }
        for (int c = 0; c < 3; c++) {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "ColorSpace px%d ch%d got %.6f want %.6f",
                          px, c, (double)outCs[px * 3 + c], want[c]);
            Check(Near((double)outCs[px * 3 + c], want[c], 1e-4), buf);
        }
        // NULL path: sRGB EOTF per channel, no matrix.
        for (int c = 0; c < 3; c++) {
            double w = SrgbEotfRef(codes[c]);
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "NULL px%d ch%d got %.6f want %.6f",
                          px, c, (double)outNull[px * 3 + c], w);
            Check(Near((double)outNull[px * 3 + c], w, 1e-4), buf);
        }
    }
    // The two paths differ exactly as the TRC/matrix predict: pixel 1
    // R is 0.2854 with the ColorSpace, 0.2140 on the NULL (sRGB) path.
    Check(std::fabs((double)outCs[1 * 3 + 0] - (double)outNull[1 * 3 + 0]) > 0.01,
          "ColorSpace vs NULL differ on px1 R (0.2854 vs 0.2140)");

    // 32-bit with a matrix: samples are already linear, so the matrix
    // applies with no TRC step.
    {
        const float px0[3] = { 0.5f, 0.25f, 1.0f };
        const float px1[3] = { 1.0f, 0.0f, 0.5f };
        // Planar, like the file format: the R plane holds both
        // pixels' R, then the G plane, then the B plane (NOT
        // interleaved px0 then px1).
        uint8_t raw32[24];
        for (int c = 0; c < 3; c++) {
            PutBe32Float(raw32 + (size_t)c * 8 + 0, px0[c]);
            PutBe32Float(raw32 + (size_t)c * 8 + 4, px1[c]);
        }
        float out[6] = { 0.0f };
        Check(DecodeImageData(raw32, sizeof(raw32), 2, 1, 3, kColorModeRgb,
                              32, kCompressionRaw, 1, out, NULL, 0, &cs),
              "synthetic ColorSpace 32-bit 2x1 decodes");
        const float* px[2] = { px0, px1 };
        for (int p = 0; p < 2; p++) {
            for (int r = 0; r < 3; r++) {
                double w = (double)kAdobeMatrix[r * 3 + 0] * (double)px[p][0] +
                           (double)kAdobeMatrix[r * 3 + 1] * (double)px[p][1] +
                           (double)kAdobeMatrix[r * 3 + 2] * (double)px[p][2];
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "32-bit matrix px%d ch%d got %.6f want %.6f",
                              p, r, (double)out[p * 3 + r], w);
                Check(Near((double)out[p * 3 + r], w, 1e-4), buf);
            }
        }
    }
    // Gray 16-bit with a TRC: the TRC linearizes, no matrix applies.
    {
        uint8_t grayRaw[4];
        PutBe16(grayRaw, 32768);
        PutBe16(grayRaw + 2, 65535);
        float out[6] = { 0.0f };
        Check(DecodeImageData(grayRaw, sizeof(grayRaw), 2, 1, 1,
                              kColorModeGrayscale, 16, kCompressionRaw, 1,
                              out, NULL, 0, &cs),
              "synthetic ColorSpace gray 16-bit decodes");
        Check(Near((double)out[0], std::pow(32768.0 / 65535.0, 2.2), 1e-4),
              "gray 16-bit 32768 -> gamma-2.2 linear 0.2176");
        Check(Near((double)out[3], 1.0, 1e-4),
              "gray 16-bit 65535 -> 1.0");
    }
    // Gray 32-bit with a ColorSpace: already linear, and the matrix
    // must NOT apply to gray (grey is grey).
    {
        uint8_t grayRaw[4];
        PutBe32Float(grayRaw, 0.5f);
        float out[3] = { 0.0f };
        Check(DecodeImageData(grayRaw, sizeof(grayRaw), 1, 1, 1,
                              kColorModeGrayscale, 32, kCompressionRaw, 1,
                              out, NULL, 0, &cs),
              "synthetic ColorSpace gray 32-bit decodes");
        Check(out[0] == 0.5f && out[1] == 0.5f && out[2] == 0.5f,
              "gray 32-bit 0.5 passes through, matrix not applied");
    }
    // A ColorSpace with all TRC tables NULL still applies its matrix,
    // with the sRGB EOTF as the per-channel fallback.
    {
        ColorSpace matrixOnly;
        matrixOnly.trc[0] = NULL;
        matrixOnly.trc[1] = NULL;
        matrixOnly.trc[2] = NULL;
        for (int i = 0; i < 9; i++) {
            matrixOnly.matrix[i] = kAdobeMatrix[i];
        }
        uint8_t one[6];
        PutBe16(one, 32768);
        PutBe16(one + 2, 32768);
        PutBe16(one + 4, 32768);
        float out[3] = { 0.0f };
        Check(DecodeImageData(one, sizeof(one), 1, 1, 3, kColorModeRgb, 16,
                              kCompressionRaw, 1, out, NULL, 0, &matrixOnly),
              "synthetic matrix-only ColorSpace decodes");
        double e = SrgbEotfRef(32768);
        for (int r = 0; r < 3; r++) {
            double w = ((double)kAdobeMatrix[r * 3 + 0] +
                        (double)kAdobeMatrix[r * 3 + 1] +
                        (double)kAdobeMatrix[r * 3 + 2]) * e;
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "matrix-only ch%d got %.6f want %.6f",
                          r, (double)out[r], w);
            Check(Near((double)out[r], w, 1e-4), buf);
        }
    }
}

// Read a whole fixture file and return the bytes of its 0x040F image
// resource (the ICC profile), the same block layout PSDWrapper reads:
// "8BIM", uint16 id, Pascal name (1 size byte + name, padded to
// even), uint32 size, data (padded to even).
bool LoadIccResource(const char* path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 26 + 4 + 4 + 4 + 2) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> data((size_t)size);
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    size_t pos = 26;
    if (pos + 4 > data.size()) {
        return false;
    }
    uint32_t colorLen = ReadBe32(data.data() + pos);
    pos += 4 + colorLen;
    if (pos + 4 > data.size()) {
        return false;
    }
    uint32_t resLen = ReadBe32(data.data() + pos);
    pos += 4;
    size_t resEnd = pos + resLen;
    if (resEnd > data.size()) {
        return false;
    }
    while (pos < resEnd) {
        if (pos + 6 + 1 + 4 > resEnd ||
            std::memcmp(data.data() + pos, "8BIM", 4) != 0) {
            return false;
        }
        uint16_t id = ReadBe16(data.data() + pos + 4);
        uint8_t nameLen = data[pos + 6];
        size_t nameField = (size_t)1 + (size_t)nameLen;
        if (nameField % 2 != 0) {
            nameField += 1;
        }
        size_t sizeOff = pos + 6 + nameField;
        if (sizeOff + 4 > resEnd) {
            return false;
        }
        uint32_t blockSize = ReadBe32(data.data() + sizeOff);
        size_t blockStart = sizeOff + 4;
        if (blockStart + blockSize > resEnd) {
            return false;
        }
        if (id == 0x040F) {
            out->assign(data.begin() + (long)blockStart,
                        data.begin() + (long)(blockStart + blockSize));
            return true;
        }
        pos = blockStart + blockSize;
        if (blockSize % 2 != 0) {
            pos += 1;
        }
    }
    return false;
}

// Decode one H-02 fixture end-to-end: parse the file, build the
// ColorSpace from its embedded 0x040F profile (this is the
// PSDWrapper wiring, at the byte level), decode with it, tone-map,
// and compare against the Python reference table.
void CheckFixtureWithIcc(const char* path, const uint8_t* expected,
                         int expectedBytes) {
    PsdFile psd;
    Check(LoadPsd(path, &psd), "parse H-02 fixture");
    std::vector<uint8_t> icc;
    Check(LoadIccResource(path, &icc), "H-02 fixture has 0x040F resource");
    PsdHdr::ColorSpace cs;
    std::vector<float> storage;
    Check(PsdIcc::BuildColorSpace(icc.data(), icc.size(), cs, storage),
          "H-02 fixture profile builds a ColorSpace");
    size_t pixels = (size_t)psd.width * (size_t)psd.height;
    std::vector<float> linear(pixels * 3);
    std::vector<uint8_t> bgr(pixels * 3);
    bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                       psd.width, psd.height, psd.channels,
                                       psd.colorMode, psd.bitDepth,
                                       psd.compression, psd.version,
                                       linear.data(), NULL, 0, &cs);
    Check(ok, "H-02 fixture decodes with ColorSpace");
    HdrToneMap::ConvertLinearImage(linear.data(), psd.width, psd.height,
                                    (size_t)psd.width * 3, bgr.data(),
                                    (size_t)psd.width * 3);
    bool match = true;
    for (int i = 0; i < expectedBytes; i++) {
        int diff = (int)bgr[(size_t)i] - (int)expected[i];
        if (diff < -2 || diff > 2) {
            match = false;
            std::printf("FAIL: %s byte %d: got %d, expected %d\n",
                        path, i, (int)bgr[(size_t)i], (int)expected[i]);
            break;
        }
    }
    g_checks++;
    if (!match) {
        g_failures++;
    }
}

void TestH02Fixtures() {
    CheckFixtureWithIcc("tests/samples/psd_hdr_16bit_adobe_rgb_raw.psd",
                        kExpectedH02AdobeRgb, sizeof(kExpectedH02AdobeRgb));
    CheckFixtureWithIcc("tests/samples/psd_hdr_16bit_prophoto_rgb_raw.psd",
                        kExpectedH02ProPhoto, sizeof(kExpectedH02ProPhoto));
    CheckFixtureWithIcc("tests/samples/psd_hdr_16bit_adobe_gray_raw.psd",
                        kExpectedH02AdobeGray, sizeof(kExpectedH02AdobeGray));
    // Negative control: the same Adobe fixture decoded as sRGB (NULL
    // ColorSpace) must NOT match the Adobe table — the ICC path has
    // to actually change the output. The colour row's B channel is
    // the discriminator (208 with the profile, ~188 as sRGB).
    PsdFile psd;
    Check(LoadPsd("tests/samples/psd_hdr_16bit_adobe_rgb_raw.psd", &psd),
          "parse Adobe fixture (NULL control)");
    std::vector<float> linear(5 * 2 * 3);
    std::vector<uint8_t> bgr(5 * 2 * 3);
    bool ok = PsdHdr::DecodeImageData(psd.imageData.data(), psd.imageData.size(),
                                       psd.width, psd.height, psd.channels,
                                       psd.colorMode, psd.bitDepth,
                                       psd.compression, psd.version,
                                       linear.data(), NULL, 0, NULL);
    Check(ok, "Adobe fixture decodes on NULL path");
    HdrToneMap::ConvertLinearImage(linear.data(), psd.width, psd.height,
                                    (size_t)psd.width * 3, bgr.data(),
                                    (size_t)psd.width * 3);
    int maxDiff = 0;
    for (int i = 0; i < (int)sizeof(kExpectedH02AdobeRgb); i++) {
        int diff = (int)bgr[(size_t)i] - (int)kExpectedH02AdobeRgb[i];
        if (diff < 0) {
            diff = -diff;
        }
        if (diff > maxDiff) {
            maxDiff = diff;
        }
    }
    Check(maxDiff > 2, "NULL decode of Adobe fixture differs from Adobe table");
    std::printf("Adobe fixture NULL-vs-ICC max BGR diff: %d\n", maxDiff);
}

}  // namespace

int main() {
    TestSrgbEotf16();
    TestSanitizeFloat32();
    TestReverseDelta();
    TestAlphaBytes();
    TestRejections();
    TestFixtures();
    TestFixtureAlpha();
    TestWrapperOrderSimulation();
    TestLinearValues();
    TestRollOff();
    TestPsdIccBuildColorSpace();
    TestPsdHdrColorSpaceSynthetic();
    TestH02Fixtures();
    std::printf("psd_hdr_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
