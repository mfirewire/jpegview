// gainmap_test.cpp - unit test for the shared gain-map helper
// (GainMap.h/.cpp, Option D Phase 1: ISO 21496-1 / UltraHDR).
//
// What this pins, and what it does not:
// --------------------------------------
// The reconstruction formula is the ISO 21496-1 one (logRecovery,
// logBoost, gain = 2^logBoost, HDR = (SDR + baseOffset) * gain -
// alternateOffset) with the sRGB EOTF inverse. A wrong constant or a
// wrong byte order in the ISO parser is internally consistent - the
// code compiles and runs, and the picture comes out uniformly wrong -
// so it cannot be caught by reading the code. These tests pin:
//   * the ISO parser against the REAL 89-byte APP2 payload extracted
//     from test_uhdr_hlg.jpg's gain-map JPEG (see kIsoPayloadHlg),
//   * the XMP parser against synthetic UltraHDR and Apple packets,
//   * FindGainMapJpeg / FindIso21496Payload against the REAL MPF
//     segments of test_uhdr_hlg.jpg / test_uhdr_pq.jpg (offset 8087,
//     sizes 8277/37284) and the REAL ISO payload bytes.
//   * ApplyGainMap against values computed independently in Python
//     from the same formula (~/workspace/gainmap_reference.py).
// This test is pure math: it needs no image files and no Windows
// toolchain. The end-to-end check against the actual test images
// (mean abs diff vs test_reference.jpg 0.312 HLG / 0.104 PQ) is the
// JPEG phase's job, not this test's.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/gainmap_test.cpp src/JPEGView/GainMap.cpp
//       -o /tmp/gainmap_test
//   /tmp/gainmap_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GainMap.h"

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

// The real 89-byte ISO 21496-1 APP2 payload from the gain-map JPEG
// inside test_uhdr_hlg.jpg (kennylevinsen/hdr-test-images), extracted
// with ~/workspace/gainmap_reference.py (parse_mpf_gainmap_offset +
// APP2 scan). Do NOT hand-edit: the parse test below verifies these
// bytes against the independently computed values.
static const uint8_t kIsoPayloadHlg[89] = {
    0x75, 0x72, 0x6e, 0x3a, 0x69, 0x73, 0x6f, 0x3a, 0x73, 0x74, 0x64, 0x3a,
    0x69, 0x73, 0x6f, 0x3a, 0x74, 0x73, 0x3a, 0x32, 0x31, 0x34, 0x39, 0x36,
    0x3a, 0x2d, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x24, 0xce, 0xa3, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xcc, 0xcc,
    0xc3, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x4d, 0x2d, 0xe5, 0x44, 0x77, 0x00, 0x00, 0x00,
    0x4d, 0x2d, 0xe5, 0x44, 0x77
};


// The REAL MPF APP2 segments (marker + length + payload, 90 bytes)
// from test_uhdr_hlg.jpg / test_uhdr_pq.jpg (MPF marker at file
// offset 644, TIFF "MM" header at 652, IFD at 660). The two segments
// differ only in the gain-map size field of MP entry 1 (8277 vs
// 37284); data_offset is 7435 in both. Do NOT hand-edit: the tests
// below verify these bytes against the known offsets/sizes.
static const uint8_t kMpfSegmentHlg[90] = {
    0xff, 0xe2, 0x00, 0x58, 0x4d, 0x50, 0x46, 0x00, 0x4d, 0x4d, 0x00, 0x2a,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xb0, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x04, 0x30, 0x31, 0x30, 0x30, 0xb0, 0x01, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xb0, 0x02, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x1f, 0x97, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x55, 0x00, 0x00,
    0x1d, 0x0b, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t kMpfSegmentPq[90] = {
    0xff, 0xe2, 0x00, 0x58, 0x4d, 0x50, 0x46, 0x00, 0x4d, 0x4d, 0x00, 0x2a,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xb0, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x04, 0x30, 0x31, 0x30, 0x30, 0xb0, 0x01, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xb0, 0x02, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x1f, 0x97, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x91, 0xa4, 0x00, 0x00,
    0x1d, 0x0b, 0x00, 0x00, 0x00, 0x00,
};

// The REAL header (SOI .. SOS, 609 bytes) of test_reference.jpg —
// a plain SDR JPEG with no MPF segment. Used as the no-gain-map
// negative case for FindGainMapJpeg / FindIso21496Payload.
static const uint8_t kSdrJpegHeader[609] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0xff, 0xdb, 0x00, 0x43, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x01, 0x40, 0x02, 0x80, 0x03,
    0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xff, 0xc4, 0x00,
    0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00,
    0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81,
    0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
    0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
    0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xff, 0xc4, 0x00,
    0x1f, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0xff, 0xc4, 0x00, 0xb5, 0x11, 0x00,
    0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31,
    0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08,
    0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15,
    0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55,
    0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

// Build a synthetic UltraHDR JPEG around a REAL MPF segment: SOI, a
// filler APP0 sized so the MPF marker lands at the real file offset
// 644, the MPF segment, zeros up to 8087, an FFD8 at 8087, then zeros
// to 8087+gainSize. Mirrors the real file layout (primary EOI 8085,
// gain-map JPEG at 8087) without embedding the whole 16/45 KB file.
std::vector<uint8_t> BuildUhdrJpeg(const uint8_t* mpfSeg, size_t gainSize) {
    std::vector<uint8_t> buf(8087 + gainSize, 0);
    buf[0] = 0xFF;
    buf[1] = 0xD8;
    // Filler APP0: marker (2) + length (2) + payload (638) = 642
    // bytes, so the MPF marker lands at 2 + 642 = 644.
    buf[2] = 0xFF;
    buf[3] = 0xE0;
    buf[4] = 0x02;  // length 640 = 0x0280
    buf[5] = 0x80;
    std::memcpy(buf.data() + 644, mpfSeg, 90);
    buf[8087] = 0xFF;  // gain-map JPEG SOI
    buf[8088] = 0xD8;
    return buf;
}

// Build a little-endian copy of the REAL big-endian MPF segment
// (P3-A8 (1)): the TIFF header and all multi-byte TIFF fields are
// byte-swapped; the JPEG APP2 length and the ASCII "0100" version
// stay as-is (the length is JPEG big-endian by spec, the version is
// a byte string, not a number). No real little-endian MPF file was
// on hand - all real UltraHDR MPFs are big-endian "MM" - so this is
// a synthetic mutation of the real segment, not a real file.
std::vector<uint8_t> MakeLittleEndianMpf() {
    std::vector<uint8_t> le(kMpfSegmentHlg,
                            kMpfSegmentHlg + sizeof(kMpfSegmentHlg));
    auto swap16 = [&le](size_t off) {
        uint8_t t = le[off];
        le[off] = le[off + 1];
        le[off + 1] = t;
    };
    auto swap32 = [&le](size_t off) {
        uint8_t t0 = le[off], t1 = le[off + 1];
        le[off] = le[off + 3];
        le[off + 1] = le[off + 2];
        le[off + 2] = t1;
        le[off + 3] = t0;
    };
    le[8] = 'I';
    le[9] = 'I';  // TIFF byte order
    swap16(10);   // magic 42
    swap32(12);   // IFD offset
    swap16(16);   // entry count
    // Entry 0 (B000): tag, type, count; "0100" value is ASCII.
    swap16(18);
    swap16(20);
    swap32(22);
    // Entry 1 (B001 NumberOfImages): tag, type, count, value.
    swap16(30);
    swap16(32);
    swap32(34);
    swap32(38);
    // Entry 2 (B002 MP list): tag, type, count, value offset.
    swap16(42);
    swap16(44);
    swap32(46);
    swap32(50);
    // MP entry 0: attributes, size; data offset is 0.
    swap32(58);
    swap32(62);
    // MP entry 1: attributes are 0; size, data offset.
    swap32(78);
    swap32(82);
    return le;
}

void TestParseIsoReal() {
    using namespace gainmap;
    Metadata m;
    Check(ParseIso21496Metadata(kIsoPayloadHlg, sizeof(kIsoPayloadHlg), m),
          "ISO parse real HLG payload succeeds");
    Check(m.valid, "ISO parse real HLG payload sets valid");
    // Rationals, written as the same num/den divisions the parser
    // does (values from gainmap_reference.py: altHeadroom 2.300448,
    // gainMapMax 0.099999927, offsets 1.0000000116883119e-07).
    Check(Near(m.baseHdrHeadroom, 0.0, 1e-12), "ISO baseHdrHeadroom 0.0");
    Check(Near(m.alternateHdrHeadroom, 2412195.0 / 1048576.0, 1e-12),
          "ISO alternateHdrHeadroom 2.300448");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "ISO gainMapMin 0.0");
    Check(Near(m.gainMapMax, 13421763.0 / 134217728.0, 1e-12),
          "ISO gainMapMax 0.1");
    Check(Near(m.gamma, 1.0, 1e-12), "ISO gamma 1.0");
    Check(Near(m.baseOffset, 77.0 / 769999991.0, 1e-12),
          "ISO baseOffset ~1e-07");
    Check(Near(m.alternateOffset, 77.0 / 769999991.0, 1e-12),
          "ISO alternateOffset ~1e-07");
    Check(!m.multiChannel, "ISO flags 0x00 -> single channel");
    Check(!m.useBaseColorSpace, "ISO flags 0x00 -> useBaseColorSpace false");
    Check(!m.appleHeadroomFallback, "ISO parse does not set Apple fallback");
    // P3-A8 (3): trailing data after the 7 rational pairs. A
    // multi-channel payload carries further channel sets after the
    // first; the parser reads only the first set and ignores the
    // rest ("further sets are ignored"). Here the trailing 56 bytes
    // are a second fake channel set (all pairs 2/1) - the parsed
    // values must still be the first set's (gamma 1.0, gainMapMax
    // 0.1), not the trailing set's (gamma 2.0, gainMapMax 2.0).
    uint8_t trailing[89 + 56];
    std::memcpy(trailing, kIsoPayloadHlg, 89);
    for (int k = 0; k < 7; ++k) {
        trailing[89 + k * 8 + 0] = 0x00;
        trailing[89 + k * 8 + 1] = 0x00;
        trailing[89 + k * 8 + 2] = 0x00;
        trailing[89 + k * 8 + 3] = 0x02;  // numerator 2
        trailing[89 + k * 8 + 4] = 0x00;
        trailing[89 + k * 8 + 5] = 0x00;
        trailing[89 + k * 8 + 6] = 0x00;
        trailing[89 + k * 8 + 7] = 0x01;  // denominator 1
    }
    Check(ParseIso21496Metadata(trailing, sizeof(trailing), m) && m.valid,
          "ISO payload with 56 trailing bytes parses");
    Check(Near(m.gamma, 1.0, 1e-12) &&
              Near(m.gainMapMax, 13421763.0 / 134217728.0, 1e-12),
          "ISO trailing second channel set ignored (first set wins)");
}

