/*  cmyk_pixel_test.cpp - end-to-end CMYK pixel test (B-CMYK1)
 *
 *  What this pins, and what it does not:
 *  --------------------------------------
 *  The samples in tests/samples/cmyk_chart_6x1.psd / .jpx are 6x1 strips with
 *  known ink values. This test reads the FILE BYTES, decodes them, runs the
 *  PRODUCT's own conversion (YmckDecode.h - the same header the product
 *  compiles), and asserts the PICTURE: no ink => white, full K => black,
 *  full C => (0,255,255), and so on. It is not a comparison against the
 *  generator's output: the expected RGB values below are the naive-ink
 *  formula evaluated by hand (see the table in ExpectedRgb()).
 *
 *  End-to-end here means: file bytes -> decode -> YmckDecode.h -> RGB.
 *  The decode step is split by format, because the product decoders cannot
 *  run on Linux:
 *
 *    PSD: the product PSDWrapper is MFC-coupled (stdafx.h) and does not
 *         compile on Linux. The test therefore contains a MINIMAL PSD
 *         reader that understands exactly one thing: an uncompressed,
 *         8-bit, 4-channel CMYK PSD, planar C/M/Y/K data - i.e. exactly
 *         the file in tests/samples. It mirrors the product's channel
 *         permutation (PSDWrapper.cpp: rchannel = (-channel - 2) %
 *         nChannels, so file planes C,M,Y,K land in the pixel as
 *         Y,M,C,K) and its single inversion (InvertPlaneInPlace, i.e.
 *         ToInk per byte). It is a TEST reader, not a product decoder.
 *         The sample itself carries no embedded ICC profile, so the
 *         product would take its no-ICC naive branch here - the branch
 *         this test mirrors. The embedded-profile ICC branch is NOT
 *         covered and still needs a Windows device test.
 *
 *    JP2: decoded with OpenJPEG. The vendored copy
 *         (src/JPEGView/libopenjpeg, v2.5.4) ships Windows .lib/.dll
 *         only, so the test links the system library instead
 *         (libopenjp2 2.5.0, same 2.5 line; pkg-config libopenjp2).
 *         The JP2 sample's components are ink amounts (0 = no ink),
 *         exactly as JP2Wrapper.cpp assumes, so InkToRgb applies
 *         directly with NO inversion - unlike PSD. The test asserts the
 *         raw component values equal the ink table first, then the RGB.
 *         Like the PSD case this mirrors the product's NAIVE fallback
 *         (JP2Wrapper.cpp:289/:334); the preferred cmyk.icm ICC route
 *         needs lcms2 + MFC and is NOT covered here.
 *
 *  OpenJPEG 2.5.0 timing quirk (verified while writing this test): after
 *  opj_read_header, image->color_space is still OPJ_CLRSPC_UNSPECIFIED
 *  even for a proper CMYK JP2 - the jp2h colr box is only applied during
 *  opj_decode, after which it reads OPJ_CLRSPC_CMYK. JP2Wrapper reads
 *  isCMYK after opj_decode too, so the test asserts the colour space
 *  AFTER decode, not after the header. Asserting it after the header
 *  would fail on a correct file.
 *
 *  Honest limit (see tests/README.md: a sample generated from the same
 *  assumption as the code can only prove self-consistency): these samples
 *  were generated from the PSD/JP2 specifications, and the polarity
 *  axiom they encode (PSD plane 255 = no ink, JP2 component 0 = no ink)
 *  is NOT proven by this test. The outside proof remains the one in
 *  YmckDecode.h: cmyk_chart_rswop.psd renders correctly on build
 *  19897de and black without the inversion. What this test DOES prove
 *  is the file->pixel pipeline for each format: planar layout, channel
 *  order, the one inversion for PSD, no inversion for JP2, and that the
 *  product conversion turns those pixels into the expected picture.
 *  The product wrappers themselves (PSDWrapper/JP2Wrapper) still need
 *  a Windows device test against these same two files.
 *
 *  Build & run (Linux; the JP2 part needs the system openjpeg dev files):
 *
 *      g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/cmyk_pixel_test.cpp \
 *          -o /tmp/cmyk_pixel_test $(pkg-config --cflags --libs libopenjp2)
 *      /tmp/cmyk_pixel_test [tests/samples]
 *
 *  The samples directory defaults to tests/samples relative to the
 *  current working directory (run from the repository root).
 *
 *  Mutation check (the guard must have teeth): change one value in
 *  kExpectedRgb below, or build with -DYMCK_PLANE_IS_INVERTED=0, and
 *  the test must fail (the latter fails at compile time inside
 *  YmckDecode.h). Both were exercised before this file was committed.
 *
 *  Exit code 0 = all checks passed, 1 = at least one failed.
 */

#include "YmckDecode.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openjpeg.h>

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

