// gainmap_rotate_test.cpp - RotateGrayFree edge-clamping boundary test.
//
// What this pins, and why:
// ------------------------
// RotateGrayFree (GainMap.cpp) resamples a gray image by an arbitrary
// angle into its rotated bounding box, with bilinear sampling and edge
// clamping: the GainMap.h contract says "out-of-image areas take the
// nearest edge value". The sampler clamped x0/y0 only from below
// (x0 < 0 -> 0) and x1/y1 only from above, so when the inverse-rotated
// source coordinate left the source bounds — which happens for ANY
// non-90-degree free rotation, because the destination is the rotated
// bounding box — x0 > srcW-1 / y0 > srcH-1 was never clamped and
// src[y0*srcW + x0] read out of bounds (audit
// ~/workspace/user/files/JPEGView-dbd2aee-.md §1, 2026-11-27; P1).
// Quantified there: a 1512x2016 gain map at 45 degrees maps a
// bounding-box corner to sx ~= 2519.5 (srcW-1 = 1511), max index
// ~= 4.19M against a 3.05M-byte buffer, ~1.1 MB past the end.
// The same gap exists on the negative side: x1 = x0+1 is computed
// before x0 is clamped, so sx < -1 leaves x1 < 0 and the neighbour
// sample reads out of bounds too (the negative bounding-box corner).
// The fix clamps the sample coordinates to the source bounds before
// flooring (so out-of-range samples take the nearest edge value,
// per the GainMap.h contract) and completes the index clamps
// (x0/y0 from above as well). The test rotates known gray images by
// 45 (square) and 15 degrees (non-square) to their bounding boxes
// and compares every output pixel against an independent reference
// implementing the documented convention with full edge clamping.
// It FAILS on the pre-fix code (the corners sample out-of-bounds
// heap garbage) and passes after the fix; under ASan the pre-fix run
// aborts with a heap-buffer-overflow instead of a value mismatch.
//
// Build and run (Linux):
//   g++ -std=c++17 -Wall -Wextra -O2 -I src/JPEGView tests/gainmap_rotate_test.cpp src/JPEGView/GainMap.cpp src/JPEGView/HdrToneMap.cpp -o /tmp/gainmap_rotate_test
//   /tmp/gainmap_rotate_test
// Under ASan (pre-fix this aborts with heap-buffer-overflow):
//   g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address -I src/JPEGView tests/gainmap_rotate_test.cpp src/JPEGView/GainMap.cpp src/JPEGView/HdrToneMap.cpp -o /tmp/gainmap_rotate_test_asan
//   ASAN_OPTIONS=detect_leaks=0 /tmp/gainmap_rotate_test_asan

#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Expected output of dst pixel (i, j), computed from the documented
// convention (GainMap.h: target pixel (i,j) samples the source at
// center + R(-angle) * ((i+offsetX, j+offsetY) - center)) with
// bilinear sampling and edge clamping: the sample coordinate is
// clamped to the source bounds first, so out-of-image areas take
// the nearest edge value.
uint8_t ExpectedPixel(const uint8_t* src, int srcW, int srcH, double angleRad,
                      int offsetX, int offsetY, int i, int j) {
    const double cx = (static_cast<double>(srcW) - 1.0) * 0.5;
    const double cy = (static_cast<double>(srcH) - 1.0) * 0.5;
    const double ca = std::cos(angleRad);
    const double sa = std::sin(angleRad);
    double dx = (static_cast<double>(i) + static_cast<double>(offsetX)) - cx;
    double dy = (static_cast<double>(j) + static_cast<double>(offsetY)) - cy;
    double sx = cx + ca * dx + sa * dy;
    double sy = cy - sa * dx + ca * dy;
    if (sx < 0.0) sx = 0.0;
    if (sx > static_cast<double>(srcW - 1)) sx = static_cast<double>(srcW - 1);
    if (sy < 0.0) sy = 0.0;
    if (sy > static_cast<double>(srcH - 1)) sy = static_cast<double>(srcH - 1);
    int x0 = static_cast<int>(std::floor(sx));
    int y0 = static_cast<int>(std::floor(sy));
    double fx = sx - static_cast<double>(x0);
    double fy = sy - static_cast<double>(y0);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 > srcW - 1) x1 = srcW - 1;
    if (y1 > srcH - 1) y1 = srcH - 1;
    double v00 = src[(size_t)y0 * (size_t)srcW + (size_t)x0];
    double v10 = src[(size_t)y0 * (size_t)srcW + (size_t)x1];
    double v01 = src[(size_t)y1 * (size_t)srcW + (size_t)x0];
    double v11 = src[(size_t)y1 * (size_t)srcW + (size_t)x1];
    double v = (v00 * (1.0 - fx) + v10 * fx) * (1.0 - fy) +
               (v01 * (1.0 - fx) + v11 * fx) * fy;
    int rounded = static_cast<int>(std::floor(v + 0.5));
    if (rounded < 0) rounded = 0;
    if (rounded > 255) rounded = 255;
    return static_cast<uint8_t>(rounded);
}