void TestParseIsoNegative() {
    using namespace gainmap;
    Metadata m;
    // Truncated payload (40 of 89 bytes) -> false.
    Check(!ParseIso21496Metadata(kIsoPayloadHlg, 40, m) && !m.valid,
          "ISO truncated payload (40B) rejected");
    // One byte short of the full layout -> false.
    Check(!ParseIso21496Metadata(kIsoPayloadHlg, 88, m) && !m.valid,
          "ISO payload 88B (one short) rejected");
    // Null / empty -> false.
    Check(!ParseIso21496Metadata(nullptr, sizeof(kIsoPayloadHlg), m) && !m.valid,
          "ISO null data rejected");
    Check(!ParseIso21496Metadata(kIsoPayloadHlg, 0, m) && !m.valid,
          "ISO empty payload rejected");
    // Wrong URN (first byte changed) -> false.
    uint8_t badUrn[89];
    std::memcpy(badUrn, kIsoPayloadHlg, sizeof(badUrn));
    badUrn[0] = 0x58;  // 'X' instead of 'u'
    Check(!ParseIso21496Metadata(badUrn, sizeof(badUrn), m) && !m.valid,
          "ISO wrong URN rejected");
    // Missing NUL after the URN (byte 27) -> false.
    uint8_t badNul[89];
    std::memcpy(badNul, kIsoPayloadHlg, sizeof(badNul));
    badNul[27] = 0x20;
    Check(!ParseIso21496Metadata(badNul, sizeof(badNul), m) && !m.valid,
          "ISO missing URN NUL rejected");
    // Gamma numerator zero (bytes 65-68, pair[4]) -> false.
    uint8_t badGamma[89];
    std::memcpy(badGamma, kIsoPayloadHlg, sizeof(badGamma));
    badGamma[65] = 0x00;
    badGamma[66] = 0x00;
    badGamma[67] = 0x00;
    badGamma[68] = 0x00;
    Check(!ParseIso21496Metadata(badGamma, sizeof(badGamma), m) && !m.valid,
          "ISO gamma 0 rejected");
    // Zero denominator (gainMapMax den, bytes 61-64, pair[3]) -> false.
    uint8_t badDen[89];
    std::memcpy(badDen, kIsoPayloadHlg, sizeof(badDen));
    badDen[61] = 0x00;
    badDen[62] = 0x00;
    badDen[63] = 0x00;
    badDen[64] = 0x00;
    Check(!ParseIso21496Metadata(badDen, sizeof(badDen), m) && !m.valid,
          "ISO zero denominator rejected");
    // Multi-channel flags (bit 7): first channel set still parses.
    uint8_t multi[89];
    std::memcpy(multi, kIsoPayloadHlg, sizeof(multi));
    multi[32] = 0x80;
    Check(ParseIso21496Metadata(multi, sizeof(multi), m) && m.valid &&
              m.multiChannel,
          "ISO flags 0x80 -> multiChannel, first set parsed");
    // useBaseColorSpace (bit 6).
    uint8_t baseCs[89];
    std::memcpy(baseCs, kIsoPayloadHlg, sizeof(baseCs));
    baseCs[32] = 0x40;
    Check(ParseIso21496Metadata(baseCs, sizeof(baseCs), m) && m.valid &&
              m.useBaseColorSpace && !m.multiChannel,
          "ISO flags 0x40 -> useBaseColorSpace");
}

// Synthetic UltraHDR XMP, rdf:Seq form (3 rationals per field, first wins).
const char* kXmpSeq =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" "
    "xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<hdrgm:Version>1.0</hdrgm:Version>"
    "<hdrgm:GainMapMin><rdf:Seq><rdf:li>0/1</rdf:li><rdf:li>0/1</rdf:li>"
    "<rdf:li>0/1</rdf:li></rdf:Seq></hdrgm:GainMapMin>"
    "<hdrgm:GainMapMax><rdf:Seq><rdf:li>1/10</rdf:li><rdf:li>1/10</rdf:li>"
    "<rdf:li>1/10</rdf:li></rdf:Seq></hdrgm:GainMapMax>"
    "<hdrgm:Gamma><rdf:Seq><rdf:li>1/1</rdf:li><rdf:li>1/1</rdf:li>"
    "<rdf:li>1/1</rdf:li></rdf:Seq></hdrgm:Gamma>"
    "<hdrgm:OffsetSDR><rdf:Seq><rdf:li>0/1</rdf:li><rdf:li>0/1</rdf:li>"
    "<rdf:li>0/1</rdf:li></rdf:Seq></hdrgm:OffsetSDR>"
    "<hdrgm:OffsetHDR><rdf:Seq><rdf:li>1/1000</rdf:li><rdf:li>1/1000</rdf:li>"
    "<rdf:li>1/1000</rdf:li></rdf:Seq></hdrgm:OffsetHDR>"
    "<hdrgm:HDRCapacityMin>0.0</hdrgm:HDRCapacityMin>"
    "<hdrgm:HDRCapacityMax>2.300448</hdrgm:HDRCapacityMax>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

