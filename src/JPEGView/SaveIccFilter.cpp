// SaveIccFilter.cpp — save-path ICC segment filter (N2) and the raw-byte
// sRGB classifier it uses. The filter was moved here verbatim from
// SaveImage.cpp (F-H10) so it can be unit-tested on Linux; only the
// bStripNonSrgbRgb gate (N2) was added. See SaveIccFilter.h for the
// contract and the D50/D65 reference-value rationale.
//
// Pure C++ (no MFC / no Windows headers).

#include "SaveIccFilter.h"

#include <cmath>
#include <cstring>
#include <new>

namespace {

// sRGB references: colourants in PCS (D50), white point D65 or D50.
// Same colourant values and tolerance as
// ICCProfileTransform::IsNoTransformNeeded, so the load side (which
// decides whether to convert) and the save side (which decides whether
// to keep the profile) agree on the PRIMARIES of "sRGB". The white
// point does NOT agree: the load side compares wtpt against a single
// D50-PCS value (0.9642, 1.0, 0.8249) — D65 expressed in that PCS
// (see ICCProfileTransform.cpp:278-282) — while this classifier
// additionally accepts the unadapted D65 triple (0.9505, 1.0, 1.0890).
// TRC is not part of the decision here either (F3 residual).
const double kSrgbRed[3] = { 0.4361, 0.2225, 0.0139 };
const double kSrgbGreen[3] = { 0.3851, 0.7169, 0.0971 };
const double kSrgbBlue[3] = { 0.1431, 0.0606, 0.7141 };
const double kSrgbWhiteD65[3] = { 0.9505, 1.0, 1.0890 };
const double kSrgbWhiteD50[3] = { 0.9642, 1.0, 0.8249 };
const double kMatchTolerance = 0.05;

uint32_t ReadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

double ReadS15Fixed16(const uint8_t* p) {
    // s15Fixed16 is a signed 32-bit fixed-point value; always finite.
    return (double)(int32_t)ReadBe32(p) / 65536.0;
}

double XyzDistance(const double xyz[3], const double ref[3]) {
    double dx = xyz[0] - ref[0];
    double dy = xyz[1] - ref[1];
    double dz = xyz[2] - ref[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Find a tag-table entry by signature. False when absent or when the
// table itself does not fit in the available bytes.
bool FindTag(const uint8_t* pProfile, size_t nProfileSize, const char* pSig,
             uint32_t& nOffset, uint32_t& nSize) {
    uint32_t nTagCount = ReadBe32(pProfile + 128);
    if ((size_t)nTagCount > (nProfileSize - 132) / 12) {
        return false;  // tag table overruns the available bytes
    }
    for (uint32_t i = 0; i < nTagCount; i++) {
        const uint8_t* pEntry = pProfile + 132 + (size_t)i * 12;
        if (std::memcmp(pEntry, pSig, 4) == 0) {
            nOffset = ReadBe32(pEntry + 4);
            nSize = ReadBe32(pEntry + 8);
            return true;
        }
    }
    return false;
}

// Read one XYZType tag (wtpt/rXYZ/gXYZ/bXYZ) into xyz[3]. False when
// the tag is absent, not XYZType, or its data does not fit in the
// available bytes.
bool ReadXyzTag(const uint8_t* pProfile, size_t nProfileSize, const char* pSig, double xyz[3]) {
    uint32_t nOffset = 0, nSize = 0;
    if (!FindTag(pProfile, nProfileSize, pSig, nOffset, nSize)) {
        return false;
    }
    // XYZType is 4 (type) + 4 (reserved) + 12 (three s15Fixed16).
    if (nSize < 20) {
        return false;
    }
    if ((size_t)nOffset + 20 > nProfileSize) {
        return false;
    }
    const uint8_t* pData = pProfile + nOffset;
    if (std::memcmp(pData, "XYZ ", 4) != 0) {
        return false;
    }
    xyz[0] = ReadS15Fixed16(pData + 8);
    xyz[1] = ReadS15Fixed16(pData + 12);
    xyz[2] = ReadS15Fixed16(pData + 16);
    return true;
}

}  // namespace

IccSrgbVerdict ClassifyIccProfile(const uint8_t* pProfile, size_t nProfileSize) {
    // Header (128 bytes) + tag count (4 bytes) must be present.
    if (pProfile == NULL || nProfileSize < 132) {
        return IccSrgbVerdict::kUnknown;
    }
    // Only RGB profiles can be sRGB; anything else is the caller's
    // (non-RGB) strip decision, not this classifier's.
    if (std::memcmp(pProfile + 16, "RGB ", 4) != 0) {
        return IccSrgbVerdict::kUnknown;
    }
    double wtpt[3], red[3], green[3], blue[3];
    if (!ReadXyzTag(pProfile, nProfileSize, "wtpt", wtpt) ||
        !ReadXyzTag(pProfile, nProfileSize, "rXYZ", red) ||
        !ReadXyzTag(pProfile, nProfileSize, "gXYZ", green) ||
        !ReadXyzTag(pProfile, nProfileSize, "bXYZ", blue)) {
        // LUT-based or truncated: cannot prove sRGB, keep (fail-safe).
        return IccSrgbVerdict::kUnknown;
    }
    // Colourants are stored in PCS (D50): compare against the D50
    // sRGB values. All three primaries must match — Adobe RGB's blue
    // alone is within tolerance of sRGB blue (0.031), so a
    // single-primary check would pass it.
    if (XyzDistance(red, kSrgbRed) > kMatchTolerance ||
        XyzDistance(green, kSrgbGreen) > kMatchTolerance ||
        XyzDistance(blue, kSrgbBlue) > kMatchTolerance) {
        return IccSrgbVerdict::kNonSrgb;
    }
    // White point: accept D65 (real Photoshop/Apple profiles) or D50
    // (lcms2-built profiles).
    if (XyzDistance(wtpt, kSrgbWhiteD65) > kMatchTolerance &&
        XyzDistance(wtpt, kSrgbWhiteD50) > kMatchTolerance) {
        return IccSrgbVerdict::kNonSrgb;
    }
    return IccSrgbVerdict::kSrgb;
}

bool FilterNonRGBICCProfileSegments(const unsigned char* pSegments, int nSegmentsLen, int nSegmentsBeforeEXIF,
    unsigned char** ppFilteredSegments, int& nFilteredLen, int& nFilteredBeforeEXIF, bool bStripNonSrgbRgb) {
    *ppFilteredSegments = NULL;
    nFilteredLen = nSegmentsLen;
    nFilteredBeforeEXIF = nSegmentsBeforeEXIF;
    if (pSegments == NULL || nSegmentsLen < 18) {
        return false;
    }
    // Pass 1: find chunk 1 and read its colour-space signature.
    // Segment layout: FF E2, length (counts itself, not the marker),
    // "ICC_PROFILE\0" (12 bytes), chunk number, chunk count, profile
    // data. The signature is profile bytes 16..19, i.e. segment
    // offsets 34..37, so chunk 1 needs at least 20 profile bytes
    // (block length >= 36). The profile data itself starts at segment
    // offset 18, so chunk 1 holds nBlockLength - 16 profile bytes.
    int nIndex = 0;
    bool bIsNonRGB = false;
    bool bIsNonSrgbRgb = false;
    bool bFoundChunk1 = false;
    while (nIndex + 4 <= nSegmentsLen) {
        if (pSegments[nIndex] != 0xFF) {
            return false;
        }
        int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
        if (nBlockLength < 2) {
            return false;
        }
        int nSegLen = 2 + nBlockLength;
        if (nIndex + nSegLen > nSegmentsLen) {
            return false;
        }
        if (pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
            memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0 &&
            pSegments[nIndex + 16] == 1) {
            if (nBlockLength < 36) {
                return false; // chunk 1 too short for the signature
            }
            bFoundChunk1 = true;
            if (memcmp(pSegments + nIndex + 34, "RGB ", 4) != 0) {
                bIsNonRGB = true;
            } else if (bStripNonSrgbRgb) {
                // N2: an RGB profile that is provably not sRGB does
                // not match the saved (sRGB) pixels. The classifier
                // fail-safes to kUnknown (keep) when the tag table or
                // the tag data does not fit in chunk 1's profile
                // bytes, e.g. a multi-chunk profile whose colourant
                // tags spill past chunk 1.
                bIsNonSrgbRgb = ClassifyIccProfile(pSegments + nIndex + 18,
                    (size_t)(nBlockLength - 16)) == IccSrgbVerdict::kNonSrgb;
            }
            break;
        }
        nIndex += nSegLen;
    }
    if (!bFoundChunk1 || (!bIsNonRGB && !bIsNonSrgbRgb)) {
        return false; // no ICC profile, sRGB/unclassifiable RGB profile, or no chunk 1: keep
    }
    // Pass 2: validate the whole chain and count the ICC bytes. The
    // chain was checked only up to chunk 1 above, so a malformed
    // segment after chunk 1 must still keep the original.
    int nICCBytes = 0;
    nIndex = 0;
    while (nIndex < nSegmentsLen) {
        if (nIndex + 4 > nSegmentsLen || pSegments[nIndex] != 0xFF) {
            return false;
        }
        int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
        if (nBlockLength < 2) {
            return false;
        }
        int nSegLen = 2 + nBlockLength;
        if (nIndex + nSegLen > nSegmentsLen) {
            return false;
        }
        if (pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
            memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0) {
            nICCBytes += nSegLen;
        }
        nIndex += nSegLen;
    }
    // nSegmentsLen is capped at 16 MB by the F-H10 walker and
    // nICCBytes <= nSegmentsLen, so the subtraction cannot underflow.
    nFilteredLen = nSegmentsLen - nICCBytes;
    if (nFilteredLen == 0) {
        nFilteredBeforeEXIF = 0;
        return true; // *ppFilteredSegments stays NULL: nothing left
    }
    unsigned char* pFiltered = new(std::nothrow) unsigned char[nFilteredLen];
    if (pFiltered == NULL) {
        nFilteredLen = nSegmentsLen;
        nFilteredBeforeEXIF = nSegmentsBeforeEXIF;
        return false; // allocation failed: keep the original
    }
    // Pass 3: copy every non-ICC segment. The chain was fully
    // validated in pass 2, so the lengths below are known good.
    // nSegmentsBeforeEXIF is a byte offset between segments (the
    // walker sets it at a kept-size boundary), so a kept segment lies
    // wholly before the split iff it started before it.
    nIndex = 0;
    int nDst = 0;
    nFilteredBeforeEXIF = 0;
    while (nIndex < nSegmentsLen) {
        int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
        int nSegLen = 2 + nBlockLength;
        bool bIsICC = pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
            memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0;
        if (!bIsICC) {
            memcpy(pFiltered + nDst, pSegments + nIndex, nSegLen);
            if (nIndex < nSegmentsBeforeEXIF) {
                nFilteredBeforeEXIF += nSegLen;
            }
            nDst += nSegLen;
        }
        nIndex += nSegLen;
    }
    *ppFilteredSegments = pFiltered;
    return true;
}
