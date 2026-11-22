// tinyexr_multipart_test.cpp - pins the CVE-2026-110597 fix in the
// vendored tinyexr (ParseEXRMultipartHeaderFromMemory heap OOB read).
//
// The bug: the multi-part header loop advanced its cursor `marker` by
// each part's header length but kept passing a `marker_size` that was
// computed once before the loop and never reduced (only an unused
// local `size` was decremented). From the second part on, ReadAttribute
// was handed an inflated remaining length, its `size < data_len`
// guard passed, and a memcpy read past the end of the input buffer
// (CWE-125). The fix keeps `marker_size` in sync with the cursor and
// rejects a part whose header claims more bytes than remain.
//
// Version note - 1.0.13 is NOT the fix for this CVE, and the vendored
// copy was already newer than 1.0.13: the vendored tinyexr.h is
// byte-identical to upstream master at commit 6f470c9 (2026-07-09),
// which is the exact commit the CVE was reported against, and its
// ParseEXRMultipartHeaderFromMemory is byte-identical to 1.0.13's.
// 1.0.13 (2026-03-31) predates the disclosure (Oct 2026) and only
// fixes the FreeEXRImage double-free and the B44/B44A overflow, both
// of which the vendored copy already contains. Replacing the vendored
// files with 1.0.13 would have been a regression (it would also drop
// the post-1.0.13 PIZ hardening and the deep-tiled decoder, and
// downgrade the bundled miniz 3.1.2 to the 3.0.0 in 1.0.13's deps).
// So the fix is the reporter's own recommended patch, applied to the
// vendored file (see the CVE-2026-110597 comment at the patched site),
// not a version bump. No upstream release fixes this CVE as of
// 2026-11-21.
//
// Reachability in JPEGView: EXRWrapper rejects multi-part files on
// the version flag before any tinyexr multi-part parsing, and no
// JPEGView code calls the multi-part entry points, so the app's own
// EXR path could not reach the bug - but the vulnerable function is
// compiled into the exe (TINYEXR_IMPLEMENTATION), so the fix still
// belongs in the vendored copy rather than relying on the gate.
//
// Teeth: the PoC below over-reads 256 bytes past a 558-byte buffer
// under ASan on unpatched code (verified: heap-buffer-overflow in
// ReadAttribute, tinyexr.h:1263, via ParseEXRHeader :5946 and
// ParseEXRMultipartHeaderFromMemory :10580). A plain build does not
// crash - the over-read garbage happens to fail later with
// "# of channels is zero." - so the test also asserts the patched
// failure mode: ret == TINYEXR_ERROR_INVALID_DATA with
// "Failed to read attribute." in the error text. That assertion
// fails on the unpatched header (wrong error text) and the ASan CI
// step fails on it outright.
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView -I src/JPEGView/tinyexr
//       tests/tinyexr_multipart_test.cpp src/JPEGView/tinyexr/miniz.c
//       src/JPEGView/tinyexr/miniz_tdef.c src/JPEGView/tinyexr/miniz_tinfl.c
//       -o /tmp/tinyexr_multipart_test
//   /tmp/tinyexr_multipart_test
// ASan guard (same build with -fsanitize=address, detect_leaks=0):
//   g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address
//       -I src/JPEGView -I src/JPEGView/tinyexr
//       tests/tinyexr_multipart_test.cpp src/JPEGView/tinyexr/miniz.c
//       src/JPEGView/tinyexr/miniz_tdef.c src/JPEGView/tinyexr/miniz_tinfl.c
//       -o /tmp/tinyexr_multipart_test_asan
//   ASAN_OPTIONS=detect_leaks=0 /tmp/tinyexr_multipart_test_asan

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define TINYEXR_IMPLEMENTATION
#include "tinyexr/tinyexr.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL: %s\n", what);
    }
}

