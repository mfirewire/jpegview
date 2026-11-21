// GainMap.cpp — shared gain-map (ISO 21496-1 / UltraHDR) helper.
//
// Pure C++17, no Windows/MFC, deterministic and side-effect free.
// See GainMap.h for the format notes and the reconstruction formula.

#include "GainMap.h"

#include <algorithm>
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

uint16_t ReadU16Le(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 static_cast<uint16_t>(p[1] << 8));
}

uint32_t ReadU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// --- JPEG marker scan (shared by FindGainMapJpeg / FindIso21496Payload) ---

// Advance `pos` (starting just after the SOI) to the next segment.
// On success sets `marker`, `segData` (first payload byte, after the
// 2-byte length field) and `segSize` (payload byte count = length
// field minus 2), and `pos` to the start of the NEXT segment. Returns
// false at SOS (scan data follows, no more visible markers), at EOI
// or any standalone marker, at the end of the buffer, or when a
// segment length runs past the buffer (truncated / malformed JPEG).
// A 0xFF fill byte before a marker code is skipped.
bool NextJpegSegment(const uint8_t* jpeg, size_t size, size_t& pos,
                     uint8_t& marker, const uint8_t*& segData, size_t& segSize) {
    segData = nullptr;
    segSize = 0;
    while (pos + 1 < size) {
        if (jpeg[pos] != 0xFF) {
            return false;  // not a marker: malformed header
        }
        marker = jpeg[pos + 1];
        if (marker == 0xFF) {
            ++pos;  // fill byte before the marker code
            continue;
        }
        pos += 2;
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            return false;  // standalone marker (SOI/EOI/TEM/RSTn): no segments
        }
        if (marker == 0xDA) {
            return false;  // SOS: scan data follows
        }
        if (pos + 2 > size) {
            return false;  // truncated length field
        }
        uint16_t len = ReadU16Be(jpeg + pos);
        if (len < 2 || pos + len > size) {
            return false;  // truncated segment
        }
        segData = jpeg + pos + 2;
        segSize = static_cast<size_t>(len) - 2;
        pos += len;
        return true;
    }
    return false;
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
    // P3-A3: strtod reads past `limit` — bound the numerator's parsed
    // extent. A number that starts inside the range but runs past the
    // closing tag is not this field's value.
    if (static_cast<size_t>(end - s.c_str()) > limit) {
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
        // P3-A3: bound the denominator's parsed extent as well.
        // strtod skips leading whitespace, so a denominator whose
        // digits start inside the range but end beyond it must be
        // rejected, not silently accepted (e.g. "1/" + 1022 spaces +
        // "2" parsed as 0.5 past the 1024-char window).
        if (static_cast<size_t>(denEnd - s.c_str()) > limit) {
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
    // P3-A3: skip whitespace after the field name. A blank
    // self-closing element (<hdrgm:Gamma />) must be treated the same
    // as <hdrgm:Gamma/> — without this, the '/' is missed and the
    // value search runs past the element and steals the next field's
    // number (verified: <hdrgm:Gamma /> stole gamma=2.0 from the
    // following GainMapMax).
    while (after < xmp.size() && (xmp[after] == ' ' || xmp[after] == '\t' ||
                                  xmp[after] == '\r' || xmp[after] == '\n')) {
        ++after;
    }
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

    // Apple HDRGainMap XMP fallback. UNVERIFIED (see GainMap.h): the
    // headroom -> gainMapMax (log2) mapping is a best guess from the
    // field names and needs a real iPhone file WITH HDRGainMapHeadroom
    // to confirm (P2-A2). The version-only XMP form, by contrast, is
    // confirmed real (two iPhone HEICs) and is handled by
    // DeriveAppleHeadroomFromGain, not here.
    if (!anyHdrgm) {
        double headroom = 0.0;
        if (FindXmpField(xmp, "HDRGainMapHeadroom", headroom)) {
            double appleGamma = 1.0;
            bool hasAppleGamma = FindXmpField(xmp, "HDRGainMapGamma", appleGamma);
            // N1/N-5: "headroom invalid" includes <= 0 (a headroom of
            // zero or fewer stops is meaningless), not just
            // unparseable/non-finite. A negative headroom used to be
            // accepted here and passed through to ApplyGainMap.
            if (!std::isfinite(headroom) || headroom <= 0.0) {
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

// N1: presence checks for the Apple HDRGainMap XMP fields. See the
// GainMap.h declarations for why a plain substring search is used
// (FindXmpField conflates "absent" with "present but unparseable").
// The field NAME is what is searched for, the same search FindXmpField
// starts from. Note the xmlns declaration itself
// ("http://ns.apple.com/HDRGainMap/1.0/") contains "HDRGainMap" but
// neither "HDRGainMapVersion" nor "HDRGainMapHeadroom", so it cannot
// trigger a false presence.
bool XmpHasAppleGainMapVersion(const std::string& xmp) {
    return xmp.find("HDRGainMapVersion") != std::string::npos;
}

bool XmpHasAppleGainMapHeadroom(const std::string& xmp) {
    return xmp.find("HDRGainMapHeadroom") != std::string::npos;
}

bool DeriveAppleHeadroomFromGain(const uint8_t* gainMap, size_t gainSize,
                                 const std::string& xmp, Metadata& out) {
    out = Metadata();  // valid=false unless this function succeeds
    if (gainMap == nullptr || gainSize == 0) {
        return false;
    }
    if (!XmpHasAppleGainMapVersion(xmp) || XmpHasAppleGainMapHeadroom(xmp)) {
        return false;
    }
    // 256-bin histogram; p99.9 = the smallest byte v with at least
    // floor(99.9% * n) pixels <= v — the same integer rule the
    // verification probe used (probe_p999: cumulative >= n*999/1000),
    // so the two real files reproduce exactly 3.000 and 1.329 stops.
    // The 64-bit multiply cannot overflow: gainSize is bounded by the
    // callers' IsImageBufferSizeOk (<= 4 GB). `need` is clamped to at
    // least 1 so a 1-pixel gain derives from that pixel (for n=1 the
    // raw floor rule gives 0, which would always pick v=0).
    size_t hist[256] = {};
    for (size_t i = 0; i < gainSize; ++i) {
        ++hist[gainMap[i]];
    }
    uint64_t need = (static_cast<uint64_t>(gainSize) * 999u) / 1000u;
    if (need < 1) {
        need = 1;
    }
    uint64_t cumulative = 0;
    int p999 = 255;
    for (int v = 0; v < 256; ++v) {
        cumulative += hist[v];
        if (cumulative >= need) {
            p999 = v;
            break;
        }
    }
    // H_CAP = 3.0 stops (see GainMap.h): p99.9 = 255 -> 3.0, all
    // zeros -> 0.0 (identity: boost 1.0 everywhere).
    double headroom = 3.0 * static_cast<double>(p999) / 255.0;
    if (headroom < 0.0) {
        headroom = 0.0;
    }
    if (headroom > 3.0) {
        headroom = 3.0;
    }
    // Gamma is re-read from the SAME XMP string that carried the
    // version (N-5): TryParseGainMapXmp discards the XMP on failure,
    // so the derive path must re-read it here. Present-and-valid wins,
    // otherwise 1.0 — an invalid gamma never rejects the derivation.
    double gamma = 1.0;
    double parsedGamma = 1.0;
    if (FindXmpField(xmp, "HDRGainMapGamma", parsedGamma) &&
        parsedGamma > 0.0 && std::isfinite(parsedGamma)) {
        gamma = parsedGamma;
    }
    out.gainMapMin = 0.0;
    out.gainMapMax = headroom;
    out.gamma = gamma;
    out.baseOffset = 0.0;
    out.alternateOffset = 0.0;
    out.appleHeadroomFallback = true;
    out.derivedHeadroom = true;
    out.valid = true;
    return true;
}

bool FindGainMapJpeg(const uint8_t* jpeg, size_t size, size_t& gainOffset, size_t& gainSize) {
    gainOffset = 0;
    gainSize = 0;
    if (jpeg == nullptr || size < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }
    size_t pos = 2;
    uint8_t marker = 0;
    const uint8_t* seg = nullptr;
    size_t segSize = 0;
    while (NextJpegSegment(jpeg, size, pos, marker, seg, segSize)) {
        if (marker != 0xE2 || segSize < 4 || std::memcmp(seg, "MPF\0", 4) != 0) {
            continue;
        }
        // TIFF header starts right after "MPF\0" (marker 2 + length 2
        // + "MPF\0" 4 = 8 bytes before the TIFF header).
        const uint8_t* tiff = seg + 4;
        size_t tiffSize = segSize - 4;
        size_t tiffStart = static_cast<size_t>(tiff - jpeg);
        if (tiffSize < 8) {
            return false;
        }
        bool bigEndian = false;
        if (tiff[0] == 'M' && tiff[1] == 'M') {
            bigEndian = true;
        } else if (tiff[0] == 'I' && tiff[1] == 'I') {
            bigEndian = false;
        } else {
            return false;  // not a TIFF header
        }
        auto rd16 = [&](const uint8_t* p) -> uint16_t {
            return bigEndian ? ReadU16Be(p) : ReadU16Le(p);
        };
        auto rd32 = [&](const uint8_t* p) -> uint32_t {
            return bigEndian ? ReadU32Be(p) : ReadU32Le(p);
        };
        if (rd16(tiff + 2) != 42) {
            return false;  // bad TIFF magic
        }
        uint32_t ifdOff = rd32(tiff + 4);
        // Two-step bound check: ifdOff is untrusted u32, so check it
        // against tiffSize before adding (no wraparound on 32-bit).
        if (ifdOff > tiffSize || ifdOff + 2 > tiffSize) {
            return false;
        }
        uint16_t entryCount = rd16(tiff + ifdOff);
        if (ifdOff + 2 + static_cast<size_t>(entryCount) * 12 > tiffSize) {
            return false;
        }
        // Find 0xB001 (NumberOfImages, LONG x1) and 0xB002 (MP entry
        // list, UNDEFINED, 16 bytes per image).
        bool haveCount = false;
        bool haveList = false;
        uint32_t numImages = 0;
        uint32_t listOff = 0;
        uint32_t listBytes = 0;
        for (uint16_t k = 0; k < entryCount; ++k) {
            const uint8_t* e = tiff + ifdOff + 2 + static_cast<size_t>(k) * 12;
            uint16_t tag = rd16(e);
            uint16_t type = rd16(e + 2);
            uint32_t count = rd32(e + 4);
            if (tag == 0xB001 && type == 4 && count == 1) {
                numImages = rd32(e + 8);
                haveCount = true;
            } else if (tag == 0xB002 && type == 7) {
                // count <= 4 would be inline, but an entry list for
                // >= 1 image is 16 bytes, so it is always an offset.
                if (count <= 4) {
                    return false;
                }
                listOff = rd32(e + 8);
                listBytes = count;
                haveList = true;
            }
        }
        if (!haveCount || !haveList || numImages < 2 || listBytes < 32) {
            return false;
        }
        if (listOff > tiffSize || listOff + 32 > tiffSize) {
            return false;
        }
        // Entry 1 is the gain map (entry 0 is the primary):
        // attr u32, size u32, data_offset u32 (relative to the TIFF
        // header start), then 4 bytes reserved.
        const uint8_t* entry1 = tiff + listOff + 16;
        uint32_t imgSize = rd32(entry1 + 4);
        uint32_t dataOff = rd32(entry1 + 8);
        if (dataOff > size || tiffStart > size - dataOff) {
            return false;
        }
        size_t absOff = tiffStart + dataOff;
        if (imgSize < 2 || imgSize > size - absOff) {
            return false;
        }
        if (jpeg[absOff] != 0xFF || jpeg[absOff + 1] != 0xD8) {
            return false;  // gain-map bytes are not a JPEG
        }
        gainOffset = absOff;
        gainSize = imgSize;
        return true;
    }
    return false;
}

bool FindIso21496Payload(const uint8_t* jpeg, size_t size,
                         const uint8_t*& payload, size_t& payloadSize) {
    payload = nullptr;
    payloadSize = 0;
    if (jpeg == nullptr || size < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }
    static const char kUrn[] = "urn:iso:std:iso:ts:21496:-1";
    const size_t kUrnLen = 27;
    size_t pos = 2;
    uint8_t marker = 0;
    const uint8_t* seg = nullptr;
    size_t segSize = 0;
    while (NextJpegSegment(jpeg, size, pos, marker, seg, segSize)) {
        if (marker == 0xE2 && segSize >= kUrnLen + 1 &&
            std::memcmp(seg, kUrn, kUrnLen) == 0 && seg[kUrnLen] == 0) {
            // P3-A5: skip payloads shorter than the minimum legal
            // ISO 21496-1 payload (89 bytes: 28 URN+NUL + 5 version
            // and flags + 7x8 rational pairs) — a version-only
            // segment (32 bytes) carries no gain-map data and the
            // parser would reject it anyway. The guard lives here,
            // NOT in the MPF finder: the minimal legal MPF segment
            // is 74 bytes, so an 89-byte floor there would break it.
            if (segSize < 89) {
                continue;
            }
            payload = seg;
            payloadSize = segSize;
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

    // P3-A2+P3-B3: clamp the metadata gain range to [-16, +16] stops.
    // A corrupt/hostile file could carry gainMapMin/Max of +/-1e308;
    // logBoost is a convex combination of the two, so it would overflow
    // to +/-Inf and boost = 2^(logBoost) would be Inf (or 0), making
    // every output pixel Inf/NaN. Real files carry logBoost <= ~2.3
    // (two real iPhone HEICs), so this changes nothing on real files.
    // The clamp bounds `boost` only, not the final hdr value: a huge
    // finite offset can still push hdr to +/-Inf, which
    // ConvertLinearPixel maps to white (C1+C2) — worst case one white
    // pixel, no crash.
    const double gainMapMin = std::min(16.0, std::max(-16.0, meta.gainMapMin));
    const double gainMapMax = std::min(16.0, std::max(-16.0, meta.gainMapMax));
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
            gainMapMin * (1.0 - logRecovery) + gainMapMax * logRecovery;
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