// Synthetic UltraHDR XMP, plain single-value form.
const char* kXmpPlain =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" "
    "xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
    "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
    "<hdrgm:Gamma>1.0</hdrgm:Gamma>"
    "<hdrgm:OffsetSDR>0.0</hdrgm:OffsetSDR>"
    "<hdrgm:OffsetHDR>0.0</hdrgm:OffsetHDR>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

// Synthetic Apple HDRGainMap XMP (field names only). The headroom is
// a LINEAR RATIO (P2-A2 settled): gainMapMax = log2(headroom), so
// 2.0 -> 1.0 stops and 1.5 -> 0.585 stops; the version-only form
// below IS real (two iPhone 13 HEICs).
const char* kXmpApple =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<HDRGainMapHeadroom>2.0</HDRGainMapHeadroom>"
    "<HDRGainMapGamma>1.0</HDRGainMapGamma>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

const char* kXmpAppleNoGamma =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
    "<rdf:Description rdf:about=\"\">"
    "<HDRGainMapHeadroom>1.5</HDRGainMapHeadroom>"
    "</rdf:Description></rdf:RDF></x:xmpmeta>";

// The REAL version-only Apple XMP packet from the gain-map item (item
// 64, 363 bytes) of both iPhone 13 HEICs (2026-06-07 19.13.11.HEIC and
// 2026-04-05 16.21.46.HEIC), byte-for-byte as dumped with xmp_dump
// (libheif). It carries ONLY HDRGainMapVersion — no Headroom, no
// Gamma, no hdrgm. Do NOT hand-edit.
const char* kXmpAppleVersionOnly =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 6.0.0\">\n"
    "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "      <rdf:Description rdf:about=\"\"\n"
    "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
    "         <HDRGainMap:HDRGainMapVersion>65536</HDRGainMap:HDRGainMapVersion>\n"
    "      </rdf:Description>\n"
    "   </rdf:RDF>\n"
    "</x:xmpmeta>\n";

// REAL Apple XMP packets, byte-for-byte from the files (generated, do
// NOT hand-edit):
//   kXmpRealIphone15Pro: iPhone 15 Pro HEIC (PhotoPrism sample,
//     ~/workspace/.scratch-headroom-heic/iphone_15_pro.heic), 444 bytes,
//     HDRGainMapHeadroom 3.847906, Version 131072.
//   kXmpRealAppleJpegNew: Apple gain-map JPEG (libultrahdr sample,
//     ~/workspace/.scratch-headroom-heic/apple_gainmap_new.jpg),
//     726 bytes, Headroom 4.532783, Version 131072.
//   kXmpRealIphone15ProIos18: iPhone 15 Pro HEIC, iOS 18.0 (user
//     sample, ~/workspace/.scratch-b7-1-heic/iphone_15_pro_ios18.heic),
//     444 bytes, Headroom 2.769181, Version 131072.
const char* kXmpRealIphone15Pro =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 6.0.0\">\n"
    "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "      <rdf:Description rdf:about=\"\"\n"
    "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
    "         <HDRGainMap:HDRGainMapHeadroom>3.847906</HDRGainMap:HDRGainMapHeadroom>\n"
    "         <HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>\n"
    "      </rdf:Description>\n"
    "   </rdf:RDF>\n"
    "</x:xmpmeta>";

const char* kXmpRealAppleJpegNew =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 6.0.0\">\n"
    "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "      <rdf:Description rdf:about=\"\"\n"
    "            xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\"\n"
    "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
    "         <apdi:NativeFormat>1278226488</apdi:NativeFormat>\n"
    "         <apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</apdi:AuxiliaryImageType>\n"
    "         <apdi:StoredFormat>1278226488</apdi:StoredFormat>\n"
    "         <HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>\n"
    "         <HDRGainMap:HDRGainMapHeadroom>4.532783</HDRGainMap:HDRGainMapHeadroom>\n"
    "      </rdf:Description>\n"
    "   </rdf:RDF>\n"
    "</x:xmpmeta>";

const char* kXmpRealIphone15ProIos18 =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"XMP Core 6.0.0\">\n"
    "   <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "      <rdf:Description rdf:about=\"\"\n"
    "            xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">\n"
    "         <HDRGainMap:HDRGainMapVersion>131072</HDRGainMap:HDRGainMapVersion>\n"
    "         <HDRGainMap:HDRGainMapHeadroom>2.769181</HDRGainMap:HDRGainMapHeadroom>\n"
    "      </rdf:Description>\n"
    "   </rdf:RDF>\n"
    "</x:xmpmeta>";

