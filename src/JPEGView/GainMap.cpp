// GainMap.cpp — shared gain-map (ISO 21496-1 / UltraHDR) helper.
//
// Pure C++17, no Windows/MFC, deterministic and side-effect free.
// See GainMap.h for the format notes and the reconstruction formula.

#include "GainMap.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace gainmap {
namespace {

// --- big-endian readers (ISO binary metadata is big-endian) ---

uint16_t ReadU16Be(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                 static_cast<uint16_t>(p[1]));
}

int32_t ReadI32Be(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                static_cast<uint32_t>(p[3]));
}

uint32_t ReadU32Be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// sRGB EOTF inverse in double (c is a normalised sRGB code, 0..1).
double SrgbToLinearDouble(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return std::pow((c + 0.055) / 1.055, 2.4);
}

// Parse the first number (or "num/den" rational) in `s` in the range
// [pos, limit). On success sets `value` and returns true. A rational
// is only recognised when '/' IMMEDIATELY follows the number.
// Rejects when no number is found before `limit`, when the denominator
// is zero/missing, or when the result is non-finite.
bool ParseFirstNumberAfter(const std::string& s, size_t pos, size_t limit, double& value) {
    size_t i = pos;
    while (i < limit) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
            (c == '.' && i + 1 < limit && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            break;
        }
        ++i;
    }
    if (i >= limit) {
        return false;
    }
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    double num = std::strtod(begin, &end);
    if (end == begin) {
        return false;
    }
    value = num;
    if (*end == '/' && static_cast<size_t>(end - s.c_str()) < limit) {
        const char* denBegin = end + 1;
        char* denEnd = nullptr;
        double den = std::strtod(denBegin, &denEnd);
        if (denEnd == denBegin || den == 0.0) {
            return false;
        }
        value = num / den;
    }
    return std::isfinite(value);
}

// Find one XMP field by name and parse its first value (a plain number
// or a "num/den" rational; for an rdf:Seq the first <rdf:li> wins).
// The value search is bounded at the field's closing tag (when present)
// and at 1024 characters past the field name, so a missing value
// cannot swallow a later field's number. A self-closing element
// (<hdrgm:Gamma/>) has no value. Returns false when the field is
// absent or has no parseable value.
bool FindXmpField(const std::string& xmp, const char* field, double& value) {
    size_t pos = xmp.find(field);
    if (pos == std::string::npos) {
        return false;
    }
    size_t after = pos + std::strlen(field);
    if (after < xmp.size() && xmp[after] == '/') {
        return false;  // self-closing element: no value
    }
    std::string closeTag = std::string("</") + field + ">";
    size_t closePos = xmp.find(closeTag, after);
    size_t limit = (closePos == std::string::npos) ? xmp.size() : closePos;
    if (limit - after > 1024) {
        limit = after + 1024;
    }
    return ParseFirstNumberAfter(xmp, after, limit, value);
}

}  // namespace

bool ParseIso21496Metadata(const uint8_t* data, size_t size, Metadata& out) {
    out = Metadata();  // valid=false unless this function succeeds

    // URN "urn:iso:std:iso:ts:21496:-1" (27 chars) + NUL = 28 bytes,
    // then min_ver u16 + writer_ver u16 + flags u8 = 5 bytes,
    // then 7 rational pairs of 8 bytes = 56 bytes. Total 89.
    static const char kUrn[] = "urn:iso:std:iso:ts:21496:-1";
    const size_t kUrnLen = 27;
    const size_t kHeaderBytes = 28 + 5;
    const size_t kNumPairs = 7;
    const size_t kPairBytes = 8;
    if (data == nullptr || size < kHeaderBytes + kNumPairs * kPairBytes) {
        return false;
    }
    if (std::memcmp(data, kUrn, kUrnLen) != 0 || data[kUrnLen] != 0) {
        return false;
    }
    // Versions are accepted as-is (the layout is the version-0 one);
    // a future version may append fields after the 7 pairs, which the
    // size >= 89 check already tolerates.
    uint16_t minVer = ReadU16Be(data + 28);
    uint16_t writerVer = ReadU16Be(data + 30);
    (void)minVer;
    (void)writerVer;
    uint8_t flags = data[32];

    // The 7 rational pairs, in order: [0] baseHdrHeadroom,
    // [1] alternateHdrHeadroom, [2] gainMapMin, [3] gainMapMax,
    // [4] gamma, [5] baseOffset, [6] alternateOffset. For a
    // multi-channel payload (flags bit 7) this is the FIRST channel
    // set; further sets are ignored.
    double vals[kNumPairs];
    for (size_t k = 0; k < kNumPairs; ++k) {
        const uint8_t* p = data + kHeaderBytes + k * kPairBytes;
        int32_t num = ReadI32Be(p);
        uint32_t den = ReadU32Be(p + 4);
        if (den == 0) {
            return false;
        }
        vals[k] = static_cast<double>(num) / static_cast<double>(den);
        if (!std::isfinite(vals[k])) {
            return false;
        }
    }
    if (!(vals[4] > 0.0)) {  // gamma must be > 0 (and finite, checked above)
        return false;
    }

    out.baseHdrHeadroom = vals[0];
    out.alternateHdrHeadroom = vals[1];
    out.gainMapMin = vals[2];
    out.gainMapMax = vals[3];
    out.gamma = vals[4];
    out.baseOffset = vals[5];
    out.alternateOffset = vals[6];
    out.multiChannel = (flags & 0x80) != 0;
    out.useBaseColorSpace = (flags & 0x40) != 0;
    out.valid = true;
    return true;
}