// Rotate src by angleRad into its bounding box (dst dims from the
// rotated extents, centred on the source centre) and check every
// output pixel against ExpectedPixel.
void CheckCase(const char* name, const std::vector<uint8_t>& src, int srcW, int srcH,
               double angleRad) {
    const double ca = std::cos(angleRad);
    const double sa = std::sin(angleRad);
    const int dstW = (int)std::ceil((double)srcW * std::fabs(ca) + (double)srcH * std::fabs(sa));
    const int dstH = (int)std::ceil((double)srcW * std::fabs(sa) + (double)srcH * std::fabs(ca));
    const int offsetX = (srcW - dstW) / 2;
    const int offsetY = (srcH - dstH) / 2;
    std::vector<uint8_t> dst;
    char what[128];
    std::snprintf(what, sizeof(what), "%s: RotateGrayFree returns true (%dx%d -> %dx%d)",
                  name, srcW, srcH, dstW, dstH);
    Check(gainmap::RotateGrayFree(src.data(), srcW, srcH, angleRad, offsetX, offsetY,
                                  dstW, dstH, dst),
          what);
    std::snprintf(what, sizeof(what), "%s: output size is %dx%d", name, dstW, dstH);
    Check(dst.size() == (size_t)dstW * (size_t)dstH, what);
    if (dst.size() != (size_t)dstW * (size_t)dstH) return;
    // The four corners are the farthest outside the source: they are
    // the samples that read out of bounds pre-fix.
    const int corners[4][2] = {{0, 0}, {dstW - 1, 0}, {0, dstH - 1}, {dstW - 1, dstH - 1}};
    for (int c = 0; c < 4; ++c) {
        const int i = corners[c][0], j = corners[c][1];
        const uint8_t want = ExpectedPixel(src.data(), srcW, srcH, angleRad, offsetX, offsetY, i, j);
        const uint8_t got = dst[(size_t)j * (size_t)dstW + (size_t)i];
        std::snprintf(what, sizeof(what), "%s: corner (%d,%d) is the edge-clamped value (%d)",
                      name, i, j, (int)want);
        Check(got == want, what);
    }
    bool allOk = true;
    for (int j = 0; j < dstH; ++j) {
        for (int i = 0; i < dstW; ++i) {
            const uint8_t want = ExpectedPixel(src.data(), srcW, srcH, angleRad, offsetX, offsetY, i, j);
            if (dst[(size_t)j * (size_t)dstW + (size_t)i] != want) allOk = false;
        }
    }
    std::snprintf(what, sizeof(what), "%s: every pixel matches the edge-clamped reference", name);
    Check(allOk, what);
}

}  // namespace

int main() {
    // Case A: 16x16, every pixel distinct (x*16+y spans 0..255), 45 deg.
    std::vector<uint8_t> srcA(16 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            srcA[(size_t)y * 16 + (size_t)x] = (uint8_t)(x * 16 + y);
    CheckCase("45deg-square", srcA, 16, 16, 3.14159265358979323846 / 4.0);

    // Case B: 24x12 non-square, 15 deg.
    std::vector<uint8_t> srcB(24 * 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 24; ++x)
            srcB[(size_t)y * 24 + (size_t)x] = (uint8_t)((x * 11 + y * 7) % 256);
    CheckCase("15deg-nonsquare", srcB, 24, 12, 3.14159265358979323846 / 12.0);

    // Case C (control): 0 deg is the identity.
    std::vector<uint8_t> dst;
    Check(gainmap::RotateGrayFree(srcA.data(), 16, 16, 0.0, 0, 0, 16, 16, dst),
          "0deg: RotateGrayFree returns true");
    Check(dst == srcA, "0deg: output equals the input");

    std::printf("gainmap_rotate_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
