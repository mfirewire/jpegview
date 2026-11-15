/*  loader_tga_rle_test.cpp - regression cases for TgaRleDecode.h.
 *
 *  The TGA RLE packet decoder (TgaRle::DecodeRlePackets) was extracted
 *  verbatim out of CReaderTGA::ReadTgaImage (ReaderTGA.cpp, B-TEST1
 *  option (a)) so it can be driven without MFC. This test runs it over
 *  the hand-written samples and in-memory packet streams and asserts the
 *  pixels.
 *
 *  The regression being pinned (F-04): channels must be (bits + 7) / 8,
 *  not bits / 8. A 15-bit pixel is a two-byte 5-5-5 word, so the old
 *  value (1) made every RLE packet consume one byte too few and the
 *  stream drifted from the second pixel on. The run sample's packet is
 *  exactly 3 bytes on disk (1 header + 2 pixel bytes), so the test also
 *  asserts the reader is exactly exhausted: a 1-byte-per-pixel decoder
 *  cannot land on the right stream position.
 *
 *  Samples (tests/samples/, 15-bit 5-5-5 little-endian word; the reader
 *  unpacks b = (v & 0x1f) << 3, g = ((v >> 5) & 0x1f) << 3,
 *  r = ((v >> 10) & 0x1f) << 3; stored order is B, G, R):
 *    tga15_rle_run.tga  4x1, one run packet (0x83) of pixel 0x7C00
 *                       -> (B,G,R) (0, 0, 248) x 4
 *    tga15_rle_raw.tga  2x1, one raw packet (0x01) of 0x7C00, 0x03E0
 *                       -> (0, 0, 248), (0, 248, 0)
 *  Both samples are 1 row, so the product's vertical flip (descriptor
 *  bit 5) does not apply here; this test covers the packet decoder
 *  only, not the file-level header checks, flip or alpha handling -
 *  those still need a Windows device test through the real loader.
 *
 *  Build & run (from the repository root, Linux; no Windows/MFC needed):
 *      g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/loader_tga_rle_test.cpp -o /tmp/loader_tga_rle_test
 *      /tmp/loader_tga_rle_test [tests/samples]
 *  Exit code 0 = all checks passed, 1 = at least one failed.
 */