void TestParseXmp() {
    using namespace gainmap;
    Metadata m;
    // rdf:Seq form.
    Check(ParseXmpMetadata(std::string(kXmpSeq), m) && m.valid,
          "XMP Seq form parses");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "XMP Seq gainMapMin 0.0");
    Check(Near(m.gainMapMax, 0.1, 1e-12), "XMP Seq gainMapMax 0.1");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP Seq gamma 1.0");
    Check(Near(m.baseOffset, 0.0, 1e-12), "XMP Seq baseOffset 0.0");
    Check(Near(m.alternateOffset, 0.001, 1e-12), "XMP Seq altOffset 0.001");
    Check(Near(m.baseHdrHeadroom, 0.0, 1e-12), "XMP Seq baseHdrHeadroom 0.0");
    Check(Near(m.alternateHdrHeadroom, 2.300448, 1e-12),
          "XMP Seq altHdrHeadroom 2.300448");
    Check(!m.appleHeadroomFallback, "XMP Seq is not Apple fallback");
    // Plain single-value form.
    Check(ParseXmpMetadata(std::string(kXmpPlain), m) && m.valid,
          "XMP plain form parses");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "XMP plain gainMapMin 0.0");
    Check(Near(m.gainMapMax, 0.1, 1e-12), "XMP plain gainMapMax 0.1");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP plain gamma 1.0");
    Check(Near(m.baseOffset, 0.0, 1e-12), "XMP plain baseOffset 0.0");
    Check(Near(m.alternateOffset, 0.0, 1e-12), "XMP plain altOffset 0.0");
    Check(Near(m.baseHdrHeadroom, 0.0, 1e-12),
          "XMP plain baseHdrHeadroom defaults 0.0");
    Check(Near(m.alternateHdrHeadroom, 0.0, 1e-12),
          "XMP plain altHdrHeadroom defaults 0.0");
    // Apple fallback, with and without gamma.
    Check(ParseXmpMetadata(std::string(kXmpApple), m) && m.valid,
          "XMP Apple fallback parses");
    Check(Near(m.gainMapMax, 1.0, 1e-12), "XMP Apple gainMapMax=log2(2.0)=1.0");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "XMP Apple gainMapMin 0.0");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP Apple gamma 1.0");
    Check(Near(m.baseOffset, 0.0, 1e-12), "XMP Apple baseOffset 0.0");
    Check(Near(m.alternateOffset, 0.0, 1e-12), "XMP Apple altOffset 0.0");
    Check(m.appleHeadroomFallback, "XMP Apple sets appleHeadroomFallback");
    Check(ParseXmpMetadata(std::string(kXmpAppleNoGamma), m) && m.valid,
          "XMP Apple without gamma parses");
    Check(Near(m.gainMapMax, 0.5849625007211562, 1e-12),
          "XMP Apple no-gamma gainMapMax=log2(1.5)=0.585");
    Check(Near(m.gamma, 1.0, 1e-12), "XMP Apple no-gamma defaults gamma 1.0");
    Check(m.appleHeadroomFallback, "XMP Apple no-gamma sets fallback flag");
    // The headroom is a LINEAR RATIO: below 1.0 is physically
    // meaningless (HDR dimmer than SDR) and above 65536 (16 stops) is
    // absurd — both are rejected. A ratio of exactly 1.0 is valid
    // with gainMapMax 0.0 (identity: no gain).
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>0.0</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio 0.0 rejected");
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>-1.5</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio -1.5 rejected");
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>0.5</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio 0.5 rejected");
    Check(ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>1.0</HDRGainMapHeadroom>"), m) &&
              m.valid,
          "XMP Apple headroom ratio 1.0 parses (identity)");
    Check(Near(m.gainMapMax, 0.0, 1e-12),
          "XMP Apple headroom ratio 1.0 -> gainMapMax 0.0");
    Check(ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>65536</HDRGainMapHeadroom>"), m) &&
              m.valid,
          "XMP Apple headroom ratio 65536 parses (16 stops)");
    Check(Near(m.gainMapMax, 16.0, 1e-12),
          "XMP Apple headroom ratio 65536 -> gainMapMax 16.0");
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>131072</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio 131072 rejected");
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>1000000</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio 1e6 rejected");
    Check(!ParseXmpMetadata(
              std::string("<HDRGainMapHeadroom>inf</HDRGainMapHeadroom>"), m) &&
              !m.valid,
          "XMP Apple headroom ratio inf rejected");
    // Version-only Apple XMP (real iPhone 13 HEICs) carries no Headroom
    // and does not parse — those files show SDR; there is no deriving.
    Check(!ParseXmpMetadata(std::string(kXmpAppleVersionOnly), m) && !m.valid,
          "XMP Apple version-only does not parse (no headroom)");
    // Negative: no gain-map fields at all -> false.
    Check(!ParseXmpMetadata(std::string("<x:xmpmeta></x:xmpmeta>"), m) &&
              !m.valid,
          "XMP with no gain-map fields rejected");
    // Negative: partial hdrgm (GainMapMin only, no Max/Gamma) -> false.
    Check(!ParseXmpMetadata(
              std::string("<rdf:Description><hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "</rdf:Description>"),
              m) &&
              !m.valid,
          "XMP partial hdrgm (Min only) rejected");
    // Negative: hdrgm gamma 0 -> false (even with Min/Max present).
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
                          "<hdrgm:Gamma>0.0</hdrgm:Gamma>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma 0 rejected");
    // P3-A3: blank self-closing Gamma (<hdrgm:Gamma />) must not
    // steal the next field's number. Without the whitespace skip in
    // FindXmpField, the '/' is missed and Gamma parses as 2.0 (the
    // following GainMapMax) — the packet is wrongly accepted.
    // Note: Gamma is required by ParseXmpMetadata (hasMin && hasMax
    // && hasGamma), so a blank Gamma rejects the whole packet; the
    // point is that it must NOT be accepted with a stolen value.
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:Gamma />"
                          "<hdrgm:GainMapMax>2.0</hdrgm:GainMapMax>"),
              m) &&
              !m.valid,
          "XMP blank self-closing Gamma rejected (must not steal GainMapMax)");
    // Negative gamma -> rejected.
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
                          "<hdrgm:Gamma>-1.0</hdrgm:Gamma>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma -1.0 rejected");
    // "inf" / "nan" gamma: no parseable number -> field absent ->
    // packet rejected (Gamma is required).
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
                          "<hdrgm:Gamma>inf</hdrgm:Gamma>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma inf rejected");
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
                          "<hdrgm:Gamma>nan</hdrgm:Gamma>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma nan rejected");
    // Overflow gamma (1e999 -> inf) -> rejected.
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"
                          "<hdrgm:Gamma>1e999</hdrgm:Gamma>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma 1e999 (overflow) rejected");
    // Denominator bound: Gamma = "1/" + 1022 spaces + "2", so the
    // denominator's digits start inside the 1024-char window but end
    // beyond it. Without the denominator bound in
    // ParseFirstNumberAfter, strtod reads past the limit and Gamma
    // parses as 0.5 — the packet is wrongly accepted.
    Check(!ParseXmpMetadata(
              std::string("<hdrgm:GainMapMin>0.0</hdrgm:GainMapMin>"
                          "<hdrgm:Gamma>1/") +
                  std::string(1022, ' ') +
                  std::string("2</hdrgm:Gamma>"
                              "<hdrgm:GainMapMax>0.1</hdrgm:GainMapMax>"),
              m) &&
              !m.valid,
          "XMP hdrgm gamma denominator past limit rejected");
}

void TestAppleHeadroomRatioRealXmp() {
    using namespace gainmap;
    Metadata m;
    // Real iPhone 15 Pro HEIC XMP (PhotoPrism sample): Headroom
    // 3.847906 is a LINEAR RATIO, so gainMapMax = log2(3.847906) =
    // 1.9440735561 stops — NOT 3.847906 stops (the old reading, which
    // overstated the max boost 3.74x: 14.40x instead of 3.85x).
    Check(ParseXmpMetadata(std::string(kXmpRealIphone15Pro), m) && m.valid,
          "real iPhone 15 Pro XMP parses");
    Check(Near(m.gainMapMax, 1.9440735561271154, 1e-9),
          "real iPhone 15 Pro gainMapMax=log2(3.847906)=1.9441");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "real iPhone 15 Pro gainMapMin 0.0");
    Check(Near(m.gamma, 1.0, 1e-12), "real iPhone 15 Pro gamma defaults 1.0");
    Check(m.appleHeadroomFallback, "real iPhone 15 Pro sets appleHeadroomFallback");
    // Real Apple JPEG (libultrahdr sample, iPhone 13 mini): Headroom
    // 4.532783 -> log2 = 2.1803970961 stops.
    Check(ParseXmpMetadata(std::string(kXmpRealAppleJpegNew), m) && m.valid,
          "real Apple JPEG XMP parses");
    Check(Near(m.gainMapMax, 2.1803970960755414, 1e-9),
          "real Apple JPEG gainMapMax=log2(4.532783)=2.1804");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "real Apple JPEG gainMapMin 0.0");
    Check(Near(m.gamma, 1.0, 1e-12), "real Apple JPEG gamma defaults 1.0");
    Check(m.appleHeadroomFallback, "real Apple JPEG sets appleHeadroomFallback");
    // Real iPhone 15 Pro HEIC, iOS 18.0 (user sample): Headroom
    // 2.769181 -> log2 = 1.4694593546 stops.
    Check(ParseXmpMetadata(std::string(kXmpRealIphone15ProIos18), m) && m.valid,
          "real iPhone 15 Pro iOS 18 XMP parses");
    Check(Near(m.gainMapMax, 1.469459354632348, 1e-9),
          "real iPhone 15 Pro iOS 18 gainMapMax=log2(2.769181)=1.4695");
    Check(Near(m.gainMapMin, 0.0, 1e-12), "real iPhone 15 Pro iOS 18 gainMapMin 0.0");
    Check(Near(m.gamma, 1.0, 1e-12), "real iPhone 15 Pro iOS 18 gamma defaults 1.0");
    Check(m.appleHeadroomFallback, "real iPhone 15 Pro iOS 18 sets appleHeadroomFallback");
}

