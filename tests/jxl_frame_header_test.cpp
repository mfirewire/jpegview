// jxl_frame_header_test.cpp - libjxl API-semantics harness for S1.
//
// What this pins, and what it does NOT:
// --------------------------------------
// It pins the libjxl 0.13 API semantics the S1 guard in JXLWrapper.cpp
// relies on: once the decoder reports JXL_DEC_FRAME, JxlDecoderGetFrameHeader
// returns JXL_DEC_SUCCESS; before a frame header is available (truncated
// input, or no ProcessInput call yet) and on garbage input it does NOT
// return JXL_DEC_SUCCESS (this box's runtime reports JXL_DEC_ERROR in those
// cases; the 0.13 header also allows JXL_DEC_NEED_MORE_INPUT - the guard
// treats any non-SUCCESS as failure, so either value is fine).
// It does NOT test the wrapper line itself: JXLWrapper.cpp is MFC-coupled
// and does not compile on Linux. The wrapper check (return false unless
// SUCCESS, plus the = {} belt-and-braces) is verified by inspection and by
// the Windows CI build. Device test: open an animated JXL, step frames,
// frame_time reasonable and unchanged from before the fix. No real file
// can force the wrapper's failure branch.
//
// Fixture: the kennylevinsen sRGB test chart (single frame), embedded as
// bytes - the same file as hdr-test-images/img/test_srgb.jxl (397 bytes).
// A still image still emits JXL_DEC_FRAME, which is all this harness needs.
//
// Build and run (Linux):
//   g++ -std=c++17 -Wall -Wextra -O2 -DJXL_STATIC_DEFINE
//       -I src/JPEGView/libjxl/include tests/jxl_frame_header_test.cpp
//       -l:libjxl.so.0.7 -o /tmp/jxl_frame_header_test
//   /tmp/jxl_frame_header_test
//
// Notes on that build line:
// - Headers are the vendored libjxl 0.13 headers (src/JPEGView/libjxl/).
//   The link at run time is the system libjxl (0.7 on this box; no dev
//   symlink, hence -l:libjxl.so.0.7). The exercised symbols and enum values
//   (JxlDecoderCreate/SubscribeEvents/SetInput/ProcessInput/GetFrameHeader/
//   Destroy; JXL_DEC_SUCCESS=0, ERROR=1, NEED_MORE_INPUT=2, FRAME=0x400)
//   are ABI-stable across those versions.
// - -DJXL_STATIC_DEFINE is needed because the vendored jxl_export.h is the
//   CMake-generated Windows variant (unconditional __declspec(dllimport),
//   which does not compile on Linux); the define makes JXL_EXPORT empty.
//   It affects only this harness's compilation, not the vendored headers.
// - The frame header is read through a padded buffer and only `duration`
//   (offset 0) and `is_last` are inspected, so no JxlFrameHeader
//   struct-layout assumption between the 0.13 headers and the 0.7 runtime
//   is load-bearing.

#include <cstdio>
#include <cstdint>
#include <cstring>

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

