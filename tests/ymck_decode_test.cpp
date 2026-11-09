/*  ymck_decode_test.cpp - endpoint assertions for YmckDecode.h
 *
 *  This file is deliberately free of Windows/MFC dependencies so it builds and
 *  runs anywhere (CI uses g++ on ubuntu-latest, locally the same command works
 *  in WSL). It exists because the CMYK polarity cannot be settled by reading
 *  the source: only the endpoints of the response curve can pin it down.
 *
 *      g++ -std=c++17 -I src/JPEGView tests/ymck_decode_test.cpp -o ymck_test && ./ymck_test
 *
 *  To watch the guard rail work, build the same test with the polarity
 *  flipped - the compile itself must fail inside YmckDecode.h:
 *
 *      g++ -std=c++17 -DYMCK_PLANE_IS_INVERTED=0 -I src/JPEGView \
 *          tests/ymck_decode_test.cpp -o ymck_test
 *
 *  Exit code 0 = all checks passed, 1 = at least one failed.
 */

#include "YmckDecode.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, int got, int expected) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL  %-58s got %3d, expected %3d\n", what, got, expected);
    }
}

void CheckRgb(const YmckDecode::Rgb& rgb, int r, int g, int b, const char* what) {
    ++g_checks;
    if (rgb.r != r || rgb.g != g || rgb.b != b) {
        ++g_failures;
        std::printf("FAIL  %-58s got (%3d,%3d,%3d), expected (%3d,%3d,%3d)\n",
                    what, rgb.r, rgb.g, rgb.b, r, g, b);
    }
}

// The released expression, written out longhand. Kept separate from
// YmckDecode::Screen on purpose: if both sides were refactored together the
// comparison would prove nothing.
int ReleasedFormula(int inkA, int inkB) {
    return (255 - inkA) * (255 - inkB) / 255;
}

void TestPlaneToInk() {
    Check(YmckDecode::ToInk(255) == 0, "ToInk(255) - paper white", YmckDecode::ToInk(255), 0);
    Check(YmckDecode::ToInk(0) == 255, "ToInk(0) - full ink", YmckDecode::ToInk(0), 255);
    Check(YmckDecode::ToInk(128) == 127, "ToInk(128)", YmckDecode::ToInk(128), 127);
}

void TestEndpoints() {
    // A1: an all-255 plane is paper white.
    const YmckDecode::Rgb white = YmckDecode::YmckToRgb(
        YmckDecode::ToInk(255), YmckDecode::ToInk(255),
        YmckDecode::ToInk(255), YmckDecode::ToInk(255));
    CheckRgb(white, 255, 255, 255, "A1 no ink stays white");

    // A2: an all-0 plane is solid black.
    const YmckDecode::Rgb black = YmckDecode::YmckToRgb(
        YmckDecode::ToInk(0), YmckDecode::ToInk(0),
        YmckDecode::ToInk(0), YmckDecode::ToInk(0));
    CheckRgb(black, 0, 0, 0, "A2 full ink is black");

    // A4: primaries pin the channel order.
    CheckRgb(YmckDecode::YmckToRgb(YmckDecode::ToInk(255), YmckDecode::ToInk(255),
                                   YmckDecode::ToInk(0), YmckDecode::ToInk(255)),
             0, 255, 255, "A4 cyan   (Y plane 255, C plane 0)");
    CheckRgb(YmckDecode::YmckToRgb(YmckDecode::ToInk(255), YmckDecode::ToInk(0),
                                   YmckDecode::ToInk(255), YmckDecode::ToInk(255)),
             255, 0, 255, "A4 magenta (M plane 0)");
    CheckRgb(YmckDecode::YmckToRgb(YmckDecode::ToInk(0), YmckDecode::ToInk(255),
                                   YmckDecode::ToInk(255), YmckDecode::ToInk(255)),
             255, 255, 0, "A4 yellow (Y plane 0)");
}