bool ParseXmpMetadata(const std::string& xmp, Metadata& out) {
    out = Metadata();  // valid=false unless this function succeeds

    double gainMapMin = 0.0, gainMapMax = 0.0, gamma = 1.0;
    double baseOffset = 0.0, altOffset = 0.0, hdrMin = 0.0, hdrMax = 0.0;
    bool hasMin = FindXmpField(xmp, "hdrgm:GainMapMin", gainMapMin);
    bool hasMax = FindXmpField(xmp, "hdrgm:GainMapMax", gainMapMax);
    bool hasGamma = FindXmpField(xmp, "hdrgm:Gamma", gamma);
    bool hasBaseOff = FindXmpField(xmp, "hdrgm:OffsetSDR", baseOffset);
    bool hasAltOff = FindXmpField(xmp, "hdrgm:OffsetHDR", altOffset);
    bool hasHdrMin = FindXmpField(xmp, "hdrgm:HDRCapacityMin", hdrMin);
    bool hasHdrMax = FindXmpField(xmp, "hdrgm:HDRCapacityMax", hdrMax);
    bool anyHdrgm = hasMin || hasMax || hasGamma || hasBaseOff || hasAltOff ||
                    hasHdrMin || hasHdrMax;

    if (hasMin && hasMax && hasGamma) {
        if (!(gamma > 0.0) || !std::isfinite(gainMapMin) ||
            !std::isfinite(gainMapMax) || !std::isfinite(gamma)) {
            return false;
        }
        out.gainMapMin = gainMapMin;
        out.gainMapMax = gainMapMax;
        out.gamma = gamma;
        if (hasBaseOff) {
            out.baseOffset = baseOffset;
        }
        if (hasAltOff) {
            out.alternateOffset = altOffset;
        }
        if (hasHdrMin) {
            out.baseHdrHeadroom = hdrMin;
        }
        if (hasHdrMax) {
            out.alternateHdrHeadroom = hdrMax;
        }
        if (!std::isfinite(out.baseOffset) || !std::isfinite(out.alternateOffset) ||
            !std::isfinite(out.baseHdrHeadroom) ||
            !std::isfinite(out.alternateHdrHeadroom)) {
            return false;
        }
        out.valid = true;
        return true;
    }

    // Apple HDRGainMap XMP fallback. UNVERIFIED (see GainMap.h): no
    // Apple sample was on hand; the mapping below is a best guess from
    // the field names and needs a real iPhone file to confirm.
    if (!anyHdrgm) {
        double headroom = 0.0;
        if (FindXmpField(xmp, "HDRGainMapHeadroom", headroom)) {
            double appleGamma = 1.0;
            bool hasAppleGamma = FindXmpField(xmp, "HDRGainMapGamma", appleGamma);
            if (!std::isfinite(headroom)) {
                return false;
            }
            if (hasAppleGamma &&
                (!(appleGamma > 0.0) || !std::isfinite(appleGamma))) {
                return false;
            }
            out.gainMapMax = headroom;
            out.gainMapMin = 0.0;
            out.gamma = hasAppleGamma ? appleGamma : 1.0;
            out.baseOffset = 0.0;
            out.alternateOffset = 0.0;
            out.appleHeadroomFallback = true;
            out.valid = true;
            return true;
        }
    }

    return false;
}