// kennylevinsen sRGB test chart (single frame), 397 bytes.
const uint8_t kTestSrbgJxl[] = {
    0x00, 0x00, 0x00, 0x0c, 0x4a, 0x58, 0x4c, 0x20, 0x0d, 0x0a, 0x87, 0x0a,
    0x00, 0x00, 0x00, 0x14, 0x66, 0x74, 0x79, 0x70, 0x6a, 0x78, 0x6c, 0x20,
    0x00, 0x00, 0x00, 0x00, 0x6a, 0x78, 0x6c, 0x20, 0x00, 0x00, 0x00, 0x09,
    0x6a, 0x78, 0x6c, 0x6c, 0x0a, 0x00, 0x00, 0x01, 0x64, 0x6a, 0x78, 0x6c,
    0x63, 0xff, 0x0a, 0xf8, 0x79, 0xfc, 0x40, 0x02, 0x08, 0x02, 0x01, 0x00,
    0x58, 0x02, 0x00, 0x00, 0x80, 0x0b, 0xc4, 0x80, 0x07, 0x50, 0xc0, 0x03,
    0x34, 0x00, 0x4b, 0x28, 0x32, 0xdd, 0xe4, 0x8b, 0x4e, 0xaf, 0xd7, 0x52,
    0x50, 0x50, 0x88, 0x50, 0x50, 0x50, 0x72, 0x00, 0x00, 0xb8, 0x23, 0xaa,
    0x6d, 0x63, 0x0c, 0x0e, 0xa3, 0xf4, 0x1c, 0x83, 0xa5, 0x11, 0x1a, 0x28,
    0xae, 0x1b, 0x37, 0x28, 0xc6, 0xe0, 0x58, 0x4c, 0x1e, 0x2f, 0xe7, 0x02,
    0xe1, 0x01, 0xd8, 0x63, 0xb8, 0x0a, 0xf8, 0xff, 0x37, 0xbd, 0x3d, 0xa6,
    0xfc, 0x07, 0xc4, 0x42, 0x43, 0xef, 0xc8, 0x93, 0xcb, 0xe9, 0x1a, 0x4b,
    0x27, 0x75, 0x7a, 0xff, 0xbb, 0xd5, 0x61, 0x72, 0x08, 0xda, 0xf0, 0x6c,
    0x08, 0x85, 0xc2, 0x43, 0x83, 0x20, 0x57, 0x88, 0xbe, 0xd4, 0x1e, 0xf3,
    0xd7, 0xc0, 0xc5, 0xee, 0xb5, 0x65, 0x2d, 0xcd, 0xd2, 0x14, 0xd0, 0x3b,
    0xdf, 0x7e, 0xbe, 0x22, 0x6a, 0xf9, 0xda, 0xfb, 0x3c, 0x40, 0x2d, 0xbf,
    0xf4, 0xa7, 0x65, 0xa6, 0x02, 0xf0, 0xbe, 0x96, 0xca, 0x54, 0x50, 0xe4,
    0x9f, 0x96, 0x71, 0x8b, 0x8b, 0xbf, 0xa7, 0x65, 0x3c, 0x14, 0xb5, 0x1c,
    0x68, 0x2c, 0xc5, 0x05, 0xea, 0x28, 0x17, 0x00, 0x93, 0x34, 0x80, 0x0a,
    0xa0, 0x19, 0xf5, 0x1f, 0xfe, 0x0b, 0x29, 0x3a, 0x97, 0x7a, 0x22, 0x74,
    0x5c, 0x2e, 0xd9, 0x0c, 0xac, 0x10, 0x1c, 0x71, 0xad, 0xa9, 0xe4, 0x2d,
    0x30, 0xe7, 0x58, 0xb7, 0x03, 0x0f, 0xe4, 0x07, 0xad, 0xad, 0x29, 0x2a,
    0x4b, 0x14, 0x37, 0x89, 0x31, 0x9a, 0x93, 0x2c, 0x4a, 0x69, 0xa5, 0xef,
    0x0a, 0xa1, 0xcc, 0xca, 0x33, 0x44, 0x49, 0xfc, 0x59, 0xee, 0xc9, 0xab,
    0x46, 0x19, 0x0d, 0xb4, 0x88, 0x05, 0x6b, 0x55, 0xc3, 0xfa, 0xc9, 0x82,
    0xc0, 0xb0, 0x09, 0xe8, 0xb6, 0x8b, 0x93, 0xfa, 0x41, 0x5b, 0x13, 0x48,
    0xc6, 0x27, 0xb8, 0xeb, 0xd2, 0xbb, 0x1a, 0x13, 0x25, 0x3c, 0x00, 0x80,
    0x57, 0x02, 0x5f, 0xfa, 0x82, 0xa3, 0xc3, 0x95, 0x02, 0xbb, 0xd2, 0x8a,
    0x24, 0x15, 0xcb, 0xb1, 0x35, 0x42, 0xfb, 0xb3, 0xb3, 0x8f, 0xe5, 0x89,
    0x01, 0x17, 0x00, 0x02, 0x40, 0x59, 0x0a, 0x59, 0x84, 0x88, 0x30, 0x82,
    0xfc, 0x7a, 0xa3, 0x1d, 0xa1, 0x1a, 0xa5, 0xc1, 0x1f, 0x43, 0x31, 0xad,
    0x93, 0x45, 0xaf, 0x8b, 0x9f, 0x15, 0x47, 0x55, 0xa3, 0x34, 0xf8, 0x03,
    0x17, 0x00, 0x01, 0x80, 0x36, 0x4c, 0x86, 0x01, 0x6b, 0xa5, 0xa8, 0xc1,
    0x1f,
};

