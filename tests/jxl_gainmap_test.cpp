// jxl_gainmap_test.cpp - JXL gain-map (jhgm) extraction test.
//
// What this pins, and what it does not:
// --------------------------------------
// The jhgm box is read from the file buffer (not from decoder box
// events: for stills the box arrives after FULL_IMAGE) and its bundle
// layout is parsed in-tree (the vendored libjxl binaries do not
// export JxlGainMapReadBundle). Both are pure C++ in GainMap.h/.cpp,
// so they are pinned here against the REAL sample file
// (tests/samples/trattore-gainmap.jxl, Chromium's test file,
// 134x100 SDR base + 33x25 gain map, headroom 4x):
//   T0: the box walk finds the jhgm box in the real file (box 458 at
//       offset 5366, payload 450 at 5374) and handles bare codestreams,
//       truncated headers/bodies, size==0, largesize and malformed
//       sizes (2..7, largesize < 16).
//   T1: the bundle parse splits the real 450-byte payload into
//       metadata (141), color encoding (3 bytes 50 b4 00), alt ICC
//       (0) and the nested codestream (298 bytes, FF 0A), and
//       rejects truncated / wrong-version / empty-codestream inputs.
//   T2: the URN-less ISO metadata parse yields the real values
//       (base 0 / alt 2 / min 0 / max 2 / gamma 1 / offsets 0 /
//       multiChannel + useBaseColorSpace), and the URN form of the
//       same bytes parses identically (the two entries share the
//       core, R1 option A).
//   T3: the nested codestream decodes (system libjxl) to 33x25 gray,
//       min 2, max 255, all 825 pixels nonzero.
//   T4: end-to-end assembly — the real base decoded with system
//       libjxl plus the real gain + metadata through
//       ApplyGainMapToBgra matches the Python reference
//       (~/workspace/gainmap_reference.py) bit-exactly on 10 pixels.
// This test does NOT cover the Windows-only wiring (the temp-decoder
// call inside JXLWrapper.cpp and the ImageLoadThread hookup): those
// are covered by the Windows CI build and the device test. The
// decode helper below mirrors the production temp-decoder loop,
// including the M-3 out-buffer return-value checks.
//
// Build and run (Linux):
//   g++ -std=c++17 -Wall -Wextra -O2 -DJXL_STATIC_DEFINE -I src/JPEGView
//       -I src/JPEGView/libjxl/include tests/jxl_gainmap_test.cpp
//       src/JPEGView/GainMap.cpp src/JPEGView/HdrToneMap.cpp
//       -l:libjxl.so.0.7 -o /tmp/jxl_gainmap_test
//   /tmp/jxl_gainmap_test [tests/samples]
// (CI links plain -ljxl after installing libjxl-dev, as in the
// jxl-frame-header job; the exercised decode symbols are ABI-stable
// across the vendored 0.13 headers and system 0.7/0.11.)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GainMap.h"
#include "HdrToneMap.h"
#include "jxl/decode.h"

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

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
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

// Minimal JXL decode with a throwaway decoder, mirroring the
// production temp-decoder loop in JXLWrapper.cpp (DecodeJxlGainMapGray),
// including the M-3 out-buffer return-value checks and the iteration
// bound. `channels` is the requested output channel count (1 = gray,
// 4 = RGBA). Returns false on any failure; no hang on a 3-channel
// codestream asked for as gray (the buffer calls fail, we return).
bool DecodeJxl(const uint8_t* data, size_t size, int channels,
               std::vector<uint8_t>& pixels, int& w, int& h) {
    w = h = 0;
    JxlDecoder* decoder = JxlDecoderCreate(nullptr);
    if (decoder == nullptr) return false;
    bool ok = false;
    if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) ==
        JXL_DEC_SUCCESS) {
        JxlDecoderSetInput(decoder, data, size);
        JxlDecoderCloseInput(decoder);
        JxlPixelFormat format = {(uint32_t)channels, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        for (int iter = 0; iter < 64; iter++) {
            JxlDecoderStatus status = JxlDecoderProcessInput(decoder);
            if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) {
                break;
            } else if (status == JXL_DEC_BASIC_INFO) {
                JxlBasicInfo info;
                if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) break;
                if (info.xsize == 0 || info.ysize == 0) break;
                w = (int)info.xsize;
                h = (int)info.ysize;
            } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                size_t bufferSize = 0;
                if (JxlDecoderImageOutBufferSize(decoder, &format, &bufferSize) !=
                    JXL_DEC_SUCCESS) {
                    break;
                }
                if (w <= 0 || h <= 0 || bufferSize != (size_t)w * h * (size_t)channels) break;
                pixels.resize(bufferSize);
                if (JxlDecoderSetImageOutBuffer(decoder, &format, pixels.data(),
                                                 pixels.size()) != JXL_DEC_SUCCESS) {
                    break;
                }
            } else if (status == JXL_DEC_FULL_IMAGE) {
                ok = w > 0 && h > 0 &&
                     pixels.size() == (size_t)w * h * (size_t)channels;
                break;
            } else {
                break;
            }
        }
    }
    JxlDecoderDestroy(decoder);
    return ok;
}