bool ApplyGainMap(const uint8_t* baseSrgb, int width, int height,
                  const uint8_t* gainMap, int gainW, int gainH,
                  const Metadata& meta, double weightFactor, float* outLinear) {
    if (baseSrgb == nullptr || gainMap == nullptr || outLinear == nullptr) {
        return false;
    }
    if (width <= 0 || height <= 0 || gainW <= 0 || gainH <= 0) {
        return false;
    }
    if (!meta.valid) {
        return false;
    }
    if (!(meta.gamma > 0.0) || !std::isfinite(meta.gamma) ||
        !std::isfinite(meta.gainMapMin) || !std::isfinite(meta.gainMapMax) ||
        !std::isfinite(meta.baseOffset) || !std::isfinite(meta.alternateOffset)) {
        return false;
    }
    if (std::isnan(weightFactor)) {
        return false;
    }
    if (weightFactor < 0.0) {
        weightFactor = 0.0;
    }
    if (weightFactor > 1.0) {
        weightFactor = 1.0;
    }

    // Upsample the gain map to base resolution when it differs.
    const uint8_t* gain = gainMap;
    std::vector<uint8_t> upsampled;
    if (gainW != width || gainH != height) {
        upsampled.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        BilinearUpsampleGray(gainMap, gainW, gainH, upsampled.data(), width, height);
        gain = upsampled.data();
    }

    const double invGamma = 1.0 / meta.gamma;
    const size_t pixelCount =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixelCount; ++i) {
        double g = static_cast<double>(gain[i]) / 255.0;
        if (g < 0.0) {
            g = 0.0;
        }
        if (g > 1.0) {
            g = 1.0;
        }
        double logRecovery = std::pow(g, invGamma);
        double logBoost =
            meta.gainMapMin * (1.0 - logRecovery) + meta.gainMapMax * logRecovery;
        double boost = std::pow(2.0, logBoost * weightFactor);
        for (int c = 0; c < 3; ++c) {
            double sdr = SrgbToLinearDouble(
                static_cast<double>(baseSrgb[i * 3 + static_cast<size_t>(c)]) / 255.0);
            double hdr = (sdr + meta.baseOffset) * boost - meta.alternateOffset;
            outLinear[i * 3 + static_cast<size_t>(c)] = static_cast<float>(hdr);
        }
    }
    return true;
}

void BilinearUpsampleGray(const uint8_t* src, int srcW, int srcH,
                          uint8_t* dst, int dstW, int dstH) {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return;
    }
    if (srcW == dstW && srcH == dstH) {
        std::memmove(dst, src,
                     static_cast<size_t>(srcW) * static_cast<size_t>(srcH));
        return;
    }
    // Centre-aligned mapping: dst pixel centres map onto src centres.
    const double scaleX = static_cast<double>(srcW) / static_cast<double>(dstW);
    const double scaleY = static_cast<double>(srcH) / static_cast<double>(dstH);
    for (int dy = 0; dy < dstH; ++dy) {
        double sy = (static_cast<double>(dy) + 0.5) * scaleY - 0.5;
        int y0 = static_cast<int>(std::floor(sy));
        double fy = sy - static_cast<double>(y0);
        int y1 = y0 + 1;
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 > srcH - 1) {
            y1 = srcH - 1;
        }
        // When a coordinate was clamped, both rows/columns are the
        // same source row/column, so the weight is harmless.
        for (int dx = 0; dx < dstW; ++dx) {
            double sx = (static_cast<double>(dx) + 0.5) * scaleX - 0.5;
            int x0 = static_cast<int>(std::floor(sx));
            double fx = sx - static_cast<double>(x0);
            int x1 = x0 + 1;
            if (x0 < 0) {
                x0 = 0;
            }
            if (x1 > srcW - 1) {
                x1 = srcW - 1;
            }
            size_t i00 = static_cast<size_t>(y0) * static_cast<size_t>(srcW) +
                         static_cast<size_t>(x0);
            size_t i10 = static_cast<size_t>(y0) * static_cast<size_t>(srcW) +
                         static_cast<size_t>(x1);
            size_t i01 = static_cast<size_t>(y1) * static_cast<size_t>(srcW) +
                         static_cast<size_t>(x0);
            size_t i11 = static_cast<size_t>(y1) * static_cast<size_t>(srcW) +
                         static_cast<size_t>(x1);
            double v = static_cast<double>(src[i00]) * (1.0 - fx) * (1.0 - fy) +
                       static_cast<double>(src[i10]) * fx * (1.0 - fy) +
                       static_cast<double>(src[i01]) * (1.0 - fx) * fy +
                       static_cast<double>(src[i11]) * fx * fy;
            long rounded = std::lround(v);
            if (rounded < 0) {
                rounded = 0;
            }
            if (rounded > 255) {
                rounded = 255;
            }
            dst[static_cast<size_t>(dy) * static_cast<size_t>(dstW) +
                static_cast<size_t>(dx)] = static_cast<uint8_t>(rounded);
        }
    }
}

float SrgbToLinear(uint8_t v) {
    return static_cast<float>(
        SrgbToLinearDouble(static_cast<double>(v) / 255.0));
}

}  // namespace gainmap