void TestMonotonicRamps() {
    // A3: every K step must be neutral and strictly darker than the previous.
    int previous = 256;
    bool neutral = true;
    bool darker = true;
    for (int k = 0; k <= 255; ++k) {
        const YmckDecode::Rgb rgb =
            YmckDecode::InkToRgb(0, 0, 0, static_cast<std::uint8_t>(k));
        if (rgb.r != rgb.g || rgb.g != rgb.b) neutral = false;
        if (rgb.r >= previous) darker = false;
        previous = rgb.r;
    }
    Check(neutral, "A3 K-only ramp is neutral for all 256 steps", neutral ? 1 : 0, 1);
    Check(darker, "A3 K-only ramp darkens at every step", darker ? 1 : 0, 1);

    // The same for C, M and Y: each channel alone must move only its own
    // direction, and more ink must never brighten the picture.
    for (int channel = 0; channel < 3; ++channel) {
        int last = 100000; // sums run to 765, so start above any real sum
        bool monotonic = true;
        for (int ink = 0; ink <= 255; ++ink) {
            std::uint8_t c = 0, m = 0, y = 0;
            if (channel == 0) c = static_cast<std::uint8_t>(ink);
            if (channel == 1) m = static_cast<std::uint8_t>(ink);
            if (channel == 2) y = static_cast<std::uint8_t>(ink);
            const YmckDecode::Rgb rgb = YmckDecode::InkToRgb(c, m, y, 0);
            const int sum = rgb.r + rgb.g + rgb.b;
            if (sum >= last) monotonic = false;
            last = sum;
        }
        Check(monotonic, "channel ramp darkens monotonically (C/M/Y)",
              monotonic ? 1 : 0, 1);
    }
}

void TestBitExactAgainstReleasedFormula() {
    // Any deviation here would change output bytes versus the released build.
    for (int a = 0; a <= 255; ++a) {
        for (int b = 0; b <= 255; ++b) {
            const int got = YmckDecode::Screen(static_cast<std::uint8_t>(a),
                                               static_cast<std::uint8_t>(b));
            const int want = ReleasedFormula(a, b);
            if (got != want) {
                ++g_checks;
                ++g_failures;
                std::printf("FAIL  Screen(%d,%d) got %d, released formula gives %d\n",
                            a, b, got, want);
                return;
            }
        }
    }
    ++g_checks; // one aggregate check
}

void TestPlaneInversion() {
    // 4-byte stride: every channel of every pixel inverted, neighbours intact.
    std::vector<std::uint8_t> four(16);
    for (int i = 0; i < 16; ++i) four[i] = static_cast<std::uint8_t>(i * 15);
    YmckDecode::InvertPlaneInPlace(four.data(), 4, 4);
    bool ok4 = true;
    for (int i = 0; i < 16; ++i) {
        if (four[i] != static_cast<std::uint8_t>(255 - i * 15)) ok4 = false;
    }
    Check(ok4, "InvertPlaneInPlace stride 4", ok4 ? 1 : 0, 1);

    // The inversion must be its own inverse: inverting twice restores the input.
    std::vector<std::uint8_t> round(16);
    for (int i = 0; i < 16; ++i) round[i] = static_cast<std::uint8_t>(i * 7);
    const std::vector<std::uint8_t> before = round;
    YmckDecode::InvertPlaneInPlace(round.data(), 4, 4);
    YmckDecode::InvertPlaneInPlace(round.data(), 4, 4);
    Check(std::memcmp(round.data(), before.data(), round.size()) == 0,
          "InvertPlaneInPlace is an involution", 0, 0);

    // 5-byte stride (the dormant CMYK+alpha layout): byte 0 of each pixel is
    // skipped, exactly like the loop it replaced.
    std::vector<std::uint8_t> five(10, 0);
    five[1] = 0x11;
    five[5] = 0x22; // byte 0 of pixel 1: must stay untouched
    YmckDecode::InvertPlaneInPlace(five.data(), 2, 5);
    Check(five[0] == 0x00, "InvertPlaneInPlace stride 5 leaves byte 0", five[0], 0x00);
    Check(five[1] == 0xEE, "InvertPlaneInPlace stride 5 inverts byte 1", five[1], 0xEE);
    Check(five[5] == 0x22, "InvertPlaneInPlace stride 5 skips next pixel's byte 0", five[5], 0x22);

    // count == 0 must not touch the buffer.
    std::uint8_t guard = 0x5A;
    YmckDecode::InvertPlaneInPlace(&guard, 0, 4);
    Check(guard == 0x5A, "InvertPlaneInPlace count 0 is a no-op", guard, 0x5A);
}

} // namespace

int main() {
    std::printf("YmckDecode endpoint test (polarity: plane %s)\n",
                YmckDecode::kPlaneIsInverted ? "inverted, 255 = no ink" : "NOT inverted");
    TestPlaneToInk();
    TestEndpoints();
    TestMonotonicRamps();
    TestBitExactAgainstReleasedFormula();
    TestPlaneInversion();

    if (g_failures == 0) {
        std::printf("PASS  %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED  %d of %d checks\n", g_failures, g_checks);
    return 1;
}
