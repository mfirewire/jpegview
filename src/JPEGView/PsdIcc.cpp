// PsdIcc.cpp — see PsdIcc.h for the design.
//
// Pure C++ (no MFC/Windows): compiles on Linux as well as in the MSVC
// project (precompiled headers disabled for this file in the vcxproj).
// MSVC constraints observed: no uint64_t (size_t only), no variable
// directly under a case label, no GetTickCount64.
//
// lcms2: "lcms2.h" resolves to the vendored headers on Windows (the
// lcms2/include dir is on the MSVC IncludePath) and to the system
// header on Linux. Only the profile-reading API is used here:
// cmsOpenProfileFromMem / cmsCloseProfile / cmsGetColorSpace /
// cmsReadTag / cmsEvalToneCurveFloat. No transform is built — the TRC
// tables and the primaries matrix are computed directly, so there is
// no lcms2 transform object to manage or leak.

#include "PsdIcc.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// This define is necessary for 32-bit builds to work, for some reason
// (same as ICCProfileTransform.cpp: without it the Win32 link fails
// with LNK2001 unresolved externals for the cms* functions, because
// the lcms2 import library only provides the __imp_ symbols).
#define CMS_DLL
#include "lcms2.h"

namespace PsdIcc {

namespace {

// sRGB reference values (IEC 61966-2-1), as stored in an ICC profile's
// PCS (D50). lcms2 returns these slightly rounded (e.g. R x = 0.4360
// vs 0.4360657), hence the 1e-3 comparison tolerance below.
const double kSrgbRed[3] = { 0.4360657, 0.2224884, 0.0139160 };
const double kSrgbGreen[3] = { 0.385147, 0.716873, 0.097076 };
const double kSrgbBlue[3] = { 0.143048, 0.060606, 0.714096 };
const double kSrgbWhite[3] = { 0.9642, 1.0, 0.8249 };

// The standard sRGB IEC 61966-2-1 profile (HP/Microsoft) stores the
// D65 white point (0.9505, 1.0, 1.0890) in its wtpt tag, NOT the D50
// value above - even though its colorants are the D50 values. The
// sRGB detection must therefore accept either white point; otherwise
// a real sRGB file is mis-detected as non-sRGB and gets a wrong
// primaries matrix (solving M*S=W with D50 colorants and a D65 white
// point gives a diagonal of 0.827/1.022/1.370 - a massive blue shift).
const double kSrgbWhiteD65[3] = { 0.9505, 1.0, 1.0890 };

// sRGB EOTF at 0.5 (see PsdHdr::SrgbEotf16: 32768/65535 linearizes to
// 0.2140482023; lcms2 evaluates an sRGB profile's TRC at 0.5 as
// 0.214041). Compared with tolerance kMatchTolerance.
const double kSrgbTrcAtHalf = 0.2140;

// Comparison tolerance for the sRGB detection (see the header): loose
// enough for the float rounding of stored colorants, tight enough to
// separate Adobe RGB (TRC at 0.5 ~0.2176, colorants far off) and
// ProPhoto (both far off).
const double kMatchTolerance = 1e-3;

// sRGB XYZ -> linear sRGB matrix for the D50 profile connection
// space (the Bradford-adapted sRGB primaries; standard published
// values). ICC colorant tags are stored in PCS, i.e. D50 — the
// sRGB colorants above (R x = 0.4360) are the D50 values, not the
// D65 ones (0.4124) — so the matrix that converts the stored XYZ
// (D50) to linear sRGB must be the D50 matrix, NOT the familiar
// D65 matrix from IEC 61966-2-1. Using the D65 matrix here mixes
// D50 XYZ with a D65 matrix and is visibly wrong: for Adobe RGB it
// gives 1.488 in matrix[0] instead of 1.398, i.e. an Adobe red of
// 0.5 encodes to sRGB 0.604 instead of the correct 0.588 (verified
// against lcms2's own Adobe->sRGB transform and an independent
// numpy computation from the xy primaries). The D50 matrix folds
// the Bradford D50->D65 chromatic adaptation into the fixed matrix
// (D65 matrix x Bradford(D50->D65) equals this matrix to 5e-4), so
// no separate adaptation step is needed and none is attempted.
const double kXyzToSrgb[3][3] = {
    { 3.1338561, -1.6168667, -0.4906146 },
    { -0.9787684, 1.9161415, 0.0334540 },
    { 0.0719453, -0.2289914, 1.4052427 },
};

bool Near3(const double v[3], const double ref[3]) {
    for (int i = 0; i < 3; i++) {
        if (std::fabs(v[i] - ref[i]) > kMatchTolerance) {
            return false;
        }
    }
    return true;
}

bool Near3Xyz(const cmsCIEXYZ* xyz, const double ref[3]) {
    if (xyz == NULL) {
        return false;
    }
    const double v[3] = { xyz->X, xyz->Y, xyz->Z };
    return Near3(v, ref);
}

// Solve a 3x3 linear system A * x = b by Gaussian elimination with
// partial pivoting. Returns false when A is singular (or so close
// that the pivot underflows) — the caller then falls back to sRGB.
bool Solve3x3(const double a[3][3], const double b[3], double x[3]) {
    double m[3][4];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            m[r][c] = a[r][c];
        }
        m[r][3] = b[r];
    }
    for (int col = 0; col < 3; col++) {
        int pivot = col;
        for (int r = col + 1; r < 3; r++) {
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (!(std::fabs(m[pivot][col]) > 0.0)) {
            return false;
        }
        if (pivot != col) {
            for (int c = col; c < 4; c++) {
                const double tmp = m[col][c];
                m[col][c] = m[pivot][c];
                m[pivot][c] = tmp;
            }
        }
        for (int r = col + 1; r < 3; r++) {
            const double f = m[r][col] / m[col][col];
            for (int c = col; c < 4; c++) {
                m[r][c] -= f * m[col][c];
            }
        }
    }
    for (int r = 2; r >= 0; r--) {
        double sum = m[r][3];
        for (int c = r + 1; c < 3; c++) {
            sum -= m[r][c] * x[c];
        }
        x[r] = sum / m[r][r];
    }
    return true;
}

// Build one 65536-entry linearization table from a TRC: table[v] is
// the curve evaluated at v/65535, clamped to [0, 1]. The stored curve
// maps encoded -> linear (see the header), so it is evaluated directly.
void BuildTrcTable(const cmsToneCurve* trc, float* table) {
    for (int v = 0; v < 65536; v++) {
        float lin = cmsEvalToneCurveFloat(trc, (float)v / 65535.0f);
        if (!(lin >= 0.0f)) {
            lin = 0.0f;
        } else if (lin > 1.0f) {
            lin = 1.0f;
        }
        table[v] = lin;
    }
}

void SetIdentity(float matrix[9]) {
    for (int i = 0; i < 9; i++) {
        matrix[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    }
}

// RAII guard for a cmsHPROFILE: closes it on scope exit, including
// when trcStorage.resize throws bad_alloc below (without this, that
// OOM path would leak the handle — the explicit close at the end of
// BuildColorSpaceImpl is never reached on the throw path).
struct ProfileGuard {
    cmsHPROFILE h;
    explicit ProfileGuard(cmsHPROFILE handle) : h(handle) {}
    ~ProfileGuard() { if (h != NULL) cmsCloseProfile(h); }
    ProfileGuard(const ProfileGuard&) = delete;
    ProfileGuard& operator=(const ProfileGuard&) = delete;
};

bool BuildColorSpaceImpl(const uint8_t* iccData, size_t iccSize,
                         PsdHdr::ColorSpace& out,
                         std::vector<float>& trcStorage) {
    if (iccData == NULL || iccSize == 0 || iccSize > (size_t)0xFFFFFFFFu) {
        return false;
    }
    out.trc[0] = NULL;
    out.trc[1] = NULL;
    out.trc[2] = NULL;
    SetIdentity(out.matrix);

    cmsHPROFILE profile = cmsOpenProfileFromMem(iccData, (cmsUInt32Number)iccSize);
    if (profile == NULL) {
        return false;
    }
    ProfileGuard profileGuard(profile);

    bool built = false;
    const cmsColorSpaceSignature space = cmsGetColorSpace(profile);
    if (space == cmsSigRgbData) {
        const cmsToneCurve* rTrc = (const cmsToneCurve*)cmsReadTag(profile, cmsSigRedTRCTag);
        const cmsToneCurve* gTrc = (const cmsToneCurve*)cmsReadTag(profile, cmsSigGreenTRCTag);
        const cmsToneCurve* bTrc = (const cmsToneCurve*)cmsReadTag(profile, cmsSigBlueTRCTag);
        const cmsCIEXYZ* rXyz = (const cmsCIEXYZ*)cmsReadTag(profile, cmsSigRedColorantTag);
        const cmsCIEXYZ* gXyz = (const cmsCIEXYZ*)cmsReadTag(profile, cmsSigGreenColorantTag);
        const cmsCIEXYZ* bXyz = (const cmsCIEXYZ*)cmsReadTag(profile, cmsSigBlueColorantTag);
        const cmsCIEXYZ* wtpt = (const cmsCIEXYZ*)cmsReadTag(profile, cmsSigMediaWhitePointTag);
        if (rTrc != NULL && gTrc != NULL && bTrc != NULL &&
            rXyz != NULL && gXyz != NULL && bXyz != NULL && wtpt != NULL) {
            // sRGB detection: colorants + white point match AND the red
            // TRC at 0.5 matches the sRGB EOTF value. Either check
            // alone could theoretically collide with another space;
            // both together separate sRGB from Adobe RGB and ProPhoto
            // (see the header for the numbers).
            const bool isSrgb =
                Near3Xyz(rXyz, kSrgbRed) && Near3Xyz(gXyz, kSrgbGreen) &&
                Near3Xyz(bXyz, kSrgbBlue) &&
                (Near3Xyz(wtpt, kSrgbWhite) || Near3Xyz(wtpt, kSrgbWhiteD65)) &&
                std::fabs((double)cmsEvalToneCurveFloat(rTrc, 0.5f) - kSrgbTrcAtHalf) <= kMatchTolerance;
            if (!isSrgb) {
                // Primaries matrix: M = [R | G | B] (columns are the
                // XYZ vectors), solve M * S = W, then
                // RGB->XYZ = M * diag(S), and finally
                // matrix = (XYZ->sRGB, D50) * (RGB->XYZ). The
                // colorants are stored in PCS (D50), so the second
                // factor is the D50 sRGB matrix (kXyzToSrgb), which
                // folds the Bradford D50->D65 adaptation into the
                // fixed matrix — see the comment on kXyzToSrgb. All
                // in double; stored as float.
                const double m[3][3] = {
                    { rXyz->X, gXyz->X, bXyz->X },
                    { rXyz->Y, gXyz->Y, bXyz->Y },
                    { rXyz->Z, gXyz->Z, bXyz->Z },
                };
                const double w[3] = { wtpt->X, wtpt->Y, wtpt->Z };
                double s[3] = { 0.0, 0.0, 0.0 };
                if (Solve3x3(m, w, s)) {
                    double rgbToXyz[3][3];
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) {
                            rgbToXyz[r][c] = m[r][c] * s[c];
                        }
                    }
                    double mat[3][3];
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) {
                            mat[r][c] = kXyzToSrgb[r][0] * rgbToXyz[0][c] +
                                        kXyzToSrgb[r][1] * rgbToXyz[1][c] +
                                        kXyzToSrgb[r][2] * rgbToXyz[2][c];
                        }
                    }
                    bool finite = true;
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) {
                            if (!std::isfinite(mat[r][c]) || !std::isfinite(s[c])) {
                                finite = false;
                            }
                        }
                    }
                    if (finite) {
                        const size_t base = trcStorage.size();
                        trcStorage.resize(base + (size_t)3 * 65536);
                        float* tables = trcStorage.data() + base;
                        BuildTrcTable(rTrc, tables + (size_t)0 * 65536);
                        BuildTrcTable(gTrc, tables + (size_t)1 * 65536);
                        BuildTrcTable(bTrc, tables + (size_t)2 * 65536);
                        out.trc[0] = tables + (size_t)0 * 65536;
                        out.trc[1] = tables + (size_t)1 * 65536;
                        out.trc[2] = tables + (size_t)2 * 65536;
                        for (int r = 0; r < 3; r++) {
                            for (int c = 0; c < 3; c++) {
                                out.matrix[r * 3 + c] = (float)mat[r][c];
                            }
                        }
                        built = true;
                    }
                }
            }
        }
    } else if (space == cmsSigGrayData) {
        const cmsToneCurve* kTrc = (const cmsToneCurve*)cmsReadTag(profile, cmsSigGrayTRCTag);
        if (kTrc != NULL) {
            // Gray has no primaries; only the TRC matters. An sRGB
            // grey TRC (0.2140 at 0.5) is the sRGB path, not a custom
            // colour space.
            if (std::fabs((double)cmsEvalToneCurveFloat(kTrc, 0.5f) - kSrgbTrcAtHalf) > kMatchTolerance) {
                const size_t base = trcStorage.size();
                trcStorage.resize(base + (size_t)65536);
                float* table = trcStorage.data() + base;
                BuildTrcTable(kTrc, table);
                out.trc[0] = table;
                out.trc[1] = NULL;
                out.trc[2] = NULL;
                SetIdentity(out.matrix);
                built = true;
            }
        }
    }

    // profile is closed by profileGuard on scope exit.
    if (!built) {
        // Do not leave a half-built ColorSpace behind: the caller only
        // uses `out` when we return true, but keep the invariant that
        // a false return means "no tables, identity matrix". The
        // appended tables (if any) stay in trcStorage — harmless, the
        // caller discards the storage with the false return.
        out.trc[0] = NULL;
        out.trc[1] = NULL;
        out.trc[2] = NULL;
        SetIdentity(out.matrix);
    }
    return built;
}

}  // namespace

bool BuildColorSpace(const uint8_t* iccData, size_t iccSize,
                     PsdHdr::ColorSpace& out, std::vector<float>& trcStorage) {
    out.trc[0] = NULL;
    out.trc[1] = NULL;
    out.trc[2] = NULL;
    SetIdentity(out.matrix);
    try {
        return BuildColorSpaceImpl(iccData, iccSize, out, trcStorage);
    } catch (...) {
        out.trc[0] = NULL;
        out.trc[1] = NULL;
        out.trc[2] = NULL;
        SetIdentity(out.matrix);
        return false;
    }
}

}  // namespace PsdIcc