#include "TgaRleDecode.h"

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
    } else {
        std::printf("ok    %s\n", what);
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

// Same formula as Helpers::DoPadding(value, 4) (Helpers.h); Helpers.h
// itself is not Linux-compilable (LPCTSTR etc.), so it is restated here.
size_t PaddedStride(int width, int targetChannels) {
    const int bytes = width * targetChannels;
    return static_cast<size_t>((bytes + 3) & ~3);
}

struct TgaHeader {
    int idLen = 0, cmapType = 0, imageType = 0;
    int cmapLen = 0, cmapBits = 0;
    int width = 0, height = 0, bits = 0, desc = 0;
};

bool ParseHeader(const std::vector<unsigned char>& data, TgaHeader& h, size_t& payloadOff) {
    if (data.size() < 18) return false;
    h.idLen = data[0];
    h.cmapType = data[1];
    h.imageType = data[2];
    h.cmapLen = data[5] | (data[6] << 8);
    h.cmapBits = data[7];
    h.width = data[12] | (data[13] << 8);
    h.height = data[14] | (data[15] << 8);
    h.bits = data[16];
    h.desc = data[17];
    payloadOff = 18 + static_cast<size_t>(h.idLen) +
        static_cast<size_t>(h.cmapLen) * static_cast<size_t>((h.cmapBits + 7) / 8);
    return payloadOff <= data.size();
}

// Decodes one sample file through the real packet decoder and checks the
// BGR bytes (plus exact stream consumption, which is what pins F-04).
void CheckSample(const std::string& path, const unsigned char* wantPixels,
                 size_t wantBytes) {
    std::vector<unsigned char> file;
    char what[256];
    std::snprintf(what, sizeof(what), "read %s", path.c_str());
    if (!ReadFile(path, file)) {
        Check(false, what);
        return;
    }
    TgaHeader h;
    size_t payloadOff = 0;
    std::snprintf(what, sizeof(what), "%s: 18-byte header parses", path.c_str());
    if (!ParseHeader(file, h, payloadOff)) {
        Check(false, what);
        return;
    }
    std::snprintf(what, sizeof(what), "%s: type 10 RLE, 15-bit, no colormap",
                  path.c_str());
    Check(h.imageType == 10 && h.bits == 15 && h.cmapType == 0 && h.idLen == 0, what);
    const int targetChannels = 3;
    const size_t stride = PaddedStride(h.width, targetChannels);
    std::vector<unsigned char> out(stride * static_cast<size_t>(h.height), 0);
    TgaRle::MemoryReader reader(file.data() + payloadOff, file.size() - payloadOff);
    const bool ok = TgaRle::DecodeRlePackets(reader, h.bits, false, h.width, h.height,
                                             targetChannels, stride, nullptr, out.data());
    std::snprintf(what, sizeof(what), "%s: decode succeeds", path.c_str());
    Check(ok, what);
    if (!ok) return;
    std::snprintf(what, sizeof(what), "%s: stream consumed exactly (%zu bytes)",
                  path.c_str(), file.size() - payloadOff);
    Check(reader.bytesConsumed() == file.size() - payloadOff, what);
    std::snprintf(what, sizeof(what), "%s: pixels match (BGR)", path.c_str());
    Check(std::memcmp(out.data(), wantPixels, wantBytes) == 0, what);
}

void TestSamples(const std::string& dir) {
    // tga15_rle_run.tga: 4x1, (B,G,R) (0,0,248) x 4.
    const unsigned char runWant[] = {0, 0, 248, 0, 0, 248, 0, 0, 248, 0, 0, 248};
    CheckSample(dir + "/tga15_rle_run.tga", runWant, sizeof(runWant));
    // tga15_rle_raw.tga: 2x1, (0,0,248), (0,248,0); stride is 8, so the
    // row carries 2 padding bytes after the 6 pixel bytes.
    const unsigned char rawWant[] = {0, 0, 248, 0, 248, 0, 0, 0};
    CheckSample(dir + "/tga15_rle_raw.tga", rawWant, sizeof(rawWant));
}

void TestTruncated(const std::string& dir) {
    // The run sample minus its last byte: the run packet's pixel is
    // short, so the decode must fail (F-13) instead of emitting pixels.
    std::vector<unsigned char> file;
    if (!ReadFile(dir + "/tga15_rle_run.tga", file)) {
        Check(false, "read tga15_rle_run.tga for truncation case");
        return;
    }
    file.pop_back();
    TgaHeader h;
    size_t payloadOff = 0;
    if (!ParseHeader(file, h, payloadOff)) {
        Check(false, "truncated: header parses");
        return;
    }
    std::vector<unsigned char> out(PaddedStride(h.width, 3) *
                                   static_cast<size_t>(h.height), 0);
    TgaRle::MemoryReader reader(file.data() + payloadOff, file.size() - payloadOff);
    Check(!TgaRle::DecodeRlePackets(reader, h.bits, false, h.width, h.height,
                                    3, PaddedStride(h.width, 3), nullptr, out.data()),
          "truncated run packet -> decode fails");
}

// In-memory packet streams for the branches the two samples do not
// cover (24-bit raw+run mix with row padding, 32-bit run, 8-bit grey
// and 8-bit indexed). Bytes are BGR in file order, as the reader stores.
void TestInMemory() {
    // 24-bit, 3x2: raw packet of 3 pixels, then a run packet of 3.
    // Stride is 12 (9 pixel bytes + 3 padding), padding stays zero.
    {
        const unsigned char payload[] = {
            0x02,                               // raw, 3 pixels
            1, 2, 3, 4, 5, 6, 7, 8, 9,          // BGR x 3
            0x82,                               // run, 3 copies
            10, 11, 12};                        // BGR
        const unsigned char want[] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0,
            10, 11, 12, 10, 11, 12, 10, 11, 12, 0, 0, 0};
        std::vector<unsigned char> out(sizeof(want), 0);
        TgaRle::MemoryReader reader(payload, sizeof(payload));
        const bool ok = TgaRle::DecodeRlePackets(reader, 24, false, 3, 2,
                                                 3, 12, nullptr, out.data());
        Check(ok, "in-memory 24-bit raw+run: decode succeeds");
        Check(ok && reader.bytesConsumed() == sizeof(payload),
              "in-memory 24-bit raw+run: stream consumed exactly");
        Check(ok && std::memcmp(out.data(), want, sizeof(want)) == 0,
              "in-memory 24-bit raw+run: pixels + row padding match");
    }
    // 32-bit, 2x1: run packet of 2, stored little-endian uint32.
    {
        const unsigned char payload[] = {0x81, 0x11, 0x22, 0x33, 0x44};
        const unsigned char want[] = {0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44};
        std::vector<unsigned char> out(sizeof(want), 0);
        TgaRle::MemoryReader reader(payload, sizeof(payload));
        const bool ok = TgaRle::DecodeRlePackets(reader, 32, false, 2, 1,
                                                 4, 8, nullptr, out.data());
        Check(ok, "in-memory 32-bit run: decode succeeds");
        Check(ok && std::memcmp(out.data(), want, sizeof(want)) == 0,
              "in-memory 32-bit run: uint32 pixels match");
    }
    // 8-bit grey, 2x1: raw packet, each byte expands to (v, v, v).
    {
        const unsigned char payload[] = {0x01, 17, 34};
        const unsigned char want[] = {17, 17, 17, 34, 34, 34, 0, 0};
        std::vector<unsigned char> out(sizeof(want), 0);
        TgaRle::MemoryReader reader(payload, sizeof(payload));
        const bool ok = TgaRle::DecodeRlePackets(reader, 8, false, 2, 1,
                                                 3, 8, nullptr, out.data());
        Check(ok, "in-memory 8-bit grey raw: decode succeeds");
        Check(ok && std::memcmp(out.data(), want, sizeof(want)) == 0,
              "in-memory 8-bit grey raw: grey expansion matches");
    }
    // 8-bit indexed, 2x1: run packet of index 1; palette[1] is (10,20,30).
    {
        unsigned char palette[768] = {0};
        palette[3] = 10; palette[4] = 20; palette[5] = 30;
        const unsigned char payload[] = {0x81, 0x01};
        const unsigned char want[] = {10, 20, 30, 10, 20, 30, 0, 0};
        std::vector<unsigned char> out(sizeof(want), 0);
        TgaRle::MemoryReader reader(payload, sizeof(payload));
        const bool ok = TgaRle::DecodeRlePackets(reader, 8, true, 2, 1,
                                                 3, 8, palette, out.data());
        Check(ok, "in-memory 8-bit indexed run: decode succeeds");
        Check(ok && std::memcmp(out.data(), want, sizeof(want)) == 0,
              "in-memory 8-bit indexed run: palette lookup matches");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    std::printf("loader_tga_rle_test (samples: %s)\n", dir.c_str());
    TestSamples(dir);
    TestTruncated(dir);
    TestInMemory();

    if (g_failures == 0) {
        std::printf("PASS  %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d of %d checks\n", g_failures, g_checks);
    return 1;
}