JxlDecoder* MakeDecoder(const uint8_t* data, size_t size) {
    JxlDecoder* dec = JxlDecoderCreate(NULL);
    if (dec == NULL) return NULL;
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return NULL;
    }
    if (JxlDecoderSetInput(dec, data, size) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return NULL;
    }
    return dec;
}

// The frame header is read through a padded buffer: the 0.13 header
// struct may differ in size from the 0.7 runtime's, and only `duration`
// (offset 0) and `is_last` are read.
JxlDecoderStatus GetFrameHeaderPadded(JxlDecoder* dec, JxlFrameHeader* out) {
    unsigned char buf[4096];
    std::memset(buf, 0, sizeof(buf));
    JxlDecoderStatus st = JxlDecoderGetFrameHeader(dec, reinterpret_cast<JxlFrameHeader*>(buf));
    if (st == JXL_DEC_SUCCESS) *out = *reinterpret_cast<JxlFrameHeader*>(buf);
    return st;
}

void TestFrameEventReturnsSuccess() {
    JxlDecoder* dec = MakeDecoder(kTestSrbgJxl, sizeof(kTestSrbgJxl));
    Check(dec != NULL, "decoder created");
    if (dec == NULL) return;
    JxlDecoderStatus st = JXL_DEC_NEED_MORE_INPUT;
    for (int i = 0; i < 16 && st != JXL_DEC_FRAME && st != JXL_DEC_SUCCESS &&
         st != JXL_DEC_ERROR; i++) {
        st = JxlDecoderProcessInput(dec);
    }
    Check(st == JXL_DEC_FRAME, "full input reaches JXL_DEC_FRAME");
    JxlFrameHeader header = {};
    JxlDecoderStatus g = GetFrameHeaderPadded(dec, &header);
    Check(g == JXL_DEC_SUCCESS, "GetFrameHeader at JXL_DEC_FRAME returns SUCCESS");
    // A still image has no animation duration and is the last frame.
    Check(header.duration == 0, "still-image frame duration is 0");
    Check(header.is_last == JXL_TRUE, "still-image frame is_last");
    JxlDecoderDestroy(dec);
}

void TestNoFrameHeaderYet() {
    // Truncated input: the decoder wants more input, and there is no
    // frame header to read yet.
    JxlDecoder* dec = MakeDecoder(kTestSrbgJxl, 12);
    Check(dec != NULL, "decoder created (truncated)");
    if (dec == NULL) return;
    JxlDecoderStatus st = JxlDecoderProcessInput(dec);
    Check(st == JXL_DEC_NEED_MORE_INPUT, "truncated input -> NEED_MORE_INPUT");
    JxlFrameHeader header = {};
    JxlDecoderStatus g = GetFrameHeaderPadded(dec, &header);
    Check(g != JXL_DEC_SUCCESS, "GetFrameHeader on truncated input is not SUCCESS");
    JxlDecoderDestroy(dec);

    // No ProcessInput call at all: likewise no frame header available.
    dec = MakeDecoder(kTestSrbgJxl, sizeof(kTestSrbgJxl));
    Check(dec != NULL, "decoder created (no ProcessInput)");
    if (dec == NULL) return;
    g = GetFrameHeaderPadded(dec, &header);
    Check(g != JXL_DEC_SUCCESS, "GetFrameHeader before ProcessInput is not SUCCESS");
    JxlDecoderDestroy(dec);
}

void TestGarbageInput() {
    uint8_t garbage[32];
    std::memset(garbage, 0xAA, sizeof(garbage));
    JxlDecoder* dec = MakeDecoder(garbage, sizeof(garbage));
    Check(dec != NULL, "decoder created (garbage)");
    if (dec == NULL) return;
    JxlDecoderStatus st = JxlDecoderProcessInput(dec);
    Check(st == JXL_DEC_ERROR, "garbage input -> ERROR");
    JxlFrameHeader header = {};
    JxlDecoderStatus g = GetFrameHeaderPadded(dec, &header);
    Check(g != JXL_DEC_SUCCESS, "GetFrameHeader on garbage input is not SUCCESS");
    JxlDecoderDestroy(dec);
}

}  // namespace

int main() {
    TestFrameEventReturnsSuccess();
    TestNoFrameHeaderYet();
    TestGarbageInput();
    std::printf("jxl_frame_header_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