// T0: box walk.
void TestBoxWalk(const std::vector<uint8_t>& file) {
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    Check(gainmap::FindJxlGainMapBox(file.data(), file.size(), payload, payloadSize),
          "T0: jhgm box found in the real file");
    Check(payload - file.data() == 5374, "T0: jhgm payload at offset 5374");
    Check(payloadSize == 450u, "T0: jhgm payload is 450 bytes");
    // The 8 bytes before the payload are the box header: size 458 + "jhgm".
    Check(file[5366] == 0x00 && file[5367] == 0x00 && file[5368] == 0x01 &&
              file[5369] == 0xCA && std::memcmp(&file[5370], "jhgm", 4) == 0,
          "T0: jhgm box header is size 458 + 'jhgm' at 5366");

    // Bare codestream: no boxes at all.
    const uint8_t bare[] = {0xFF, 0x0A, 0xC0, 0x30, 0x90, 0x00};
    Check(!gainmap::FindJxlGainMapBox(bare, sizeof(bare), payload, payloadSize),
          "T0: bare codestream has no jhgm box");
    // Empty / 1-byte buffer.
    Check(!gainmap::FindJxlGainMapBox(nullptr, 0, payload, payloadSize),
          "T0: null buffer has no jhgm box");
    Check(!gainmap::FindJxlGainMapBox(bare, 1, payload, payloadSize),
          "T0: 1-byte buffer has no jhgm box");
    // Truncated box header (claims 20 bytes, only 8 present).
    const uint8_t truncHead[] = {0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p'};
    Check(!gainmap::FindJxlGainMapBox(truncHead, sizeof(truncHead), payload, payloadSize),
          "T0: truncated box header is no jhgm box");
    // Truncated box body (12-byte JXL box, only 10 bytes present).
    const uint8_t truncBody[] = {0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A};
    Check(!gainmap::FindJxlGainMapBox(truncBody, sizeof(truncBody), payload, payloadSize),
          "T0: truncated box body is no jhgm box");
    // Malformed size 2..7.
    const uint8_t badSize[] = {0x00, 0x00, 0x00, 0x05, 'j', 'h', 'g', 'm'};
    Check(!gainmap::FindJxlGainMapBox(badSize, sizeof(badSize), payload, payloadSize),
          "T0: box size 5 is malformed");
    // Malformed largesize (< 16).
    const uint8_t badLarge[] = {0x00, 0x00, 0x00, 0x01, 'j', 'h', 'g', 'm',
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A};
    Check(!gainmap::FindJxlGainMapBox(badLarge, sizeof(badLarge), payload, payloadSize),
          "T0: largesize 10 is malformed");
    // Valid largesize (16, empty payload) is found with size 0.
    const uint8_t okLarge[] = {0x00, 0x00, 0x00, 0x01, 'j', 'h', 'g', 'm',
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    Check(gainmap::FindJxlGainMapBox(okLarge, sizeof(okLarge), payload, payloadSize) &&
              payloadSize == 0u,
          "T0: largesize 16 box found with empty payload");
    // size == 0 extends to EOF: a non-jhgm size-0 box swallows the rest.
    const uint8_t sizeZero[] = {0x00, 0x00, 0x00, 0x00, 'x', 'x', 'x', 'x',
                                0x00, 0x00, 0x00, 0x0C, 'j', 'h', 'g', 'm'};
    Check(!gainmap::FindJxlGainMapBox(sizeZero, sizeof(sizeZero), payload, payloadSize),
          "T0: size-0 box extends to EOF, no jhgm after it");
    // size == 0 jhgm box: payload is the rest of the buffer.
    const uint8_t sizeZeroJhgm[] = {0x00, 0x00, 0x00, 0x00, 'j', 'h', 'g', 'm', 'A', 'B'};
    Check(gainmap::FindJxlGainMapBox(sizeZeroJhgm, sizeof(sizeZeroJhgm), payload,
                                    payloadSize) &&
              payloadSize == 2u && payload[0] == 'A' && payload[1] == 'B',
          "T0: size-0 jhgm box payload is the rest of the buffer");
    // Empty (size 8) box is skipped; the jhgm after it is found.
    const uint8_t emptyFirst[] = {0x00, 0x00, 0x00, 0x08, 'x', 'x', 'x', 'x',
                                  0x00, 0x00, 0x00, 0x0C, 'j', 'h', 'g', 'm',
                                  'A', 'B', 'C', 'D'};
    Check(gainmap::FindJxlGainMapBox(emptyFirst, sizeof(emptyFirst), payload,
                                    payloadSize) &&
              payloadSize == 4u && std::memcmp(payload, "ABCD", 4) == 0,
          "T0: empty box skipped, jhgm after it found");
    // Only the first jhgm box is returned.
    const uint8_t twoJhgm[] = {0x00, 0x00, 0x00, 0x0A, 'j', 'h', 'g', 'm', 'A', 'B',
                               0x00, 0x00, 0x00, 0x0A, 'j', 'h', 'g', 'm', 'C', 'D'};
    Check(gainmap::FindJxlGainMapBox(twoJhgm, sizeof(twoJhgm), payload, payloadSize) &&
              payloadSize == 2u && payload[0] == 'A',
          "T0: only the first jhgm box is returned");
}

// T1: bundle parse on the real payload.
void TestBundleParse(const uint8_t* payload, size_t payloadSize,
                     const uint8_t*& meta, size_t& metaSize,
                     const uint8_t*& code, size_t& codeSize) {
    Check(gainmap::ParseJxlGainMapBundle(payload, payloadSize, meta, metaSize, code, codeSize),
          "T1: real jhgm payload parses");
    Check(metaSize == 141, "T1: metadata is 141 bytes");
    Check(meta[0] == 0x00 && meta[1] == 0x00 && meta[2] == 0x00 && meta[3] == 0x00 &&
              meta[4] == 0xC0,
          "T1: metadata starts min_ver 0, writer_ver 0, flags 0xC0 (no URN)");
    Check(codeSize == 298, "T1: gain codestream is 298 bytes");
    Check(code[0] == 0xFF && code[1] == 0x0A, "T1: gain codestream starts FF 0A");
    // The color encoding (3 bytes 50 b4 00) sits between metadata and
    // the alt-ICC length: payload[1+2+141] == 3, then 50 b4 00, then
    // the u32 ICC length 0.
    Check(payload[144] == 3 && payload[145] == 0x50 && payload[146] == 0xB4 &&
              payload[147] == 0x00 && payload[148] == 0x00 && payload[149] == 0x00 &&
              payload[150] == 0x00 && payload[151] == 0x00,
          "T1: color encoding 3 bytes 50 b4 00, alt-ICC length 0");
    // Malformed inputs.
    const uint8_t* m2 = nullptr;
    size_t m2Size = 0;
    const uint8_t* c2 = nullptr;
    size_t c2Size = 0;
    Check(!gainmap::ParseJxlGainMapBundle(payload, 10, m2, m2Size, c2, c2Size),
          "T1: 10-byte payload rejected");
    std::vector<uint8_t> badVer(payload, payload + payloadSize);
    badVer[0] = 1;
    Check(!gainmap::ParseJxlGainMapBundle(badVer.data(), badVer.size(), m2, m2Size, c2, c2Size),
          "T1: version 1 rejected");
    // Truncated right after the color-encoding length byte: the
    // 3-byte color encoding runs past the end.
    Check(!gainmap::ParseJxlGainMapBundle(payload, 146, m2, m2Size, c2, c2Size),
          "T1: color encoding truncated rejected");
    // Truncated right after the alt-ICC length: empty codestream.
    Check(!gainmap::ParseJxlGainMapBundle(payload, 152, m2, m2Size, c2, c2Size),
          "T1: empty gain codestream rejected");
    Check(!gainmap::ParseJxlGainMapBundle(nullptr, 0, m2, m2Size, c2, c2Size),
          "T1: null payload rejected");
}

// T2: URN-less ISO metadata parse on the real 141 bytes.
void TestMetadataNoUrn(const uint8_t* meta, size_t metaSize, gainmap::Metadata& out) {
    Check(gainmap::ParseIso21496MetadataNoUrn(meta, metaSize, out),
          "T2: real 141-byte metadata parses (no URN)");
    Check(out.valid, "T2: metadata valid");
    Check(out.baseHdrHeadroom == 0.0, "T2: baseHdrHeadroom 0 (SDR base)");
    Check(out.alternateHdrHeadroom == 2.0, "T2: alternateHdrHeadroom 2.0 (4x)");
    Check(out.gainMapMin == 0.0, "T2: gainMapMin 0");
    Check(out.gainMapMax == 2.0, "T2: gainMapMax 2.0");
    Check(out.gamma == 1.0, "T2: gamma 1.0");
    Check(out.baseOffset == 0.0 && out.alternateOffset == 0.0, "T2: offsets 0");
    Check(out.multiChannel, "T2: multiChannel (flags bit 7)");
    Check(out.useBaseColorSpace, "T2: useBaseColorSpace (flags bit 6)");
    // The URN form of the same bytes parses identically: the two
    // entries share the core (R1 option A).
    std::vector<uint8_t> withUrn;
    const char urn[] = "urn:iso:std:iso:ts:21496:-1";
    withUrn.insert(withUrn.end(), urn, urn + 27);
    withUrn.push_back(0);
    withUrn.insert(withUrn.end(), meta, meta + metaSize);
    gainmap::Metadata outUrn;
    Check(gainmap::ParseIso21496Metadata(withUrn.data(), withUrn.size(), outUrn) &&
              outUrn.valid && outUrn.baseHdrHeadroom == out.baseHdrHeadroom &&
              outUrn.alternateHdrHeadroom == out.alternateHdrHeadroom &&
              outUrn.gainMapMin == out.gainMapMin && outUrn.gainMapMax == out.gainMapMax &&
              outUrn.gamma == out.gamma && outUrn.multiChannel == out.multiChannel &&
              outUrn.useBaseColorSpace == out.useBaseColorSpace,
          "T2: URN form of the same bytes parses identically");
    // Minimal 61-byte body: flags 0, all rationals 0/1 except gamma 1/1.
    uint8_t minimal[61] = {};
    minimal[4] = 0;  // flags
    for (int k = 0; k < 7; k++) {
        minimal[5 + k * 8 + 3] = 0;      // numerator 0 (BE i32, high bytes 0)
        minimal[5 + k * 8 + 7] = 1;      // denominator 1 (BE u32)
    }
    minimal[5 + 4 * 8 + 3] = 1;  // gamma numerator 1
    gainmap::Metadata outMin;
    Check(gainmap::ParseIso21496MetadataNoUrn(minimal, sizeof(minimal), outMin) &&
              outMin.valid && outMin.gamma == 1.0 && !outMin.multiChannel,
          "T2: minimal 61-byte body parses");
    // 60 bytes is one short.
    Check(!gainmap::ParseIso21496MetadataNoUrn(minimal, 60, outMin),
          "T2: 60-byte body rejected");
    // Zero denominator (gamma's) rejected.
    std::vector<uint8_t> zeroDen(meta, meta + metaSize);
    zeroDen[5 + 4 * 8 + 4] = 0;
    zeroDen[5 + 4 * 8 + 5] = 0;
    zeroDen[5 + 4 * 8 + 6] = 0;
    zeroDen[5 + 4 * 8 + 7] = 0;
    Check(!gainmap::ParseIso21496MetadataNoUrn(zeroDen.data(), zeroDen.size(), outMin),
          "T2: zero denominator rejected");
    // Gamma 0 rejected (numerator 0, denominator 1).
    std::vector<uint8_t> zeroGamma(meta, meta + metaSize);
    zeroGamma[5 + 4 * 8 + 0] = 0;
    zeroGamma[5 + 4 * 8 + 1] = 0;
    zeroGamma[5 + 4 * 8 + 2] = 0;
    zeroGamma[5 + 4 * 8 + 3] = 0;
    Check(!gainmap::ParseIso21496MetadataNoUrn(zeroGamma.data(), zeroGamma.size(), outMin),
          "T2: gamma 0 rejected");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    std::vector<uint8_t> file;
    if (!ReadFile(dir + "/trattore-gainmap.jxl", file)) {
        std::printf("jxl_gainmap_test: cannot read %s/trattore-gainmap.jxl\n", dir.c_str());
        return 2;
    }
    Check(file.size() == 5824u, "sample file is 5824 bytes");

    TestBoxWalk(file);

    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    Check(gainmap::FindJxlGainMapBox(file.data(), file.size(), payload, payloadSize),
          "jhgm box found for T1/T2/T3");
    if (payload == nullptr) {
        std::printf("jxl_gainmap_test: %d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    const uint8_t* meta = nullptr;
    size_t metaSize = 0;
    const uint8_t* code = nullptr;
    size_t codeSize = 0;
    TestBundleParse(payload, payloadSize, meta, metaSize, code, codeSize);

    gainmap::Metadata md;
    TestMetadataNoUrn(meta, metaSize, md);

    // T3: decode the nested codestream to gray.
    std::vector<uint8_t> gray;
    int gw = 0, gh = 0;
    Check(DecodeJxl(code, codeSize, 1, gray, gw, gh), "T3: nested codestream decodes");
    Check(gw == 33 && gh == 25, "T3: gain map is 33x25");
    Check(gray.size() == 825u, "T3: 825 gain pixels");
    if (gray.size() == 825u) {
        const auto mm = std::minmax_element(gray.begin(), gray.end());
        Check(*mm.first == 2, "T3: gain min is 2");
        Check(*mm.second == 255, "T3: gain max is 255");
        Check(std::all_of(gray.begin(), gray.end(), [](uint8_t v) { return v != 0; }),
              "T3: all gain pixels nonzero");
    }

    // T4: end-to-end assembly — real base + real gain + real metadata
    // through ApplyGainMapToBgra, against the Python reference
    // (~/workspace/gainmap_reference.py, bit-exact on all 10 pixels).
    std::vector<uint8_t> base;
    int bw = 0, bh = 0;
    Check(DecodeJxl(file.data(), file.size(), 4, base, bw, bh),
          "T4: real base decodes (RGBA)");
    Check(bw == 134 && bh == 100, "T4: base is 134x100");
    if (bw == 134 && bh == 100 && gw == 33 && gh == 25) {
        std::vector<uint8_t> out(base.size());
        Check(gainmap::ApplyGainMapToBgra(base.data(), bw, bh, (size_t)bw * 4,
                                          gray.data(), gw, gh, md, 1.0,
                                          out.data(), (size_t)bw * 4),
              "T4: ApplyGainMapToBgra runs");
        // {x, y, B, G, R, A} from the Python reference.
        static const int kWant[10][6] = {
            {0, 0, 172, 208, 232, 255},   {133, 0, 125, 137, 136, 255},
            {0, 99, 84, 83, 76, 255},     {133, 99, 121, 109, 93, 255},
            {67, 50, 171, 162, 157, 255}, {20, 30, 65, 73, 59, 255},
            {100, 70, 149, 133, 124, 255}, {33, 12, 204, 225, 237, 255},
            {90, 20, 235, 234, 228, 255}, {10, 80, 50, 54, 56, 255},
        };
        for (int i = 0; i < 10; i++) {
            const int x = kWant[i][0], y = kWant[i][1];
            const uint8_t* px = out.data() + ((size_t)y * bw + x) * 4;
            char what[64];
            std::snprintf(what, sizeof(what), "T4: pixel (%d,%d) matches Python reference",
                          x, y);
            Check(px[0] == kWant[i][2] && px[1] == kWant[i][3] &&
                      px[2] == kWant[i][4] && px[3] == kWant[i][5],
                  what);
        }
        // The gain is active: the brightest-gain pixel differs from SDR.
        const uint8_t* px = out.data() + ((size_t)50 * bw + 67) * 4;
        const uint8_t* sd = base.data() + ((size_t)50 * bw + 67) * 4;
        Check(px[0] != sd[0] || px[1] != sd[1] || px[2] != sd[2],
              "T4: HDR pixel differs from the SDR base");
    }

    std::printf("jxl_gainmap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