void CheckRgb(const YmckDecode::Rgb& rgb, int r, int g, int b, const char* what) {
    ++g_checks;
    if (rgb.r != r || rgb.g != g || rgb.b != b) {
        ++g_failures;
        std::printf("FAIL  %-56s got (%3d,%3d,%3d), expected (%3d,%3d,%3d)\n",
                    what, rgb.r, rgb.g, rgb.b, r, g, b);
    }
}

// One strip, six patches. Ink is (C, M, Y, K); 0 = no ink, 255 = full ink.
// The RGB column is the naive ink formula evaluated by hand, not copied
// from the generator: R = (255-C)*(255-K)/255, G = (255-M)*(255-K)/255,
// B = (255-Y)*(255-K)/255 (integer division, no rounding term - the same
// expression YmckDecode::Screen uses). The K=128 patch pins the rounding:
// (255*127)/255 = 127.
constexpr int kWidth = 6;
constexpr int kHeight = 1;
constexpr std::uint8_t kInk[kWidth][4] = {
    {  0,   0,   0,   0}, // white:   no ink at all
    {  0,   0,   0, 255}, // black:   full K only
    {255,   0,   0,   0}, // cyan:    full C only
    {  0, 255,   0,   0}, // magenta: full M only
    {  0,   0, 255,   0}, // yellow:  full Y only
    {  0,   0,   0, 128}, // mid grey: half K only -> (127,127,127)
};
constexpr int kExpectedRgb[kWidth][3] = {
    {255, 255, 255},
    {  0,   0,   0},
    {  0, 255, 255},
    {255,   0, 255},
    {255, 255,   0},
    {127, 127, 127},
};
const char* kPatchName[kWidth] = {
    "white", "black", "cyan", "magenta", "yellow", "mid-grey K=128",
};

bool ReadFile(const std::string& path, std::vector<std::uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return got == out->size();
}

std::uint16_t Be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t Be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

// --- PSD ---------------------------------------------------------------
// Minimal reader: uncompressed 8-bit 4-channel CMYK PSD only. Anything
// else is rejected, so a wrong sample fails here instead of silently
// decoding as something else.
bool ReadPsdPixels(const std::vector<std::uint8_t>& bytes,
                   std::uint8_t plane[kWidth][4], // [pixel][Y,M,C,K] file order
                   std::uint8_t stored[kWidth][4]) { // [pixel][C,M,Y,K] as stored
    if (bytes.size() < 26) return false;
    if (std::memcmp(bytes.data(), "8BPS", 4) != 0) return false;
    if (Be16(bytes.data() + 4) != 1) return false; // version 1 only
    if (Be16(bytes.data() + 12) != 4) return false; // 4 channels
    if (Be32(bytes.data() + 14) != kHeight) return false;
    if (Be32(bytes.data() + 18) != kWidth) return false;
    if (Be16(bytes.data() + 22) != 8) return false; // 8 bit
    if (Be16(bytes.data() + 24) != 4) return false; // colour mode 4 = CMYK
    std::size_t pos = 26;
    for (int section = 0; section < 3; ++section) { // colour-mode data,
        if (pos + 4 > bytes.size()) return false;   // image resources,
        const std::uint32_t len = Be32(bytes.data() + pos); // reserved
        pos += 4;
        if (pos + len > bytes.size()) return false;
        pos += len;
    }
    if (pos + 2 > bytes.size()) return false;
    if (Be16(bytes.data() + pos) != 0) return false; // 0 = raw, uncompressed
    pos += 2;
    // Planar data, planes in file order C, M, Y, K, one row each here.
    if (bytes.size() - pos != 4u * kWidth * kHeight) return false;
    const std::uint8_t* p = bytes.data() + pos;
    for (int x = 0; x < kWidth; ++x) {
        for (int ch = 0; ch < 4; ++ch)
            stored[x][ch] = p[ch * kWidth + x]; // ch: 0=C, 1=M, 2=Y, 3=K
        // PSDWrapper's permutation: rchannel = (-channel - 2) % nChannels,
        // so file planes C,M,Y,K land in the pixel as Y,M,C,K.
        plane[x][0] = stored[x][2]; // Y
        plane[x][1] = stored[x][1]; // M
        plane[x][2] = stored[x][0]; // C
        plane[x][3] = stored[x][3]; // K
    }
    return true;
}