void TestSrgbToLinear() {
    using namespace gainmap;
    // Spot values computed in Python from the sRGB EOTF inverse
    // (~/workspace/gainmap_reference.py srgb_to_linear).
    Check(Near(SrgbToLinear(0), 0.0, 1e-9), "SrgbToLinear 0 -> 0");
    Check(Near(SrgbToLinear(255), 1.0, 1e-9), "SrgbToLinear 255 -> 1");
    Check(Near(SrgbToLinear(128), 0.21586050011389926, 1e-6),
          "SrgbToLinear 128 -> 0.2158605");
    // The 0.04045 branch boundary: 10 is on the linear segment,
    // 11 is on the power segment.
    Check(Near(SrgbToLinear(10), 0.003035269835488375, 1e-6),
          "SrgbToLinear 10 -> 0.00303527 (linear segment)");
    Check(Near(SrgbToLinear(11), 0.003346535763899161, 1e-6),
          "SrgbToLinear 11 -> 0.00334654 (power segment)");
}

void TestBilinearUpsample() {
    using namespace gainmap;
    // 2x2 -> 4x4, centre-aligned mapping.
    uint8_t src[4] = {0, 100, 150, 255};  // (0,0)=0 (1,0)=100 (0,1)=150 (1,1)=255
    uint8_t dst[16] = {0};
    BilinearUpsampleGray(src, 2, 2, dst, 4, 4);
    Check(dst[0] == 0, "upsample corner (0,0) == src (0,0)");
    Check(dst[3] == 100, "upsample corner (3,0) == src (1,0)");
    Check(dst[12] == 150, "upsample corner (0,3) == src (0,1)");
    Check(dst[15] == 255, "upsample corner (3,3) == src (1,1)");
    // Centre (1,1): weights 0.75/0.25 on each axis ->
    // 0*0.5625 + 100*0.1875 + 150*0.1875 + 255*0.0625 = 62.8125 -> 63.
    Check(dst[5] == 63, "upsample centre (1,1) == 63 (bilinear average)");
    // Centre (2,2): weights 0.25/0.75 ->
    // 0*0.0625 + 100*0.1875 + 150*0.1875 + 255*0.5625 = 190.3125 -> 190.
    Check(dst[10] == 190, "upsample centre (2,2) == 190 (bilinear average)");
    // Top edge (1,0): row 0 only, 0*0.75 + 100*0.25 = 25.
    Check(dst[1] == 25, "upsample top edge (1,0) == 25");
    // Same size -> exact copy.
    uint8_t same[4] = {0};
    BilinearUpsampleGray(src, 2, 2, same, 2, 2);
    Check(same[0] == 0 && same[1] == 100 && same[2] == 150 && same[3] == 255,
          "upsample same size copies exactly");
    // 1x1 source -> every dst pixel is the single source value.
    uint8_t one[1] = {77};
    uint8_t big[16] = {0};
    BilinearUpsampleGray(one, 1, 1, big, 4, 4);
    bool all77 = true;
    for (int i = 0; i < 16; ++i) {
        if (big[i] != 77) {
            all77 = false;
        }
    }
    Check(all77, "upsample 1x1 source fills with the single value");
    // P3-A8 (2): downsampling (4x4 -> 2x2). Centre-aligned mapping:
    // dst (0,0) samples src (0.5,0.5), the mean of the top-left 2x2
    // block; dst (1,1) samples src (2.5,2.5), the mean of the
    // bottom-right block. Values are multiples of 4 so every mean
    // is an exact integer (no rounding dependence).
    uint8_t big4[16] = {0, 4, 8, 12, 16, 20, 24, 28,
                        32, 36, 40, 44, 48, 52, 56, 60};
    uint8_t small[4] = {0};
    BilinearUpsampleGray(big4, 4, 4, small, 2, 2);
    Check(small[0] == 10 && small[1] == 18 && small[2] == 42 && small[3] == 50,
          "downsample 4x4 -> 2x2 == block means (10,18,42,50)");
}

