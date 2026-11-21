// png_hdr_alpha_test.cpp - pins the HDR PNG alpha rule (e8228df regression).
//
// What this pins: in PngReader::ConvertHdrRowsToPixels (PNGWrapper.cpp
// pass 2) the output alpha is
//   dst[3] = cache.hasAlpha ? (uint8_t)(px[3] / 257) : 0xff;
// and cache.hasAlpha is set in BeginReading from the ORIGINAL color type
// and tRNS validity:
//   hasAlpha = (colorType & PNG_COLOR_MASK_ALPHA) != 0 || tRNS present.
//
// Why it matters: e8228df (Batch 4, P2-B4) copied px[3]/257 unconditionally,
// claiming the rows carry "the file's alpha (0xffff filler when absent)".
// That claim is wrong: libpng's png_set_add_alpha(0xff) filler is 0x00ff
// (255) in 16-bit rows, NOT 0xffff. A 16-bit RGB HDR PNG with no alpha
// and no tRNS therefore got alpha 255/257 = 0 for every pixel - fully
// transparent - and rendered as a black screen (user device test:
// kennylevinsen_test_pq_bt2020.png black at e8228df, normal at 50892e3).
//
// Proven with a libpng probe (/tmp/png_alpha_probe, the exact JPEGView
// transform sequence): test_pq.png and test_pq_p3.png are 16-bit RGB
// (color_type=2, bit_depth=16, no tRNS); after the transforms the rows
// are 4-channel 16-bit and alpha == 255 for all 204,800 pixels, so
// /257 gives 0. The other three HDR converters are safe by the same
// kind of evidence (AVIF: libavif fills maxChannel; HEIF: 8-bit probe,
// alpha 255 for all 12,192,768 px; JXL: probe, alpha 65535 for all
// 204,800 px) - only the PNG path needed this fix.
//
// The production converter is MFC/libpng-coupled and cannot run in this
// Linux suite (no libpng dev in CI), so this test pins the rule itself:
// the truth tables of the two expressions above, mirrored verbatim.
// A mutant that restores the unconditional /257 copy fails the
// hasAlpha=false cases (255 -> 0 instead of 0xff).
//
// Build and run (Linux, no Windows toolchain needed):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView
//       tests/png_hdr_alpha_test.cpp -o /tmp/png_hdr_alpha_test
//   /tmp/png_hdr_alpha_test

#include <cstdint>
#include <cstdio>

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

// Mirrors PNGWrapper.cpp ConvertHdrRowsToPixels pass 2, verbatim.
uint8_t Alpha16To8(uint16_t v, bool hasAlpha) {
    return hasAlpha ? (uint8_t)(v / 257) : 0xff;
}

// Mirrors PNGWrapper.cpp BeginReading: PNG_COLOR_MASK_ALPHA is 0x04 in
// libpng (color types 4 and 6 carry alpha); a tRNS chunk becomes a real
// alpha channel under png_set_expand.
bool HasAlpha(int colorType, bool hasTrns) {
    return (colorType & 0x04) != 0 || hasTrns;
}

void TestAlphaMapping() {
    // No real alpha: forced opaque whatever the 16-bit filler holds.
    // The regression case is v=255: the libpng add_alpha filler for
    // 16-bit rows. The old unconditional /257 copy turned it into 0.
    Check(Alpha16To8(255, false) == 0xff, "no-alpha 255 (filler) -> 0xff");
    Check(Alpha16To8(0, false) == 0xff, "no-alpha 0 -> 0xff");
    Check(Alpha16To8(1, false) == 0xff, "no-alpha 1 -> 0xff");
    Check(Alpha16To8(32768, false) == 0xff, "no-alpha 32768 -> 0xff");
    Check(Alpha16To8(65535, false) == 0xff, "no-alpha 65535 -> 0xff");
    // Real alpha: /257 scaling, 65535 -> 255 exactly.
    Check(Alpha16To8(65535, true) == 255, "alpha 65535 -> 255");
    Check(Alpha16To8(0, true) == 0, "alpha 0 -> 0");
    Check(Alpha16To8(257, true) == 1, "alpha 257 -> 1");
    Check(Alpha16To8(514, true) == 2, "alpha 514 -> 2");
    Check(Alpha16To8(65280, true) == 254, "alpha 65280 -> 254");
    // A real 16-bit alpha of 255/65535 IS nearly transparent, so 255 -> 0
    // here is correct - the distinction the fix preserves is filler 255
    // (no alpha) versus real alpha 255.
    Check(Alpha16To8(255, true) == 0, "real alpha 255 -> 0");
}

void TestHasAlphaDerivation() {
    // Color types without alpha and without tRNS: no alpha.
    Check(!HasAlpha(0, false), "gray, no tRNS -> false");
    Check(!HasAlpha(2, false), "RGB, no tRNS -> false");
    Check(!HasAlpha(3, false), "palette, no tRNS -> false");
    // Color types with alpha: alpha even without tRNS.
    Check(HasAlpha(4, false), "gray+alpha, no tRNS -> true");
    Check(HasAlpha(6, false), "RGBA, no tRNS -> true");
    // tRNS makes any color type carry a real alpha channel (png_set_expand
    // turns it into alpha 0 or max, so /257 is correct for it).
    Check(HasAlpha(0, true), "gray + tRNS -> true");
    Check(HasAlpha(2, true), "RGB + tRNS -> true");
    Check(HasAlpha(3, true), "palette + tRNS -> true");
    Check(HasAlpha(6, true), "RGBA + tRNS -> true");
}

}  // namespace

int main() {
    TestAlphaMapping();
    TestHasAlphaDerivation();
    std::printf("png_hdr_alpha_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
