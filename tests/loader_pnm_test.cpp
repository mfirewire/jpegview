/*  loader_pnm_test.cpp - regression cases for the PNM maxval scaling.
 *
 *  PNM defines white as maxval, not as 255, and stb_image ignores the field
 *  and hands the raw samples back. Two regressions hid there:
 *
 *    (1) 8-bit maxval < 255: a legal maxval=15 PGM displayed near-black
 *        (sample 15 out of 255) instead of pure white.
 *    (2) 16-bit maxval != 65535 (B-PNM1): stb already shifted the samples
 *        down by >>8, so maxval=4095 white decoded to 15 and displayed at
 *        15/255 instead of 255.
 *
 *  The scaling loop now lives in src/JPEGView/PnmScale.h (plain C++17, no
 *  MFC), so this test can exercise the released code on Linux. It decodes
 *  hand-written minimal PNM files from tests/samples/ with the vendored
 *  stb_image, using exactly the STBI_NO_* defines PPMWrapper.cpp uses (PNM
 *  kept, JPEG/PNG/BMP/TGA/GIF/PIC compiled out; HDR and PSD kept, because
 *  stb v2.30 omits stbi__convert_format16 when both STBI_NO_PNG and
 *  STBI_NO_PSD are defined and stbi__pnm_load still references it), applies
 *  the wrapper's own pre-decode steps (header parse, 16-bit raster
 *  byte-swap: the PNM spec is big-endian, stb reads host-endian), then
 *  PnmScale::ScaleSamples, and asserts the pixels.
 *
 *  Samples (all hand-written minimal files, raster right after the single
 *  delimiter byte following maxval; 16-bit samples big-endian per spec):
 *    pnm_8bit_maxval15.pgm     P5 3x1 maxval 15,  samples 0, 8, 15
 *    pnm_16bit_maxval4095.pgm  P5 3x1 maxval 4095, samples 0, 2048, 4095
 *    pnm_16bit_maxval4095.ppm  P6 2x1 maxval 4095, white then black pixel
 *    pnm_8bit_maxval255.pgm    P5 3x1 maxval 255,  samples 0, 128, 255 (identity)
 *    pnm_16bit_maxval65535.pgm P5 3x1 maxval 65535, samples 0, 32768, 65535 (identity)
 *
 *  Build & run (from the repository root, Linux; no Windows/MFC needed):
 *      g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/loader_pnm_test.cpp -o /tmp/loader_pnm_test
 *      /tmp/loader_pnm_test [tests/samples]
 *  Exit code 0 = all checks passed, 1 = at least one failed.
 */

#include "PnmScale.h"

// Same defines as PPMWrapper.cpp: PNM (and HDR/PSD) kept, the rest out.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#include "stb/stb_image.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

void CheckPixels(const std::vector<unsigned char>& got,
                 const unsigned char* want, size_t n, const char* what) {
    ++g_checks;
    if (got.size() != n || std::memcmp(got.data(), want, n) != 0) {
        ++g_failures;
        std::printf("FAIL  %s: got [", what);
        for (size_t i = 0; i < got.size(); ++i)
            std::printf("%s%u", i ? ", " : "", got[i]);
        std::printf("], expected [");
        for (size_t i = 0; i < n; ++i)
            std::printf("%s%u", i ? ", " : "", want[i]);
        std::printf("]\n");
    }
}

bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return read == out.size();
}

// Minimal PNM header parse, mirroring the pre-decode block in
// PPMWrapper.cpp: whitespace and '#' comments between tokens, raster
// starting right after the single delimiter byte following maxval.
struct PnmHeader {
    int width = 0;
    int height = 0;
    int maxval = 0;
    int channels = 0;
    size_t rasterOffset = 0;
};

bool ParsePnmHeader(const unsigned char* p, size_t size, PnmHeader& hdr) {
    if (size < 2 || p[0] != 'P' || (p[1] != '5' && p[1] != '6')) return false;
    hdr.channels = (p[1] == '6') ? 3 : 1;
    size_t pos = 2;
    unsigned char c = p[pos++];
    int values[3] = {0, 0, 0};
    for (int t = 0; t < 3; ++t) {
        for (;;) {
            while (pos <= size && (c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
                                   c == '\f' || c == '\r')) {
                if (pos >= size) return false;
                c = p[pos++];
            }
            if (c != '#') break;
            while (pos <= size && c != '\n' && c != '\r') {
                if (pos >= size) return false;
                c = p[pos++];
            }
        }
        if (c < '0' || c > '9') return false;
        int v = 0;
        while (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            if (pos >= size) return false;
            c = p[pos++];
        }
        values[t] = v;
    }
    hdr.width = values[0];
    hdr.height = values[1];
    hdr.maxval = values[2];
    hdr.rasterOffset = pos;
    return true;
}

