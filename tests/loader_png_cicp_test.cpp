// loader_png_cicp_test.cpp - regression cases for FindCicpChunk.
//
// What this pins: the cICP chunk scan that decides whether a PNG is HDR
// (PQ/HLG) and therefore routes to libpng + the tone mapper. A wrong
// scan (e.g. reading the transfer byte from the wrong offset, or
// stopping at the first chunk instead of walking to cICP) silently
// sends HDR PNGs to GDI+, where they render too dark with no error.
// The scan is in src/JPEGView/FindCicpChunk.h, plain C++17, no MFC.
//
// The test reads the real kennylevinsen samples in tests/samples/
// (test_pq.png, test_pq_p3.png, test_hlg.png, test_hlg_p3.png,
// test_srgb.png) and asserts the parsed CICP codes, plus synthetic
// edge cases built by hand (no cICP, malformed cICP, cICP after IDAT).
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/loader_png_cicp_test.cpp src/JPEGView/HdrToneMap.cpp
//       -o /tmp/loader_png_cicp_test
//   /tmp/loader_png_cicp_test

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "FindCicpChunk.h"
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

std::vector<unsigned char> ReadFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf((size_t)n);
    if (n > 0 && std::fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear();
    std::fclose(f);
    return buf;
}

// Build a minimal PNG byte string: signature + IHDR + optional chunks +
// IDAT (empty) + IEND. `chunks` are (name, payload) pairs in order.
std::vector<unsigned char> BuildPng(
    const std::vector<std::pair<const char*, std::vector<unsigned char>>>& chunks) {
    std::vector<unsigned char> out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    auto appendChunk = [&out](const char* name, const std::vector<unsigned char>& payload) {
        unsigned int len = (unsigned int)payload.size();
        out.push_back((unsigned char)((len >> 24) & 0xFF));
        out.push_back((unsigned char)((len >> 16) & 0xFF));
        out.push_back((unsigned char)((len >> 8) & 0xFF));
        out.push_back((unsigned char)(len & 0xFF));
        out.insert(out.end(), name, name + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        // CRC is not verified by the scan, so four zero bytes suffice.
        out.insert(out.end(), { 0, 0, 0, 0 });
    };
    // IHDR: 13 bytes (1x1, 8-bit, colour type 2). Values do not matter
    // for the chunk scan.
    appendChunk("IHDR", { 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0 });
    for (const auto& c : chunks) appendChunk(c.first, c.second);
    appendChunk("IDAT", {});
    appendChunk("IEND", {});
    return out;
}

void TestRealSamples() {
    struct Case { const char* file; int prim; int tr; bool hdr; };
    const Case cases[] = {
        { "tests/samples/test_pq.png", 9, 16, true },
        { "tests/samples/test_pq_p3.png", 12, 16, true },
        { "tests/samples/test_hlg.png", 9, 18, true },
        { "tests/samples/test_hlg_p3.png", 12, 18, true },
        { "tests/samples/test_srgb.png", 1, 13, false },
    };
    for (const Case& c : cases) {
        std::vector<unsigned char> buf = ReadFile(c.file);
        Check(!buf.empty(), c.file);
        if (buf.empty()) continue;
        HdrToneMap::Cicp cicp;
        bool found = PngCicp::FindCicpChunk(buf.data(), buf.size(), 8, cicp);
        Check(found, c.file);
        if (!found) continue;
        Check(cicp.primaries == c.prim && cicp.transfer == c.tr,
              c.file);
        Check(HdrToneMap::IsHdr(cicp) == c.hdr, c.file);
        // The signature-stripped buffer (offset 0) must give the same
        // result: that is the form BeginReading receives.
        HdrToneMap::Cicp cicp2;
        bool found2 = PngCicp::FindCicpChunk(buf.data() + 8, buf.size() - 8, 0, cicp2);
        Check(found2 && cicp2.primaries == c.prim && cicp2.transfer == c.tr,
              c.file);
    }
}

void TestSynthetic() {
    // No cICP at all -> false.
    {
        std::vector<unsigned char> png = BuildPng({});
        HdrToneMap::Cicp cicp;
        Check(!PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "no cICP -> false");
    }
    // cICP before IDAT, PQ -> found, HDR.
    {
        std::vector<unsigned char> png = BuildPng({ { "cICP", { 9, 16, 0, 1 } } });
        HdrToneMap::Cicp cicp;
        Check(PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP PQ found");
        Check(cicp.primaries == 9 && cicp.transfer == 16 && cicp.matrix == 0 && cicp.fullRange == 1,
              "cICP PQ fields");
        Check(HdrToneMap::IsHdr(cicp), "cICP PQ is HDR");
    }
    // cICP after IDAT -> not found (the scan stops at IDAT).
    {
        // Build manually: IHDR, IDAT, cICP, IEND.
        std::vector<unsigned char> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        auto appendChunk = [&png](const char* name, const std::vector<unsigned char>& payload) {
            unsigned int len = (unsigned int)payload.size();
            png.push_back((unsigned char)((len >> 24) & 0xFF));
            png.push_back((unsigned char)((len >> 16) & 0xFF));
            png.push_back((unsigned char)((len >> 8) & 0xFF));
            png.push_back((unsigned char)(len & 0xFF));
            png.insert(png.end(), name, name + 4);
            png.insert(png.end(), payload.begin(), payload.end());
            png.insert(png.end(), { 0, 0, 0, 0 });
        };
        appendChunk("IHDR", { 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0 });
        appendChunk("IDAT", {});
        appendChunk("cICP", { 9, 16, 0, 1 });
        appendChunk("IEND", {});
        HdrToneMap::Cicp cicp;
        Check(!PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP after IDAT -> false");
    }
    // Malformed cICP (wrong length) -> false, not a crash.
    {
        std::vector<unsigned char> png = BuildPng({ { "cICP", { 9, 16, 0 } } });
        HdrToneMap::Cicp cicp;
        Check(!PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP length 3 -> false");
    }
    {
        std::vector<unsigned char> png = BuildPng({ { "cICP", { 9, 16, 0, 1, 0 } } });
        HdrToneMap::Cicp cicp;
        Check(!PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP length 5 -> false");
    }
    // cICP with an sRGB transfer -> found, but not HDR.
    {
        std::vector<unsigned char> png = BuildPng({ { "cICP", { 1, 13, 0, 1 } } });
        HdrToneMap::Cicp cicp;
        Check(PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP sRGB found");
        Check(!HdrToneMap::IsHdr(cicp), "cICP sRGB is not HDR");
    }
    // Truncated buffer (cuts off inside the cICP data) -> false.
    // The cICP data sits at offset 41..45 in this file (sig 8 + IHDR
    // 25 + cICP header 8); cutting 30 bytes off the 73-byte file leaves
    // 43 bytes, i.e. only 2 of the 4 cICP data bytes.
    {
        std::vector<unsigned char> png = BuildPng({ { "cICP", { 9, 16, 0, 1 } } });
        png.resize(png.size() - 30);
        HdrToneMap::Cicp cicp;
        Check(!PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "truncated cICP -> false");
    }
    // A cICP chunk after another ancillary chunk (e.g. gAMA) is still found.
    {
        std::vector<unsigned char> png = BuildPng({
            { "gAMA", { 0, 0, 0xB1, 0x8F } },
            { "cICP", { 12, 18, 0, 1 } },
        });
        HdrToneMap::Cicp cicp;
        Check(PngCicp::FindCicpChunk(png.data(), png.size(), 8, cicp),
              "cICP after gAMA found");
        Check(cicp.primaries == 12 && cicp.transfer == 18,
              "cICP after gAMA fields");
    }
}

}  // namespace

int main() {
    TestRealSamples();
    TestSynthetic();
    std::printf("loader_png_cicp_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
