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
//       src/JPEGView/HdrToneMap.cpp -lz -o /tmp/psd_hdr_test
//   /tmp/psd_hdr_test
// The vendored include dir is passed so the test proves the vendored
// zlib 1.3.2 headers compile; it links the system -lz (inflateInit_
// only checks version[0], and the headers match the linked 1.3.2
// zlib.lib on Windows exactly).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "HdrToneMap.h"
#include "PsdHdr.h"

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
                                       linear.data(), NULL, 0);
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
                          16, kCompressionRaw, 1, out, NULL, 0),
          "valid 1x1 grey raw decodes");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           8, kCompressionRaw, 1, out, NULL, 0),
          "8-bit rejected (8-bit wrapper owns it)");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeCmyk,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "CMYK rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 3, kColorModeLab,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "Lab rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, 4, 1, out, NULL, 0),
          "compression 4 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 3, out, NULL, 0),
          "version 3 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 0, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "width 0 rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 2, kColorModeRgb,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "RGB with 2 channels rejected");
    Check(!DecodeImageData(NULL, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "NULL src rejected");
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, NULL, NULL, 0),
          "NULL output rejected");
    // Truncated raw data (1 byte for a 2-byte sample).
    Check(!DecodeImageData(raw16, 1, 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, NULL, 0),
          "truncated raw rejected");
    // Alpha stride smaller than the width.
    uint8_t alpha[1] = { 0 };
    Check(!DecodeImageData(raw16, sizeof(raw16), 1, 1, 1, kColorModeGrayscale,
                           16, kCompressionRaw, 1, out, alpha, 0),
          "alpha stride 0 rejected");
    (void)alpha;
    // RLE: a repeat run crossing the row boundary (1x1 grey, 2 bytes
    // per row; header 0xFE = repeat 3 copies of the next byte).
    // Table (1 row x 1 channel, 2 bytes) + 2 PackBits bytes.
    const uint8_t rleCross[4] = { 0x00, 0x02, 0xFE, 0x80 };
    Check(!DecodeImageData(rleCross, sizeof(rleCross), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionRle, 1,
                           out, NULL, 0),
          "RLE run crossing row boundary rejected");
    // RLE: truncated byte-count table (declares 1 row but no table).
    const uint8_t rleNoTable[1] = { 0x00 };
    Check(!DecodeImageData(rleNoTable, sizeof(rleNoTable), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionRle, 1,
                           out, NULL, 0),
          "RLE truncated table rejected");
    // ZIP: truncated zlib stream (first bytes of a real stream for
    // 2 zero bytes, cut short).
    const uint8_t zipTrunc[3] = { 0x78, 0x9C, 0x63 };
    Check(!DecodeImageData(zipTrunc, sizeof(zipTrunc), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionZip, 1,
                           out, NULL, 0),
          "truncated ZIP rejected");
    // ZIP: garbage that is not a zlib stream at all.
    const uint8_t zipGarbage[4] = { 0x00, 0x01, 0x02, 0x03 };
    Check(!DecodeImageData(zipGarbage, sizeof(zipGarbage), 1, 1, 1,
                           kColorModeGrayscale, 16, kCompressionZip, 1,
                           out, NULL, 0),
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
                                           linear.data(), alpha, 5);
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
                                           linear.data(), alpha, 9);
        Check(ok, "decode 32-bit RGBA fixture with alpha");
        const uint8_t expected[18] = { 255, 191, 128, 64, 0, 0, 0, 0, 0,
                                       255, 191, 128, 64, 0, 0, 0, 0, 0 };
        Check(std::memcmp(alpha, expected, sizeof(expected)) == 0,
              "32-bit RGBA alpha ramp 255/191/128/64/0");
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
                                           linear.data(), NULL, 0);
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
                                           linear.data(), NULL, 0);
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
                                       linear.data(), NULL, 0);
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

}  // namespace

int main() {
    TestSrgbEotf16();
    TestSanitizeFloat32();
    TestReverseDelta();
    TestAlphaBytes();
    TestRejections();
    TestFixtures();
    TestFixtureAlpha();
    TestLinearValues();
    TestRollOff();
    std::printf("psd_hdr_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