// The CVE-2026-110597 PoC (558 bytes, from the public write-up at
// hackmd.io/@haind-pwn/tinyexr-multipart-header-stale-size-oob-read):
// a two-part EXR whose part 0 is a valid header and whose part 1
// starts near the end of the buffer and declares an attribute with
// data_len 0x100. On unpatched code the stale marker_size lets
// ReadAttribute memcpy 256 bytes past the 558-byte input.
const unsigned char kCve2026110597Poc[] = {
    0x76, 0x2f, 0x31, 0x01, 0x02, 0x10, 0x00, 0x00, 0x63, 0x68, 0x61, 0x6e,
    0x6e, 0x65, 0x6c, 0x73, 0x00, 0x63, 0x68, 0x6c, 0x69, 0x73, 0x74, 0x00,
    0x13, 0x00, 0x00, 0x00, 0x52, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x63,
    0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x00, 0x63,
    0x6f, 0x6d, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x64, 0x61, 0x74, 0x61, 0x57, 0x69, 0x6e, 0x64,
    0x6f, 0x77, 0x00, 0x62, 0x6f, 0x78, 0x32, 0x69, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x69, 0x73, 0x70, 0x6c, 0x61, 0x79,
    0x57, 0x69, 0x6e, 0x64, 0x6f, 0x77, 0x00, 0x62, 0x6f, 0x78, 0x32, 0x69,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x6c, 0x69, 0x6e,
    0x65, 0x4f, 0x72, 0x64, 0x65, 0x72, 0x00, 0x6c, 0x69, 0x6e, 0x65, 0x4f,
    0x72, 0x64, 0x65, 0x72, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x70, 0x69,
    0x78, 0x65, 0x6c, 0x41, 0x73, 0x70, 0x65, 0x63, 0x74, 0x52, 0x61, 0x74,
    0x69, 0x6f, 0x00, 0x66, 0x6c, 0x6f, 0x61, 0x74, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x3f, 0x73, 0x63, 0x72, 0x65, 0x65, 0x6e, 0x57,
    0x69, 0x6e, 0x64, 0x6f, 0x77, 0x43, 0x65, 0x6e, 0x74, 0x65, 0x72, 0x00,
    0x76, 0x34, 0x66, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x73, 0x63, 0x72, 0x65, 0x65, 0x6e, 0x57, 0x69,
    0x6e, 0x64, 0x6f, 0x77, 0x57, 0x69, 0x64, 0x74, 0x68, 0x00, 0x66, 0x6c,
    0x6f, 0x61, 0x74, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
    0x6e, 0x61, 0x6d, 0x65, 0x00, 0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x70, 0x61, 0x72, 0x74, 0x30, 0x74, 0x79, 0x70,
    0x65, 0x00, 0x73, 0x74, 0x72, 0x69, 0x6e, 0x67, 0x00, 0x0d, 0x00, 0x00,
    0x00, 0x73, 0x63, 0x61, 0x6e, 0x6c, 0x69, 0x6e, 0x65, 0x69, 0x6d, 0x61,
    0x67, 0x65, 0x63, 0x68, 0x75, 0x6e, 0x6b, 0x43, 0x6f, 0x75, 0x6e, 0x74,
    0x00, 0x69, 0x6e, 0x74, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x73, 0x00, 0x63,
    0x68, 0x6c, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x64, 0x69, 0x73, 0x76, 0x2f, 0x31, 0x01, 0x02, 0x10, 0x00, 0x00, 0x63,
    0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c, 0x73, 0x00, 0x63, 0x68, 0x6c, 0x69,
    0x73, 0x74, 0x00, 0x13, 0x00, 0x00, 0x00, 0x52, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x63, 0x70, 0x6c, 0x61, 0x79, 0x57, 0x69, 0x6e, 0x64, 0x6f,
    0x77, 0x00, 0x62, 0x6f, 0x78, 0x32, 0x69, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x6c, 0x69, 0x6e, 0x65, 0x4f, 0x72, 0x64, 0x65,
    0x72, 0x00, 0x6c, 0x69, 0x6e, 0x65, 0x4f, 0x72, 0x64, 0x65, 0x72, 0x72,
    0x00, 0x76, 0x32, 0x66, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0x63, 0x72, 0x65, 0x04, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6e, 0x57, 0x69, 0x6e, 0x64, 0x6f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void FreeParsedHeaders(EXRHeader **headers, int num) {
    if (!headers) return;
    for (int i = 0; i < num; i++) {
        FreeEXRHeader(headers[i]);
        free(headers[i]);
    }
    free(headers);
}

void TestCvePoC() {
    EXRVersion version;
    Check(ParseEXRVersionFromMemory(&version, kCve2026110597Poc,
                                    sizeof(kCve2026110597Poc)) ==
              TINYEXR_SUCCESS,
          "poc: version parses");
    Check(version.multipart != 0, "poc: version flag says multipart");

    EXRHeader **headers = NULL;
    int num_headers = 0;
    const char *err = NULL;
    int ret = ParseEXRMultipartHeaderFromMemory(
        &headers, &num_headers, &version, kCve2026110597Poc,
        sizeof(kCve2026110597Poc), &err);
    // Patched code fails inside part 1's ParseEXRHeader: the corrected
    // marker_size makes ReadAttribute's guard reject the 256-byte
    // attribute. Unpatched code over-reads instead and fails later in
    // ConvertHeader ("# of channels is zero."), so the error text is
    // the discriminator in a plain (non-ASan) build.
    Check(ret == TINYEXR_ERROR_INVALID_DATA,
          "poc: multipart header parse fails with INVALID_DATA");
    Check(num_headers == 0, "poc: no headers produced");
    Check(headers == NULL, "poc: headers pointer stays NULL");
    Check(err != NULL, "poc: error message set");
    Check(err != NULL &&
              std::string(err).find("Failed to read attribute") !=
                  std::string::npos,
          "poc: error is the ReadAttribute guard, not a later failure");
    if (err) FreeEXRErrorMessage(err);
    FreeParsedHeaders(headers, num_headers);
}

// S2: HeaderInfo::header_len is size_t, so ParseEXRHeader reports the
// true consumed byte count even for a part header longer than 4 GiB
// (before, the static_cast<unsigned int> at the assignment wrapped
// it and the CVE-2026-110597 check could be defeated by a wrapped
// small value). A >4 GiB end-to-end repro is infeasible in CI - no
// such file can be built or parsed there - so this pins the
// accounting on a medium-sized real header instead of faking it:
// part 0 of the CVE PoC above. Its attribute list ends with the
// single 0x00 terminator at PoC offset 349 (byte 350 starts part
// 1's "channels" attribute), so starting at offset 8 the parser
// must consume 349 - 8 + 1 == 342 bytes.
static_assert(std::is_same<decltype(std::declval<tinyexr::HeaderInfo>().header_len),
                           size_t>::value,
              "S2: HeaderInfo::header_len must be size_t");

void TestHeaderLenAccounting() {
    EXRVersion version;
    Check(ParseEXRVersionFromMemory(&version, kCve2026110597Poc,
                                    sizeof(kCve2026110597Poc)) ==
              TINYEXR_SUCCESS,
          "header_len: version parses");
    tinyexr::HeaderInfo info;
    std::string err;
    int ret = tinyexr::ParseEXRHeader(
        &info, NULL, &version, &err, kCve2026110597Poc + 8,
        sizeof(kCve2026110597Poc) - 8);
    Check(ret == TINYEXR_SUCCESS, "header_len: part 0 header parses");
    Check(info.header_len == 342,
          "header_len: part 0 consumed byte count is 342");
    // The public EXRHeader still carries the value through
    // ConvertHeader (narrowed with an explicit cast; see the S2
    // note there).
    EXRHeader hdr;
    InitEXRHeader(&hdr);
    std::string warn;
    bool ok = tinyexr::ConvertHeader(&hdr, info, &warn, &err);
    Check(ok, "header_len: ConvertHeader succeeds for part 0");
    if (ok) {
        Check(hdr.header_len == 342,
              "header_len: public header carries 342");
        FreeEXRHeader(&hdr);
    }
}

void SetupPart(EXRHeader *hdr, EXRImage *img, const char *name, int w,
               int h, float base) {
    InitEXRHeader(hdr);
    InitEXRImage(img);
    hdr->num_channels = 3;
    hdr->channels =
        (EXRChannelInfo *)malloc(sizeof(EXRChannelInfo) * 3);
    hdr->pixel_types = (int *)malloc(sizeof(int) * 3);
    hdr->requested_pixel_types = (int *)malloc(sizeof(int) * 3);
    const char *names[3] = {"R", "G", "B"};
    for (int c = 0; c < 3; c++) {
        memset(&hdr->channels[c], 0, sizeof(EXRChannelInfo));
        strcpy(hdr->channels[c].name, names[c]);
        hdr->channels[c].pixel_type = TINYEXR_PIXELTYPE_FLOAT;
        hdr->channels[c].x_sampling = 1;
        hdr->channels[c].y_sampling = 1;
        hdr->pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
        hdr->requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
    }
    hdr->compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
    hdr->data_window.min_x = 0;
    hdr->data_window.min_y = 0;
    hdr->data_window.max_x = w - 1;
    hdr->data_window.max_y = h - 1;
    hdr->display_window = hdr->data_window;
    hdr->pixel_aspect_ratio = 1.0f;
    hdr->line_order = 0;
    hdr->screen_window_center[0] = 0.0f;
    hdr->screen_window_center[1] = 0.0f;
    hdr->screen_window_width = 1.0f;
    EXRSetNameAttr(hdr, name);

    img->width = w;
    img->height = h;
    img->num_channels = 3;
    img->images = (unsigned char **)malloc(sizeof(unsigned char *) * 3);
    for (int c = 0; c < 3; c++) {
        float *plane = (float *)malloc(sizeof(float) * (size_t)w * h);
        for (int i = 0; i < w * h; i++)
            plane[i] = base + (float)c * 10.0f + (float)i;
        img->images[c] = (unsigned char *)plane;
    }
}

void TestValidMultipartRoundTrip() {
    // A valid two-part file must still parse and load after the fix:
    // the fix only changes behaviour when a part header claims more
    // bytes than remain, which cannot happen for a well-formed file.
    const int kW = 8, kH = 4;
    EXRHeader headers[2];
    EXRImage images[2];
    SetupPart(&headers[0], &images[0], "part0", kW, kH, 1.0f);
    SetupPart(&headers[1], &images[1], "part1", kW, kH, 100.0f);

    const EXRHeader *hdrs[2] = {&headers[0], &headers[1]};
    const char *err = NULL;
    unsigned char *mem = NULL;
    size_t mem_size =
        SaveEXRMultipartImageToMemory(images, hdrs, 2, &mem, &err);
    Check(mem_size > 0, "multipart: writer produces bytes");
    Check(mem != NULL, "multipart: writer returns a buffer");
    if (err) FreeEXRErrorMessage(err);
    if (mem_size == 0 || !mem) return;

    EXRVersion version;
    Check(ParseEXRVersionFromMemory(&version, mem, mem_size) ==
              TINYEXR_SUCCESS,
          "multipart: written file version parses");
    Check(version.multipart != 0, "multipart: written file is multipart");

    EXRHeader **parsed = NULL;
    int num = 0;
    err = NULL;
    int ret = ParseEXRMultipartHeaderFromMemory(&parsed, &num, &version,
                                                mem, mem_size, &err);
    Check(ret == TINYEXR_SUCCESS, "multipart: headers parse");
    Check(num == 2, "multipart: two headers parsed");
    if (err) FreeEXRErrorMessage(err);
    if (ret == TINYEXR_SUCCESS && num == 2 && parsed) {
        Check(strcmp(parsed[0]->name, "part0") == 0,
              "multipart: part 0 name");
        Check(strcmp(parsed[1]->name, "part1") == 0,
              "multipart: part 1 name");
        Check(parsed[0]->num_channels == 3, "multipart: part 0 channels");
        Check(parsed[1]->num_channels == 3, "multipart: part 1 channels");
        Check(parsed[0]->data_window.max_x -
                      parsed[0]->data_window.min_x + 1 ==
                  kW,
              "multipart: part 0 width");
        Check(parsed[0]->data_window.max_y -
                      parsed[0]->data_window.min_y + 1 ==
                  kH,
              "multipart: part 0 height");
    }

    EXRImage out[2];
    for (int i = 0; i < 2; i++) InitEXRImage(&out[i]);
    err = NULL;
    ret = LoadEXRMultipartImageFromMemory(
        out, (const EXRHeader **)parsed, 2, mem, mem_size, &err);
    Check(ret == TINYEXR_SUCCESS, "multipart: images load");
    if (err) FreeEXRErrorMessage(err);
    if (ret == TINYEXR_SUCCESS) {
        // part0 R[0] = 1, part1 B[5] = 100 + 20 + 5 = 125.
        Check(((float *)out[0].images[0])[0] == 1.0f,
              "multipart: part 0 pixel value");
        Check(((float *)out[1].images[2])[5] == 125.0f,
              "multipart: part 1 pixel value");
    }
    for (int i = 0; i < 2; i++) FreeEXRImage(&out[i]);
    FreeParsedHeaders(parsed, num);
    free(mem);
    for (int i = 0; i < 2; i++) {
        FreeEXRHeader(&headers[i]);
        FreeEXRImage(&images[i]);
    }
}

void TestSinglePartStillLoads() {
    // The single-part path shares ReadAttribute but was never
    // affected; a save -> load round-trip pins that it still works.
    float rgb[4 * 3] = {0.0f, 0.5f, 1.0f, 0.25f, 0.75f, 0.125f,
                        1.0f, 1.0f, 1.0f, 0.0f, 0.0f,  0.0f};
    unsigned char *single = NULL;
    const char *err = NULL;
    int ssize = SaveEXRToMemory(rgb, 2, 2, 3, 0, &single, &err);
    Check(ssize > 0, "single: writer produces bytes");
    Check(single != NULL, "single: writer returns a buffer");
    if (err) FreeEXRErrorMessage(err);
    if (ssize <= 0 || !single) return;

    float *rgba = NULL;
    int w = 0, h = 0;
    err = NULL;
    int ret =
        LoadEXRFromMemory(&rgba, &w, &h, single, (size_t)ssize, &err);
    Check(ret == TINYEXR_SUCCESS, "single: image loads");
    Check(w == 2 && h == 2, "single: dimensions");
    if (err) FreeEXRErrorMessage(err);
    if (ret == TINYEXR_SUCCESS && rgba) {
        Check(rgba[4] == 0.25f && rgba[5] == 0.75f &&
                  rgba[6] == 0.125f && rgba[7] == 1.0f,
              "single: pixel values round-trip");
    }
    free(rgba);
    free(single);
}

}  // namespace

int main() {
    TestCvePoC();
    TestHeaderLenAccounting();
    TestValidMultipartRoundTrip();
    TestSinglePartStillLoads();
    std::printf("tinyexr_multipart_test: %d checks, %d failures\n",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
