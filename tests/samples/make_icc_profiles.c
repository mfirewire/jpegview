// make_icc_profiles.c - build the ICC profiles embedded in the H-02
// PSD fixtures (tests/samples/make_psd_hdr_samples.py).
//
// The profiles are built with lcms2, NOT hand-crafted: writing ICC
// bytes by hand is too error-prone (tag tables, checksums, the
// parametric TRC encoding). The generator (make_psd_hdr_samples.py)
// runs this helper when the .icc files are missing; the .icc files
// themselves are checked in next to the fixtures so regenerating the
// PSDs does not need a compiler.
//
//   Adobe RGB (1998): primaries R(0.64,0.33) G(0.21,0.71) B(0.15,0.06),
//     white D65 (0.3127,0.3290), gamma 2.2 per channel.
//   ProPhoto RGB: primaries R(0.7347,0.2653) G(0.1596,0.8404)
//     B(0.0366,0.0001), white D50 (0.3457,0.3585), gamma 1.8.
//   Adobe RGB gray: D65 white, gamma 2.2 (gray TRC only, no primaries).
//
// The same primaries/white/gamma values are used by the C++ unit test
// (tests/psd_hdr_test.cpp), which builds the profiles on the fly with
// the same lcms2 calls and feeds them to PsdIcc::BuildColorSpace.
//
// Build and run:
//   cc make_icc_profiles.c -o /tmp/make_icc_profiles -llcms2
//   /tmp/make_icc_profiles [output-dir]
//   (output-dir defaults to the directory holding this file. If the
//   system has no lcms2 headers / -llcms2, add the vendored headers
//   -I ../../src/JPEGView/lcms2/include and link the runtime library
//   directly with -l:liblcms2.so.2 instead of -llcms2.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcms2.h"

static int WriteProfile(cmsHPROFILE h, const char* path) {
    if (h == NULL) {
        fprintf(stderr, "make_icc_profiles: could not create %s\n", path);
        return 1;
    }
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(h, NULL, &size) || size == 0) {
        fprintf(stderr, "make_icc_profiles: could not size %s\n", path);
        cmsCloseProfile(h);
        return 1;
    }
    void* buf = malloc(size);
    if (buf == NULL) {
        fprintf(stderr, "make_icc_profiles: out of memory for %s\n", path);
        cmsCloseProfile(h);
        return 1;
    }
    if (!cmsSaveProfileToMem(h, buf, &size)) {
        fprintf(stderr, "make_icc_profiles: could not save %s\n", path);
        free(buf);
        cmsCloseProfile(h);
        return 1;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "make_icc_profiles: could not open %s\n", path);
        free(buf);
        cmsCloseProfile(h);
        return 1;
    }
    int rc = 0;
    if (fwrite(buf, 1, size, f) != size) {
        fprintf(stderr, "make_icc_profiles: short write to %s\n", path);
        rc = 1;
    }
    fclose(f);
    free(buf);
    cmsCloseProfile(h);
    if (rc == 0) {
        printf("wrote %s (%u bytes)\n", path, (unsigned)size);
    }
    return rc;
}

static cmsHPROFILE MakeRgb(double rx, double ry, double gx, double gy,
                           double bx, double by, double wx, double wy,
                           double gamma) {
    cmsCIExyY white;
    white.x = wx;
    white.y = wy;
    white.Y = 1.0;
    cmsCIExyYTRIPLE prim;
    prim.Red.x = rx;   prim.Red.y = ry;   prim.Red.Y = 1.0;
    prim.Green.x = gx; prim.Green.y = gy; prim.Green.Y = 1.0;
    prim.Blue.x = bx;  prim.Blue.y = by;  prim.Blue.Y = 1.0;
    cmsToneCurve* trc[3];
    for (int i = 0; i < 3; i++) {
        trc[i] = cmsBuildGamma(NULL, gamma);
        if (trc[i] == NULL) {
            for (int j = 0; j < i; j++) cmsFreeToneCurve(trc[j]);
            return NULL;
        }
    }
    cmsHPROFILE h = cmsCreateRGBProfile(&white, &prim, trc);
    for (int i = 0; i < 3; i++) cmsFreeToneCurve(trc[i]);
    return h;
}

static cmsHPROFILE MakeGray(double wx, double wy, double gamma) {
    cmsCIExyY white;
    white.x = wx;
    white.y = wy;
    white.Y = 1.0;
    cmsToneCurve* trc = cmsBuildGamma(NULL, gamma);
    if (trc == NULL) {
        return NULL;
    }
    cmsHPROFILE h = cmsCreateGrayProfile(&white, trc);
    cmsFreeToneCurve(trc);
    return h;
}

int main(int argc, char** argv) {
    const char* dir = (argc > 1) ? argv[1] : ".";
    char path[1024];
    int rc = 0;

    snprintf(path, sizeof(path), "%s/adobe_rgb.icc", dir);
    rc |= WriteProfile(
        MakeRgb(0.64, 0.33, 0.21, 0.71, 0.15, 0.06, 0.3127, 0.3290, 2.2),
        path);
    snprintf(path, sizeof(path), "%s/prophoto_rgb.icc", dir);
    rc |= WriteProfile(
        MakeRgb(0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001,
                0.3457, 0.3585, 1.8),
        path);
    snprintf(path, sizeof(path), "%s/adobe_rgb_gray.icc", dir);
    rc |= WriteProfile(MakeGray(0.3127, 0.3290, 2.2), path);
    return rc;
}