void TestApplyGainMap() {
    using namespace gainmap;
    // (a) Synthetic: base sRGB mid-grey (128,128,128), gain-map all
    // 255, gainMapMin=0, gainMapMax=1.0 (log2), gamma=1, offsets 0,
    // weightFactor=1: gain = 2^1 = 2, so HDR linear =
    // sRGBtoLinear(128/255) * 2 = 0.21586050011389926 * 2 = 0.431721...
    // (Python: ~/workspace/gainmap_reference.py).
    Metadata m;
    m.gainMapMin = 0.0;
    m.gainMapMax = 1.0;
    m.gamma = 1.0;
    m.baseOffset = 0.0;
    m.alternateOffset = 0.0;
    m.valid = true;
    uint8_t base[3] = {128, 128, 128};
    uint8_t gainFull[1] = {255};
    float out[3] = {0, 0, 0};
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, 1.0, out),
          "ApplyGainMap synthetic (a) succeeds");
    Check(Near(out[0], 0.4317210002277985, 1e-5) &&
              Near(out[1], 0.4317210002277985, 1e-5) &&
              Near(out[2], 0.4317210002277985, 1e-5),
          "ApplyGainMap (a) gain 255 -> linear * 2");
    // (b) Gain-map all 0 -> gain 1.0, output == sRGBtoLinear(base).
    uint8_t gainZero[1] = {0};
    Check(ApplyGainMap(base, 1, 1, gainZero, 1, 1, m, 1.0, out),
          "ApplyGainMap synthetic (b) succeeds");
    Check(Near(out[0], 0.21586050011389926, 1e-6) &&
              Near(out[1], 0.21586050011389926, 1e-6) &&
              Near(out[2], 0.21586050011389926, 1e-6),
          "ApplyGainMap (b) gain 0 -> linear unchanged");
    // weightFactor clamps: 2.0 behaves as 1.0, -1.0 behaves as 0.0.
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, 2.0, out) &&
              Near(out[0], 0.4317210002277985, 1e-5),
          "ApplyGainMap weightFactor 2.0 clamps to 1.0");
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, -1.0, out) &&
              Near(out[0], 0.21586050011389926, 1e-6),
          "ApplyGainMap weightFactor -1.0 clamps to 0.0 (gain 1.0)");
    // weightFactor 0.5: gain = 2^(1*0.5) = sqrt(2) ->
    // 0.2158605 * 1.41421356 = 0.305275...
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, 0.5, out) &&
              Near(out[0], 0.21586050011389926 * 1.4142135623730951, 1e-5),
          "ApplyGainMap weightFactor 0.5 -> gain sqrt(2)");
    // P3-A8 (2): downsampling through ApplyGainMap - a 4x4 gain map
    // on a 2x2 base. All-255 gain downsamples to 255, so every pixel
    // gets boost 2^1 = 2; all-0 gain downsamples to 0, boost 1.
    uint8_t gainBig[16];
    std::memset(gainBig, 255, sizeof(gainBig));
    uint8_t base2[12];
    for (int i = 0; i < 12; ++i) base2[i] = 128;
    float outDown[12] = {0};
    Check(ApplyGainMap(base2, 2, 2, gainBig, 4, 4, m, 1.0, outDown),
          "ApplyGainMap 4x4 gain on 2x2 base succeeds (downsample)");
    bool downOk = true;
    for (int i = 0; i < 12; ++i) {
        if (!Near(outDown[i], 0.4317210002277985, 1e-5)) downOk = false;
    }
    Check(downOk, "ApplyGainMap downsampled all-255 gain -> linear * 2");
    std::memset(gainBig, 0, sizeof(gainBig));
    Check(ApplyGainMap(base2, 2, 2, gainBig, 4, 4, m, 1.0, outDown) &&
              Near(outDown[0], 0.21586050011389926, 1e-6),
          "ApplyGainMap downsampled all-0 gain -> linear unchanged");
    // Bad inputs -> false.
    Check(!ApplyGainMap(nullptr, 1, 1, gainFull, 1, 1, m, 1.0, out),
          "ApplyGainMap null base rejected");
    Check(!ApplyGainMap(base, 1, 1, nullptr, 1, 1, m, 1.0, out),
          "ApplyGainMap null gain map rejected");
    Check(!ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, 1.0, nullptr),
          "ApplyGainMap null output rejected");
    Check(!ApplyGainMap(base, 0, 1, gainFull, 1, 1, m, 1.0, out),
          "ApplyGainMap zero width rejected");
    Check(!ApplyGainMap(base, 1, 1, gainFull, 0, 1, m, 1.0, out),
          "ApplyGainMap zero gain width rejected");
    double nanW = std::nan("");
    Check(!ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, nanW, out),
          "ApplyGainMap NaN weightFactor rejected");
    // P3-A8 (5): weightFactor +-Inf clamps like the finite
    // out-of-range values (+Inf -> 1.0, -Inf -> 0.0); only NaN
    // rejects (checked above).
    double infW = HUGE_VAL;
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, infW, out) &&
              Near(out[0], 0.4317210002277985, 1e-5),
          "ApplyGainMap weightFactor +Inf clamps to 1.0");
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, m, -infW, out) &&
              Near(out[0], 0.21586050011389926, 1e-6),
          "ApplyGainMap weightFactor -Inf clamps to 0.0 (gain 1.0)");
    Metadata invalid;
    Check(!ApplyGainMap(base, 1, 1, gainFull, 1, 1, invalid, 1.0, out),
          "ApplyGainMap invalid metadata rejected");
    Metadata badGamma = m;
    badGamma.gamma = 0.0;
    Check(!ApplyGainMap(base, 1, 1, gainFull, 1, 1, badGamma, 1.0, out),
          "ApplyGainMap gamma 0 rejected");
    // P3-A2+P3-B3: hostile gainMapMax (1e6 / 1e308) is clamped to +16
    // stops: gain 255 -> logBoost = 16 -> boost 2^16 = 65536 (finite),
    // not Inf. (Probe: without the clamp, gainMapMax=1e6/1e308 makes
    // boost Inf and out[0] Inf.)
    Metadata hugeMax = m;
    hugeMax.gainMapMax = 1e6;
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, hugeMax, 1.0, out) &&
              std::isfinite(out[0]) &&
              Near(out[0], 14146.633735464502, 0.01),
          "ApplyGainMap gainMapMax=1e6 clamps to +16 stops (finite)");
    Metadata hugeMax2 = m;
    hugeMax2.gainMapMax = 1e308;
    Check(ApplyGainMap(base, 1, 1, gainFull, 1, 1, hugeMax2, 1.0, out) &&
              std::isfinite(out[0]) &&
              Near(out[0], 14146.633735464502, 0.01),
          "ApplyGainMap gainMapMax=1e308 clamps to +16 stops (finite)");
    // Negative side: gainMapMin=-1e6 clamps to -16; gain 0 ->
    // logRecovery 0 -> logBoost = gainMapMin = -16 -> boost 2^-16.
    Metadata hugeMin = m;
    hugeMin.gainMapMin = -1e6;
    Check(ApplyGainMap(base, 1, 1, gainZero, 1, 1, hugeMin, 1.0, out) &&
              std::isfinite(out[0]) &&
              Near(out[0], 3.293769838163746e-06, 1e-9),
          "ApplyGainMap gainMapMin=-1e6 clamps to -16 stops (finite)");
}

void TestApplyGainMapRealMeta() {
    using namespace gainmap;
    // (d) End-to-end vs the Python reference on a known small input,
    // using the REAL HLG metadata (gainMapMin=0, gainMapMax=
    // 0.09999992698431015, gamma=1, offsets 1.0000000116883119e-07).
    // Expected floats were computed in Python with the exact formula
    // (~/workspace/gainmap_reference.py: srgb_to_linear + the
    // reconstruct_uhdr gain math) and rounded to float32; see the
    // cross-check in the task notes. Tolerance 1e-6 (float storage).
    Metadata m;
    m.gainMapMin = 0.0;
    m.gainMapMax = 0.09999992698431015;
    m.gamma = 1.0;
    m.baseOffset = 1.0000000116883119e-07;
    m.alternateOffset = 1.0000000116883119e-07;
    m.valid = true;
    // 2x2 base RGB, row-major; 2x2 gain map, row-major.
    uint8_t base[12] = {200, 100, 50, 30, 180, 90, 255, 0, 128, 64, 64, 64};
    uint8_t gain[4] = {0, 51, 105, 255};
    float out[12] = {0};
    Check(ApplyGainMap(base, 2, 2, gain, 2, 2, m, 1.0, out),
          "ApplyGainMap real-meta 2x2 succeeds");
    const double kExpected[12] = {
        0.577580451965332, 0.12743768095970154, 0.03189603239297867,
        0.013164269737899303, 0.4627822935581207, 0.10366897284984589,
        1.0289525985717773, 2.8952540365168034e-09, 0.22211021184921265,
        0.05494925007224083, 0.05494925007224083, 0.05494925007224083};
    bool ok = true;
    for (int i = 0; i < 12; ++i) {
        if (!Near(out[i], kExpected[i], 1e-6)) {
            ok = false;
        }
    }
    Check(ok, "ApplyGainMap real-meta 2x2 matches Python reference");
    // The same 2x2 base with a 1x1 gain map (value 255, upsampled to
    // 2x2) must equal the all-255 gain result: gain = 2^0.1 =
    // 1.0717734... for every pixel.
    uint8_t gain1[1] = {255};
    float outUp[12] = {0};
    Check(ApplyGainMap(base, 2, 2, gain1, 1, 1, m, 1.0, outUp),
          "ApplyGainMap 1x1 gain map upsample succeeds");
    bool okUp = true;
    for (int i = 0; i < 12; ++i) {
        // Expected: (sRGBtoLinear(base[i]) + 1e-07) * 2^0.1 - 1e-07,
        // computed here from the same formula in double; the 1e-6
        // band covers the float rounding.
        double sdr = SrgbToLinear(base[i]);
        double expected = (sdr + 1.0000000116883119e-07) * 1.0717734082931756 -
                          1.0000000116883119e-07;
        if (!Near(outUp[i], expected, 1e-6)) {
            okUp = false;
        }
    }
    Check(okUp, "ApplyGainMap 1x1 gain map upsample matches 2^0.1 gain");
}