// Decodes one sample the way PPMReader::ReadImage does, up to the scale:
// parse the header, byte-swap the 16-bit raster (spec big-endian, stb
// host-endian), stbi_load_from_memory, then PnmScale::ScaleSamples.
// Pre-scale samples are returned too, so the stb >>8 step is pinned
// separately from the scale.
bool DecodeSample(const std::string& path, PnmHeader& hdr,
                  std::vector<unsigned char>& preScale,
                  std::vector<unsigned char>& postScale) {
    std::vector<unsigned char> file;
    if (!ReadFile(path, file)) return false;
    if (!ParsePnmHeader(file.data(), file.size(), hdr)) return false;
    const unsigned char* pLoad = file.data();
    std::vector<unsigned char> swapped;
    if (hdr.maxval > 255) {
        const size_t rasterBytes = static_cast<size_t>(hdr.width) *
                                   static_cast<size_t>(hdr.height) *
                                   static_cast<size_t>(hdr.channels) * 2;
        if (hdr.rasterOffset + rasterBytes > file.size()) return false;
        swapped = file;
        for (size_t i = hdr.rasterOffset; i + 1 < hdr.rasterOffset + rasterBytes; i += 2) {
            const unsigned char tmp = swapped[i];
            swapped[i] = swapped[i + 1];
            swapped[i + 1] = tmp;
        }
        pLoad = swapped.data();
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* decoded = stbi_load_from_memory(pLoad, static_cast<int>(file.size()), &w, &h, &ch, 0);
    if (decoded == nullptr) return false;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(ch);
    preScale.assign(decoded, decoded + n);
    PnmScale::ScaleSamples(decoded, n, hdr.maxval);
    postScale.assign(decoded, decoded + n);
    const bool dimsOk = (w == hdr.width && h == hdr.height && ch == hdr.channels);
    stbi_image_free(decoded);
    return dimsOk;
}

void TestSamples(const std::string& dir) {
    PnmHeader hdr;
    std::vector<unsigned char> pre, post;

    // (1) 8-bit maxval=15: stb hands back raw 0/8/15, the scale must map
    // white 15 -> 255 and the midpoint 8 -> 136 ((8*255+7)/15).
    Check(DecodeSample(dir + "/pnm_8bit_maxval15.pgm", hdr, pre, post),
          "decode pnm_8bit_maxval15.pgm");
    Check(hdr.maxval == 15, "pnm_8bit_maxval15.pgm maxval is 15");
    { const unsigned char wantPre[] = {0, 8, 15}; CheckPixels(pre, wantPre, 3, "maxval 15 pre-scale"); }
    { const unsigned char want[] = {0, 136, 255}; CheckPixels(post, want, 3, "maxval 15 white->255, mid->136"); }

    // (2) 16-bit maxval=4095 (B-PNM1): after the byte-swap stb >>8 gives
    // 0/8/15, and the scale (divisor 4095>>8 == 15) must give 0/136/255.
    Check(DecodeSample(dir + "/pnm_16bit_maxval4095.pgm", hdr, pre, post),
          "decode pnm_16bit_maxval4095.pgm");
    Check(hdr.maxval == 4095, "pnm_16bit_maxval4095.pgm maxval is 4095");
    { const unsigned char wantPre[] = {0, 8, 15}; CheckPixels(pre, wantPre, 3, "maxval 4095 pre-scale (stb >>8)"); }
    { const unsigned char want[] = {0, 136, 255}; CheckPixels(post, want, 3, "maxval 4095 white->255, mid->136"); }

    // (2b) same, 3-channel PPM: white pixel -> (255,255,255), black -> (0,0,0).
    Check(DecodeSample(dir + "/pnm_16bit_maxval4095.ppm", hdr, pre, post),
          "decode pnm_16bit_maxval4095.ppm");
    Check(hdr.channels == 3, "pnm_16bit_maxval4095.ppm has 3 channels");
    { const unsigned char wantPre[] = {15, 15, 15, 0, 0, 0}; CheckPixels(pre, wantPre, 6, "maxval 4095 PPM pre-scale"); }
    { const unsigned char want[] = {255, 255, 255, 0, 0, 0}; CheckPixels(post, want, 6, "maxval 4095 PPM white->255"); }

    // Identity controls: maxval 255 and 65535 must pass through untouched.
    Check(DecodeSample(dir + "/pnm_8bit_maxval255.pgm", hdr, pre, post),
          "decode pnm_8bit_maxval255.pgm");
    { const unsigned char want[] = {0, 128, 255}; CheckPixels(post, want, 3, "maxval 255 identity"); }
    Check(DecodeSample(dir + "/pnm_16bit_maxval65535.pgm", hdr, pre, post),
          "decode pnm_16bit_maxval65535.pgm");
    { const unsigned char want[] = {0, 128, 255}; CheckPixels(post, want, 3, "maxval 65535 identity"); }
    Check(pre == post, "identity samples: pre-scale equals post-scale");
}

void TestScaleFunction() {
    // Endpoint table, hand-computed from (v*255 + d/2)/d.
    struct Case { int maxval; unsigned char in[6]; unsigned char want[6]; int n; };
    const Case cases[] = {
        {15,   {0, 1, 7, 8, 14, 15}, {0, 17, 119, 136, 238, 255}, 6},
        {100,  {0, 50, 100},         {0, 128, 255},                3},
        {256,  {0, 1},               {0, 255},                     2}, // divisor 256>>8 == 1
        {4095, {0, 8, 15},           {0, 136, 255},                3},
    };
    for (const Case& tc : cases) {
        std::vector<unsigned char> buf(tc.in, tc.in + tc.n);
        PnmScale::ScaleSamples(buf.data(), buf.size(), tc.maxval);
        char what[64];
        std::snprintf(what, sizeof(what), "ScaleSamples maxval=%d table", tc.maxval);
        CheckPixels(buf, tc.want, static_cast<size_t>(tc.n), what);
    }

    // Identity / no-op maxvals must leave the buffer byte-identical.
    for (int mv : {255, 65535, 0, -1, 65536}) {
        std::vector<unsigned char> buf = {0, 1, 127, 128, 255};
        const std::vector<unsigned char> before = buf;
        PnmScale::ScaleSamples(buf.data(), buf.size(), mv);
        char what[64];
        std::snprintf(what, sizeof(what), "ScaleSamples maxval=%d is a no-op", mv);
        Check(buf == before, what);
    }
    // Null buffer and zero count must not crash.
    PnmScale::ScaleSamples(nullptr, 5, 15);
    std::vector<unsigned char> guard = {42};
    PnmScale::ScaleSamples(guard.data(), 0, 15);
    Check(guard[0] == 42, "ScaleSamples count 0 is a no-op");

    // Exhaustive agreement with the released formula, written out longhand
    // here so a transcription slip in the header cannot hide behind itself.
    // v only runs over the values stb can actually hand back: [0, maxval]
    // for 8-bit files, [0, maxval>>8] for 16-bit ones (stb already shifted
    // by >>8). Above that range the (unsigned char) cast in the formula
    // wraps, which no real file can reach - testing it would assert a
    // property the code never promised.
    for (int maxval : {15, 100, 256, 1000, 4095}) {
        const unsigned d = (maxval < 255) ? static_cast<unsigned>(maxval)
                                          : (static_cast<unsigned>(maxval) >> 8);
        const int vMax = (maxval < 256) ? maxval : static_cast<int>(d);
        bool ok = true;
        unsigned char prev = 0;
        for (int v = 0; v <= vMax; ++v) {
            unsigned char buf[1] = {static_cast<unsigned char>(v)};
            PnmScale::ScaleSamples(buf, 1, maxval);
            const unsigned want = (static_cast<unsigned>(v) * 255u + d / 2u) / d;
            if (buf[0] != want) ok = false;
            if (v > 0 && buf[0] < prev) ok = false; // the scale never darkens
            prev = buf[0];
        }
        char what[64];
        std::snprintf(what, sizeof(what), "ScaleSamples maxval=%d matches formula, monotone", maxval);
        Check(ok, what);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    std::printf("loader_pnm_test (samples: %s)\n", dir.c_str());
    TestSamples(dir);
    TestScaleFunction();

    if (g_failures == 0) {
        std::printf("PASS  %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d of %d checks\n", g_failures, g_checks);
    return 1;
}