void TestPsd(const std::string& dir) {
    const std::string path = dir + "/cmyk_chart_6x1.psd";
    std::vector<std::uint8_t> bytes;
    if (!ReadFile(path, &bytes)) {
        Check(false, "PSD: cannot read cmyk_chart_6x1.psd");
        return;
    }
    std::uint8_t plane[kWidth][4];
    std::uint8_t stored[kWidth][4];
    if (!ReadPsdPixels(bytes, plane, stored)) {
        Check(false, "PSD: header/layout is not the expected uncompressed CMYK PSD");
        return;
    }
    ++g_checks; // the header/layout check above
    for (int x = 0; x < kWidth; ++x) {
        char what[96];
        // File-convention check: PSD stores the plane INVERTED
        // (255 = no ink), so the stored byte must be 255 - ink.
        bool storedOk = true;
        for (int ch = 0; ch < 4; ++ch)
            if (stored[x][ch] != static_cast<std::uint8_t>(255 - kInk[x][ch]))
                storedOk = false;
        std::snprintf(what, sizeof what, "PSD %s: stored plane byte is 255 - ink", kPatchName[x]);
        Check(storedOk, what);
        // Product conversion: invert once (ToInk), then YmckToRgb in
        // Y,M,C,K order - the same calls PSDWrapper's no-ICC branch makes.
        const YmckDecode::Rgb rgb = YmckDecode::YmckToRgb(
            YmckDecode::ToInk(plane[x][0]), YmckDecode::ToInk(plane[x][1]),
            YmckDecode::ToInk(plane[x][2]), YmckDecode::ToInk(plane[x][3]));
        std::snprintf(what, sizeof what, "PSD %s -> RGB", kPatchName[x]);
        CheckRgb(rgb, kExpectedRgb[x][0], kExpectedRgb[x][1], kExpectedRgb[x][2], what);
    }
}

// --- JP2 ---------------------------------------------------------------
// Same helper as JP2Wrapper.cpp's JP2SampleTo8 (prec 1..16, unsigned
// re-biased first). The sample is 8-bit, so only the last line runs.
std::uint8_t Jp2SampleTo8(OPJ_INT32 v, OPJ_UINT32 prec, OPJ_UINT32 sgnd) {
    if (sgnd) v += (OPJ_INT32)(1u << (prec - 1));
    if (v < 0) v = 0;
    const OPJ_INT32 maxVal = (OPJ_INT32)((1u << prec) - 1);
    if (v > maxVal) v = maxVal;
    if (prec > 8) return static_cast<std::uint8_t>(v >> (prec - 8));
    if (prec < 8) return static_cast<std::uint8_t>((v * 255) / maxVal);
    return static_cast<std::uint8_t>(v);
}

void TestJp2(const std::string& dir) {
    const std::string path = dir + "/cmyk_chart_6x1.jpx";
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_JP2);
    if (codec == nullptr) {
        Check(false, "JP2: opj_create_decompress failed");
        return;
    }
    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    opj_setup_decoder(codec, &params);
    opj_stream_t* stream = opj_stream_create_default_file_stream(path.c_str(), OPJ_TRUE);
    if (stream == nullptr) {
        Check(false, "JP2: cannot open cmyk_chart_6x1.jpx");
        opj_destroy_codec(codec);
        return;
    }
    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, codec, &image) || image == nullptr) {
        Check(false, "JP2: opj_read_header failed");
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return;
    }
    // Header facts that must hold before decoding. The colour space is
    // deliberately NOT asserted here: on openjpeg 2.5.0 it still reads
    // UNSPECIFIED until opj_decode applies the jp2h colr box (see the
    // file header).
    Check(image->numcomps == 4, "JP2: header numcomps == 4");
    Check(image->x1 - image->x0 == kWidth && image->y1 - image->y0 == kHeight,
          "JP2: header dimensions are 6x1");
    if (!opj_decode(codec, stream, image)) {
        Check(false, "JP2: opj_decode failed");
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return;
    }
    // NOW the colour space is meaningful: enumCS 12 in the colr box.
    Check(image->color_space == OPJ_CLRSPC_CMYK,
          "JP2: color_space is CMYK after decode (colr enumCS 12)");
    Check(image->numcomps == 4, "JP2: numcomps == 4 after decode");
    for (int x = 0; x < kWidth; ++x) {
        char what[96];
        std::uint8_t v[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) {
            const opj_image_comp_t* comp = &image->comps[c];
            v[c] = Jp2SampleTo8(comp->data[x], comp->prec, comp->sgnd);
        }
        // File-convention check: JP2 components ARE ink amounts
        // (0 = no ink) - no inversion, unlike PSD.
        bool inkOk = true;
        for (int c = 0; c < 4; ++c)
            if (v[c] != kInk[x][c]) inkOk = false;
        std::snprintf(what, sizeof what, "JP2 %s: component values are the ink amounts", kPatchName[x]);
        Check(inkOk, what);
        // Product naive fallback: InkToRgb(C,M,Y,K) directly.
        const YmckDecode::Rgb rgb = YmckDecode::InkToRgb(v[0], v[1], v[2], v[3]);
        std::snprintf(what, sizeof what, "JP2 %s -> RGB", kPatchName[x]);
        CheckRgb(rgb, kExpectedRgb[x][0], kExpectedRgb[x][1], kExpectedRgb[x][2], what);
    }
    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/samples";
    std::printf("CMYK pixel test (samples: %s)\n", dir.c_str());
    TestPsd(dir);
    TestJp2(dir);

    if (g_failures == 0) {
        std::printf("PASS  %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d of %d checks\n", g_failures, g_checks);
    return 1;
}