void TestApplyGainMapLutExact() {
    using namespace gainmap;
    // N1-A: ApplyGainMap serves the per-pixel pow calls from 256-entry
    // tables (boost table + sRGB->linear table). The tables are EXACT,
    // not approximate — every input is a byte plus per-image constants —
    // so the output must be bit-identical to a direct-pow evaluation
    // of the ISO formula. This pins that: a from-the-formula reference
    // (direct std::pow, same expression order) over ALL 256 gain bytes
    // x ALL 256 base bytes per channel, for several parameter sets
    // (iPhone-derived headroom, real HLG metadata, non-1 gamma,
    // negative gainMapMin, non-zero offsets, partial weight). The
    // comparison is float-exact (==), not a tolerance band: any table
    // error, however small, fails.
    struct Params {
        double gainMapMin, gainMapMax, gamma, baseOffset, alternateOffset;
        double weightFactor;
    };
    const Params kSets[] = {
        {0.0, 3.0, 1.0, 0.0, 0.0, 1.0},        // derived iPhone headroom
        {0.0, 0.09999992698431015, 1.0, 1.0000000116883119e-07,
         1.0000000116883119e-07, 1.0},          // real HLG metadata
        {0.0, 1.329, 2.2, 0.0, 0.0, 1.0},       // gamma 2.2, mid headroom
        {-1.5, 2.5, 0.45, 0.01, -0.02, 0.5},    // negative min, partial weight
        {0.0, 0.0, 1.0, 0.0, 0.0, 1.0},         // zero gain range
        {0.0, 3.0, 1.0, 0.0, 0.0, 0.0},         // weight 0 -> SDR unchanged
        {0.0, 2.0, 1.8, 0.001, 0.002, 2.0},     // weight 2.0 clamps to 1.0
    };
    // 256x256 image: gain byte = x (all 256), base R = y, G = (x+y)&255,
    // B = (7x+13y)&255 — every (gain, base) pair per channel is covered.
    const int kW = 256, kH = 256;
    std::vector<uint8_t> base((size_t)kW * kH * 3);
    std::vector<uint8_t> gain((size_t)kW * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            size_t i = (size_t)y * kW + x;
            gain[i] = (uint8_t)x;
            base[i * 3 + 0] = (uint8_t)y;
            base[i * 3 + 1] = (uint8_t)((x + y) & 0xff);
            base[i * 3 + 2] = (uint8_t)((7 * x + 13 * y) & 0xff);
        }
    }
    std::vector<float> out((size_t)kW * kH * 3);
    for (size_t s = 0; s < sizeof(kSets) / sizeof(kSets[0]); ++s) {
        const Params& p = kSets[s];
        Metadata m;
        m.gainMapMin = p.gainMapMin;
        m.gainMapMax = p.gainMapMax;
        m.gamma = p.gamma;
        m.baseOffset = p.baseOffset;
        m.alternateOffset = p.alternateOffset;
        m.valid = true;
        bool ran = ApplyGainMap(base.data(), kW, kH, gain.data(), kW, kH, m,
                              p.weightFactor, out.data());
        Check(ran, "ApplyGainMap LUT param set succeeds");
        // Reference: direct-pow ISO formula, same expression order as
        // GainMap.h documents (gainMapMin/Max clamped to +/-16 and
        // weightFactor to [0,1], mirroring ApplyGainMap's contract).
        // Skipped when the call above failed (no output to compare);
        // `exact` stays false so the check below still records it.
        double gmin = std::min(16.0, std::max(-16.0, p.gainMapMin));
        double gmax = std::min(16.0, std::max(-16.0, p.gainMapMax));
        double w = p.weightFactor;
        if (w < 0.0) w = 0.0;
        if (w > 1.0) w = 1.0;
        double invGamma = 1.0 / p.gamma;
        bool exact = false;
        if (ran) {
            exact = true;
            for (size_t i = 0; i < (size_t)kW * kH; ++i) {
                double g = static_cast<double>(gain[i]) / 255.0;
                double logRecovery = std::pow(g, invGamma);
                double logBoost = gmin * (1.0 - logRecovery) + gmax * logRecovery;
                double boost = std::pow(2.0, logBoost * w);
                for (int c = 0; c < 3; ++c) {
                    // sRGB EOTF inverse in double (same formula as
                    // SrgbToLinearDouble in GainMap.cpp).
                    double c01 = static_cast<double>(base[i * 3 + (size_t)c]) / 255.0;
                    double sdrD = (c01 <= 0.04045)
                                      ? c01 / 12.92
                                      : std::pow((c01 + 0.055) / 1.055, 2.4);
                    float expected = (float)((sdrD + p.baseOffset) * boost -
                                             p.alternateOffset);
                    if (out[i * 3 + (size_t)c] != expected) {
                        exact = false;
                    }
                }
                if (!exact) break;
            }
        }
        char what[128];
        std::snprintf(what, sizeof(what),
                      "ApplyGainMap LUT bit-identical to direct pow, param set %u",
                      (unsigned)s);
        Check(exact, what);
    }
}

void TestFindGainMapJpeg() {
    using namespace gainmap;
    size_t off = 0, sz = 0;
    // Real HLG MPF: gain map at 8087, size 8277.
    std::vector<uint8_t> hlg = BuildUhdrJpeg(kMpfSegmentHlg, 8277);
    Check(FindGainMapJpeg(hlg.data(), hlg.size(), off, sz) && off == 8087 &&
              sz == 8277,
          "FindGainMapJpeg HLG real MPF -> offset 8087 size 8277");
    // Real PQ MPF: gain map at 8087, size 37284.
    std::vector<uint8_t> pq = BuildUhdrJpeg(kMpfSegmentPq, 37284);
    Check(FindGainMapJpeg(pq.data(), pq.size(), off, sz) && off == 8087 &&
              sz == 37284,
          "FindGainMapJpeg PQ real MPF -> offset 8087 size 37284");
    // Plain SDR JPEG (real test_reference.jpg header, no MPF) -> false.
    Check(!FindGainMapJpeg(kSdrJpegHeader, sizeof(kSdrJpegHeader), off, sz),
          "FindGainMapJpeg plain SDR JPEG (no MPF) -> false");
    // Truncated inside the MPF segment (700 of 734 bytes) -> false.
    Check(!FindGainMapJpeg(hlg.data(), 700, off, sz),
          "FindGainMapJpeg truncated MPF -> false");
    // Truncated so offset+size runs past the end -> false.
    Check(!FindGainMapJpeg(hlg.data(), 8087 + 100, off, sz),
          "FindGainMapJpeg gain map past end of buffer -> false");
    // NumberOfImages = 1 (B001 value field, last byte at buffer
    // offset 685) -> false.
    std::vector<uint8_t> oneImg = hlg;
    oneImg[685] = 0x01;
    Check(!FindGainMapJpeg(oneImg.data(), oneImg.size(), off, sz),
          "FindGainMapJpeg NumberOfImages=1 -> false");
    // P3-A8 (6): NumberOfImages = 3. No real 3-image MPF file was on
    // hand (all real UltraHDR MPFs carry 2 images) - synthetic
    // mutation of the real HLG segment. The finder only reads entry
    // 1 and never cross-checks count == numImages*16, so a third
    // image is accepted with the same gain-map offset/size. This
    // test pins the current "entry 1 is the gain map" assumption:
    // a real 3-image file could violate it (entry 1 might not be
    // the gain map).
    std::vector<uint8_t> threeImg = hlg;
    threeImg[685] = 0x03;
    Check(FindGainMapJpeg(threeImg.data(), threeImg.size(), off, sz) &&
              off == 8087 && sz == 8277,
          "FindGainMapJpeg NumberOfImages=3 -> offset 8087 size 8277 (entry 1)");
    // P3-A8 (1): little-endian MPF (synthetic mutation of the real
    // HLG segment, see MakeLittleEndianMpf). The finder must read
    // it identically: offset 8087, size 8277.
    std::vector<uint8_t> leMpf = MakeLittleEndianMpf();
    std::vector<uint8_t> leJpeg = BuildUhdrJpeg(leMpf.data(), 8277);
    Check(FindGainMapJpeg(leJpeg.data(), leJpeg.size(), off, sz) && off == 8087 &&
              sz == 8277,
          "FindGainMapJpeg little-endian MPF -> offset 8087 size 8277");
    // Gain-map bytes do not start with FFD8 -> false.
    std::vector<uint8_t> badSoi = hlg;
    badSoi[8087] = 0x00;
    Check(!FindGainMapJpeg(badSoi.data(), badSoi.size(), off, sz),
          "FindGainMapJpeg gain map without SOI -> false");
    // Bad TIFF byte order ("MM" -> "ZM" at buffer offset 652) -> false.
    std::vector<uint8_t> badMagic = hlg;
    badMagic[652] = 0x5A;
    Check(!FindGainMapJpeg(badMagic.data(), badMagic.size(), off, sz),
          "FindGainMapJpeg bad TIFF byte order -> false");
    // Null / empty / non-JPEG -> false.
    Check(!FindGainMapJpeg(nullptr, hlg.size(), off, sz),
          "FindGainMapJpeg null data rejected");
    Check(!FindGainMapJpeg(hlg.data(), 0, off, sz),
          "FindGainMapJpeg empty buffer rejected");
    uint8_t notJpeg[16] = {0};
    Check(!FindGainMapJpeg(notJpeg, sizeof(notJpeg), off, sz),
          "FindGainMapJpeg non-JPEG rejected");
}

void TestFindIso21496Payload() {
    using namespace gainmap;
    // Synthetic gain-map JPEG: SOI + APP2 (len 91) carrying the REAL
    // 89-byte ISO payload + EOI.
    uint8_t gm[2 + 2 + 2 + 89 + 2];
    gm[0] = 0xFF;
    gm[1] = 0xD8;  // SOI
    gm[2] = 0xFF;
    gm[3] = 0xE2;  // APP2
    gm[4] = 0x00;
    gm[5] = 0x5B;  // length 91
    std::memcpy(gm + 6, kIsoPayloadHlg, 89);
    gm[95] = 0xFF;
    gm[96] = 0xD9;  // EOI
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
    Check(FindIso21496Payload(gm, sizeof(gm), payload, payloadSize),
          "FindIso21496Payload real ISO APP2 found");
    Check(payload == gm + 6 && payloadSize == 89,
          "FindIso21496Payload returns pointer+size into the buffer, no copy");
    Metadata m;
    Check(ParseIso21496Metadata(payload, payloadSize, m) && m.valid,
          "FindIso21496Payload payload parses as ISO metadata");
    Check(Near(m.alternateHdrHeadroom, 2.300448, 1e-6),
          "FindIso21496Payload payload altHeadroom 2.300448");
    Check(Near(m.gainMapMax, 0.1, 1e-6),
          "FindIso21496Payload payload gainMapMax 0.1");
    // Plain SDR JPEG (real test_reference.jpg header) has no ISO APP2.
    Check(!FindIso21496Payload(kSdrJpegHeader, sizeof(kSdrJpegHeader),
                               payload, payloadSize),
          "FindIso21496Payload plain SDR JPEG -> false");
    // A JPEG with only a non-ISO APP2 (ICC profile) -> false.
    uint8_t icc[2 + 2 + 2 + 14 + 2];
    icc[0] = 0xFF;
    icc[1] = 0xD8;
    icc[2] = 0xFF;
    icc[3] = 0xE2;
    icc[4] = 0x00;
    icc[5] = 0x10;  // length 16
    std::memcpy(icc + 6, "ICC_PROFILE\0\x01\x01", 14);
    icc[20] = 0xFF;
    icc[21] = 0xD9;
    Check(!FindIso21496Payload(icc, sizeof(icc), payload, payloadSize),
          "FindIso21496Payload ICC-only APP2 -> false");
    // ISO APP2 after the ICC APP2 is still found (scan continues).
    uint8_t both[sizeof(icc) - 2 + sizeof(gm) - 2];
    std::memcpy(both, icc, sizeof(icc) - 2);
    std::memcpy(both + sizeof(icc) - 2, gm + 2, sizeof(gm) - 2);
    Check(FindIso21496Payload(both, sizeof(both), payload, payloadSize) &&
              payloadSize == 89,
          "FindIso21496Payload ISO APP2 after ICC APP2 found");
    // Truncated ISO APP2 (length runs past the buffer) -> false.
    Check(!FindIso21496Payload(gm, 20, payload, payloadSize),
          "FindIso21496Payload truncated ISO APP2 -> false");
    // Version-only ISO segment (URN + 4 version bytes, 32-byte
    // payload, as in some primary JPEGs): the payload is shorter than
    // the minimum legal ISO 21496-1 payload (89 bytes: 28 URN+NUL +
    // 5 version and flags + 7x8 rational pairs), so the finder skips
    // it (P3-A5) and the parser never sees it.
    uint8_t verOnly[2 + 2 + 2 + 32 + 2];
    verOnly[0] = 0xFF;
    verOnly[1] = 0xD8;
    verOnly[2] = 0xFF;
    verOnly[3] = 0xE2;
    verOnly[4] = 0x00;
    verOnly[5] = 0x22;  // length 34
    std::memcpy(verOnly + 6, kIsoPayloadHlg, 28);
    verOnly[34] = 0x00;
    verOnly[35] = 0x00;  // min_ver 0
    verOnly[36] = 0x00;
    verOnly[37] = 0x00;  // writer_ver 0
    verOnly[38] = 0xFF;
    verOnly[39] = 0xD9;
    Check(!FindIso21496Payload(verOnly, sizeof(verOnly), payload,
                               payloadSize),
          "FindIso21496Payload version-only ISO segment skipped (< 89 bytes)");
    // P3-A8 (3): a 145-byte ISO APP2 (89 + 56 trailing junk bytes)
    // is found with its full size and parses (the finder floor is
    // 89, so a longer payload is not skipped).
    uint8_t gmLong[2 + 2 + 2 + 145 + 2];
    gmLong[0] = 0xFF;
    gmLong[1] = 0xD8;  // SOI
    gmLong[2] = 0xFF;
    gmLong[3] = 0xE2;  // APP2
    gmLong[4] = 0x00;
    gmLong[5] = 0x93;  // length 147
    std::memcpy(gmLong + 6, kIsoPayloadHlg, 89);
    std::memset(gmLong + 6 + 89, 0xAB, 56);  // trailing junk
    gmLong[151] = 0xFF;
    gmLong[152] = 0xD9;  // EOI
    Check(FindIso21496Payload(gmLong, sizeof(gmLong), payload, payloadSize) &&
              payloadSize == 145,
          "FindIso21496Payload 145-byte ISO APP2 found with full size");
    Check(ParseIso21496Metadata(payload, payloadSize, m) && m.valid &&
              Near(m.gainMapMax, 0.1, 1e-6),
          "FindIso21496Payload 145-byte payload parses as first set");
    // The parser still rejects the 32-byte payload directly
    // (size < 33+56), if it ever saw one.
    Check(!ParseIso21496Metadata(verOnly + 6, 32, m) && !m.valid,
          "ParseIso21496Metadata version-only payload rejected (size < 33+56)");
    // Null / empty -> false.
    Check(!FindIso21496Payload(nullptr, sizeof(gm), payload, payloadSize),
          "FindIso21496Payload null data rejected");
    Check(!FindIso21496Payload(gm, 0, payload, payloadSize),
          "FindIso21496Payload empty buffer rejected");
}

}  // namespace

int main() {
    TestParseIsoReal();
    TestParseIsoNegative();
    TestFindGainMapJpeg();
    TestFindIso21496Payload();
    TestParseXmp();
    TestAppleHeadroomRatioRealXmp();
    TestSrgbToLinear();
    TestBilinearUpsample();
    TestApplyGainMap();
    TestApplyGainMapRealMeta();
    TestApplyGainMapLutExact();
    std::printf("gainmap_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
