// GainMap.cpp — shared gain-map (ISO 21496-1 / UltraHDR) helper.
//
// Pure C++17, no Windows/MFC, deterministic and side-effect free.
// See GainMap.h for the format notes and the reconstruction formula.

#include "GainMap.h"
#include "HdrToneMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <memory>
#include <new>

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

// Find the offset of the 0xFF byte of the primary image's first EOI
// (FFD9), or SIZE_MAX when the file has no SOS/EOI (truncated).
// G5: the gain image must start strictly after the primary image's
// EOI — a gain image nominated inside the primary scan data (or in a
// file with no EOI at all) is not a real gain map. The scan walks the
// header segments with NextJpegSegment (which stops at the first SOS),
// skips the SOS header by its 2-byte length field, then scans the
// entropy data: 0xFF00 is a stuffed data byte, repeated 0xFF are fill
// bytes, RSTn (FFD0-FFD7), TEM (FF01) and SOI (FFD8) are standalone
// 2-byte markers, and every other marker (a further SOS in a
// progressive file, DNL, DHT/DQT/DRI/APPn/COM between scans) carries
// a 2-byte length field and is skipped by it. The first FFD9 ends the
// primary image. A length that runs past the buffer, or reaching the
// end of the buffer, means no EOI.
size_t FindPrimaryEoi(const uint8_t* jpeg, size_t size) {
    size_t pos = 2;
    uint8_t marker = 0;
    const uint8_t* seg = nullptr;
    size_t segSize = 0;
    while (NextJpegSegment(jpeg, size, pos, marker, seg, segSize)) {
        // Walk the header segments to the first SOS.
    }
    if (marker != 0xDA) {
        return SIZE_MAX;  // stopped somewhere other than SOS: no scan data
    }
    // pos is just past the FFDA marker: skip the SOS header.
    if (pos + 2 > size) {
        return SIZE_MAX;
    }
    uint16_t sosLen = ReadU16Be(jpeg + pos);
    if (sosLen < 2 || pos + sosLen > size) {
        return SIZE_MAX;
    }
    size_t i = pos + sosLen;
    while (i + 1 < size) {
        if (jpeg[i] != 0xFF) {
            ++i;
            continue;
        }
        uint8_t m = jpeg[i + 1];
        if (m == 0x00) {
            i += 2;  // stuffed 0xFF data byte
            continue;
        }
        if (m == 0xFF) {
            ++i;  // fill byte before the marker code
            continue;
        }
        if (m == 0xD9) {
            return i;  // EOI: offset of its 0xFF byte
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            i += 2;  // standalone marker (SOI/TEM/RSTn)
            continue;
        }
        // Any other marker carries a 2-byte length field.
        if (i + 4 > size) {
            return SIZE_MAX;
        }
        uint16_t len = ReadU16Be(jpeg + i + 2);
        if (len < 2 || i + 2 + len > size) {
            return SIZE_MAX;
        }
        i += 2 + len;
    }
    return SIZE_MAX;
}

// sRGB EOTF inverse in double (c is a normalised sRGB code, 0..1).
double SrgbToLinearDouble(double c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return std::pow((c + 0.055) / 1.055, 2.4);
}

// Shared validation + table building for ApplyGainMap and the fused
// entry points. Validates the metadata and weightFactor (same contract
// as ApplyGainMap), clamps the gain range to [-16, +16] stops
// (P3-A2+P3-B3) and weightFactor to [0,1], and builds the two
// 256-entry tables. Returns false on bad input (tables untouched).
// The clamp comment from ApplyGainMap applies: a corrupt/hostile file
// could carry gainMapMin/Max of +/-1e308; logBoost is a convex
// combination of the two, so it would overflow to +/-Inf and boost =
// 2^(logBoost) would be Inf (or 0). Real files carry logBoost <=
// ~2.3, so this changes nothing on real files.
bool BuildGainTables(const Metadata& meta, double weightFactor,
                     double (&boostTable)[256], double (&srgbTable)[256]) {
    if (!meta.valid) {
        return false;
    }
    if (!(meta.gamma > 0.0) || !std::isfinite(meta.gamma) ||
        !std::isfinite(meta.gainMapMin) || !std::isfinite(meta.gainMapMax) ||
        !std::isfinite(meta.baseOffset) || !std::isfinite(meta.alternateOffset)) {
        return false;
    }
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
    // N1-A (perf): the per-pixel pow calls are replaced by two 256-entry
    // tables, built once per call from the fixed per-image parameters.
    // This is EXACT, not approximate: every input to both functions is
    // a byte (gain byte, sRGB byte) plus parameters that are constant
    // for the whole image, so each table entry is the bit-identical
    // double the direct computation would produce for that byte.
    //   boostTable[g]  = 2^(logBoost(g) * weightFactor),
    //     logBoost(g)  = gainMapMin*(1-g^(1/gamma)) + gainMapMax*g^(1/gamma)
    //   srgbTable[v]   = SrgbToLinearDouble(v / 255)
    // Cost: 757 pow calls per image instead of up to 5 per pixel
    // (~61M at 4032x3024): 512 for the boost table (2 per entry) plus
    // 245 for the sRGB table (the 11 darkest bytes take the linear
    // branch of SrgbToLinearDouble, which calls no pow).
    const double invGamma = 1.0 / meta.gamma;
    for (int b = 0; b < 256; ++b) {
        double g = static_cast<double>(b) / 255.0;
        double logRecovery = std::pow(g, invGamma);
        double logBoost =
            gainMapMin * (1.0 - logRecovery) + gainMapMax * logRecovery;
        boostTable[b] = std::pow(2.0, logBoost * weightFactor);
    }
    for (int v = 0; v < 256; ++v) {
        srgbTable[v] = SrgbToLinearDouble(static_cast<double>(v) / 255.0);
    }
    return true;
}

// Content peak in scene-linear units from byte base + upsampled gain,
// replicating HdrToneMap::ComputePeakLinear on the float buffer
// ApplyGainMap would produce: per channel, hdr = (float)((sdr +
// baseOffset) * boost - alternateOffset), negatives/NaN clamped to 0,
// then the max sRGB luminance (kSrgbLuma). A non-finite peak falls
// back to 1.0 (P2-C2), same as ComputePeakLinear. NOTE: the peak is
// the max LUMINANCE, not the max channel — a saturated pixel can have
// a channel above the peak. `base` is BGR (bytesPerPixel 3) or BGRA
// (bytesPerPixel 4); `gain` is the upsampled gain, width*height bytes.
double PeakLinearFromBytes(const uint8_t* base, size_t baseStride,
                           const uint8_t* gain, int width, int height,
                           const double (&srgbTable)[256],
                           const double (&boostTable)[256],
                           const Metadata& meta, int bytesPerPixel) {
    double peak = 0.0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* baseRow = base + (size_t)y * baseStride;
        const uint8_t* gainRow = gain + (size_t)y * (size_t)width;
        for (int x = 0; x < width; ++x) {
            const uint8_t* bp = baseRow + (size_t)x * (size_t)bytesPerPixel;
            double boost = boostTable[gainRow[x]];
            double r = (double)(float)((srgbTable[bp[2]] + meta.baseOffset) * boost - meta.alternateOffset);
            double g = (double)(float)((srgbTable[bp[1]] + meta.baseOffset) * boost - meta.alternateOffset);
            double b = (double)(float)((srgbTable[bp[0]] + meta.baseOffset) * boost - meta.alternateOffset);
            if (!(r >= 0.0)) r = 0.0;
            if (!(g >= 0.0)) g = 0.0;
            if (!(b >= 0.0)) b = 0.0;
            double yLum = r * HdrToneMap::kSrgbLuma[0] + g * HdrToneMap::kSrgbLuma[1] +
                          b * HdrToneMap::kSrgbLuma[2];
            if (yLum > peak) peak = yLum;
        }
    }
    if (!std::isfinite(peak)) return 1.0;
    return peak;
}

// Core of ApplyGainMapToBgr/ToBgra: gain-map apply + tone map fused
// through a 256x256 table (variant B). `base`/`out` are BGR
// (bytesPerPixel 3) or BGRA (bytesPerPixel 4), JPEGView order; the
// BGRA variant copies the base alpha byte to the output. The table
// holds, per (gain byte, base byte), the display-linear double the
// old per-pixel code feeds GamutMap — built from the same functions,
// in the same order (PqNitsToSignal -> Bt2390EetfPqWithParams ->
// PqSignalToNits -> /203) — so the fused loop is bit-identical to
// ApplyGainMap + ConvertLinearImage(/ToBgra). There is deliberately
// NO fast-path branch in the table build: the old code skips the PQ
// round-trip only when the content peak is <= 1 AND every channel of
// the pixel is <= 1, and a pixel with a channel above 1 in a peak<=1
// image takes the slow path, where the EETF (ks == 1) clamps that
// channel to exactly 1.0. An image-level identity table would feed
// 2.0 to GamutMap for such a pixel and render (255,142,142) instead
// of (255,0,0). The chain table reproduces the slow path for those
// channels; for sub-1.0 channels it differs from the identity only
// by a double pow round-trip (~1e-16), covered by the byte-exact test.
bool FusedApply(const uint8_t* base, int width, int height, size_t baseStride,
                const uint8_t* gainMap, int gainW, int gainH,
                const Metadata& meta, double weightFactor,
                uint8_t* out, size_t outStride, int bytesPerPixel) {
    if (base == nullptr || gainMap == nullptr || out == nullptr) {
        return false;
    }
    if (width <= 0 || height <= 0 || gainW <= 0 || gainH <= 0) {
        return false;
    }
    if (bytesPerPixel != 3 && bytesPerPixel != 4) {
        return false;
    }
    // Stride guards (P2-C5/T3, same shape as ConvertLinearImage):
    // a row must hold width*bytesPerPixel, and y*stride + rowBytes
    // must not wrap size_t for y < height.
    const size_t rowBytes = (size_t)width * (size_t)bytesPerPixel;
    if (baseStride < rowBytes || outStride < rowBytes) {
        return false;
    }
    if (baseStride > ((size_t)-1 - rowBytes) / (size_t)height ||
        outStride > ((size_t)-1 - rowBytes) / (size_t)height) {
        return false;
    }
    double boostTable[256], srgbTable[256];
    if (!BuildGainTables(meta, weightFactor, boostTable, srgbTable)) {
        return false;
    }
    // Upsample the gain map to base resolution when it differs.
    const uint8_t* gain = gainMap;
    std::vector<uint8_t> upsampled;
    if (gainW != width || gainH != height) {
        upsampled.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        BilinearUpsampleGray(gainMap, gainW, gainH, upsampled.data(), width, height);
        gain = upsampled.data();
    }
    // Content peak (max luminance), replicating ComputePeakLinear.
    double peakLinear = PeakLinearFromBytes(base, baseStride, gain, width, height,
                                            srgbTable, boostTable, meta, bytesPerPixel);
    // The 256x256 table, stored as double (512 KB, heap — too large
    // for the stack). See GainMap.h for why double, not float.
    std::unique_ptr<double[]> table(new(std::nothrow) double[256 * 256]);
    if (table == nullptr) {
        return false;
    }
    // EETF parameters, built once (P2-C3). PeakNitsFromLinear is
    // replicated here (it is static in HdrToneMap.cpp): peak nits =
    // peakLinear * kSdrWhiteNits, clamped to >= SDR white.
    double peakNits = peakLinear * HdrToneMap::kSdrWhiteNits;
    if (!(peakNits > 0.0)) peakNits = HdrToneMap::kSdrWhiteNits;
    HdrToneMap::Bt2390Params params = HdrToneMap::Bt2390MakeParams(peakNits);
    for (int g = 0; g < 256; ++g) {
        double boost = boostTable[g];
        for (int b = 0; b < 256; ++b) {
            // Same per-channel value ApplyGainMap stores as a float.
            float hdrF = (float)((srgbTable[b] + meta.baseOffset) * boost - meta.alternateOffset);
            // Same chain ConvertLinearPixelWithParams runs per channel
            // in the slow path: nits -> PQ signal -> EETF -> nits ->
            // display-linear (SDR white = 1.0).
            double sig = HdrToneMap::PqNitsToSignal((double)hdrF * HdrToneMap::kSdrWhiteNits);
            double e = HdrToneMap::Bt2390EetfPqWithParams(sig, params);
            table[(size_t)g * 256 + (size_t)b] =
                HdrToneMap::PqSignalToNits(e) / HdrToneMap::kSdrWhiteNits;
        }
    }
    // Main loop: table lookup -> negative/NaN clamp -> GamutMap ->
    // exact SrgbOetf -> ToByte. No pow, no float linear buffer.
    for (int y = 0; y < height; ++y) {
        const uint8_t* baseRow = base + (size_t)y * baseStride;
        const uint8_t* gainRow = gain + (size_t)y * (size_t)width;
        uint8_t* outRow = out + (size_t)y * outStride;
        for (int x = 0; x < width; ++x) {
            const uint8_t* bp = baseRow + (size_t)x * (size_t)bytesPerPixel;
            const size_t t = (size_t)gainRow[x] * 256;
            double r = table[t + bp[2]];
            double g = table[t + bp[1]];
            double b = table[t + bp[0]];
            if (!(r >= 0.0)) r = 0.0;
            if (!(g >= 0.0)) g = 0.0;
            if (!(b >= 0.0)) b = 0.0;
            HdrToneMap::GamutMap(&r, &g, &b);
            uint8_t* op = outRow + (size_t)x * (size_t)bytesPerPixel;
            op[0] = HdrToneMap::ToByte(HdrToneMap::SrgbOetf(b));
            op[1] = HdrToneMap::ToByte(HdrToneMap::SrgbOetf(g));
            op[2] = HdrToneMap::ToByte(HdrToneMap::SrgbOetf(r));
            if (bytesPerPixel == 4) op[3] = bp[3];
        }
    }
    return true;
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

// G4: whether the XMP packet contains an HDRGainMapGamma field with a
// value — as opposed to missing, or a self-closing element, which
// FindXmpField treats as missing. Mirrors FindXmpField's own search:
// find the field name, skip whitespace, and report present-with-value
// only when the next character is not '/'. A bare substring check
// would wrongly reject self-closing tags and match stray occurrences.
bool AppleGammaFieldPresentWithValue(const std::string& xmp) {
    static const char kField[] = "HDRGainMapGamma";
    size_t pos = xmp.find(kField);
    if (pos == std::string::npos) {
        return false;
    }
    size_t after = pos + std::strlen(kField);
    while (after < xmp.size() && (xmp[after] == ' ' || xmp[after] == '\t' ||
                                  xmp[after] == '\r' || xmp[after] == '\n')) {
        ++after;
    }
    if (after >= xmp.size() || xmp[after] == '/') {
        return false;  // self-closing element (or truncated): missing
    }
    return true;
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

    // Apple HDRGainMap XMP fallback. VERIFIED (P2-A2 settled
    // 2026-11-21): HDRGainMapHeadroom is a LINEAR RATIO (brightest
    // white / SDR white), not log2 stops — an iPhone 15 Pro HEIC
    // carries 3.847906, and the same file's MakerNote independently
    // yields 3.847921 (agreement to 0.0004%). So gainMapMax =
    // log2(ratio); using the raw value as stops would overstate the
    // max boost (3.847906 stops = 14.40x instead of 3.85x). The
    // version-only XMP form (two iPhone 13 HEICs) carries no Headroom
    // and fails here, so those files show SDR — no deriving.
    // Coexistence: this branch is only entered when NO hdrgm field
    // PARSED (!anyHdrgm below) — anyHdrgm is the OR of the seven
    // FindXmpField results above, and FindXmpField returns false for
    // an unparseable or self-closing field, so a malformed hdrgm
    // field plus a valid Apple Headroom WOULD enter this branch. An
    // XMP packet carrying both parseable hdrgm fields and Apple
    // fields therefore never uses the Apple Headroom here: if the
    // hdrgm set is incomplete or invalid, this function returns
    // false and the caller shows SDR — a packet with both hdrgm and
    // a valid Apple Headroom yields no gain map at all. No such
    // packet is known to exist (Apple files carry no hdrgm;
    // UltraHDR files carry no Apple version). Note the HEIF caller
    // (TryApplyHeifGainMap) is currently disabled and returns NULL
    // up front (SDR-for-now, user decision 2026-11-21), so this
    // branch's result presently reaches only the JPEG/AVIF callers.
    if (!anyHdrgm) {
        double headroom = 0.0;
        if (FindXmpField(xmp, "HDRGainMapHeadroom", headroom)) {
            double appleGamma = 1.0;
            bool hasAppleGamma = FindXmpField(xmp, "HDRGainMapGamma", appleGamma);
            // The ratio must be finite and in [1.0, 65536.0] (0 to 16
            // stops). Below 1.0 is physically meaningless (HDR dimmer
            // than SDR); above 65536 is absurd (the ApplyGainMap
            // clamp bound). A ratio of exactly 1.0 is valid with
            // gainMapMax 0.0 (identity: no gain).
            if (!std::isfinite(headroom) || headroom < 1.0 ||
                headroom > 65536.0) {
                return false;
            }
            // G4: an HDRGainMapGamma that is PRESENT but unparseable
            // must reject the packet (like the hdrgm path), not
            // silently default to 1.0. Missing (including
            // self-closing) still defaults to 1.0.
            if (!hasAppleGamma && AppleGammaFieldPresentWithValue(xmp)) {
                return false;
            }
            if (hasAppleGamma &&
                (!(appleGamma > 0.0) || !std::isfinite(appleGamma))) {
                return false;
            }
            out.gainMapMax = std::log2(headroom);
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
    // G5: the primary image's EOI (offset of the FFD9's 0xFF byte),
    // or SIZE_MAX when the file has no SOS/EOI. Computed once: it
    // does not depend on which MPF segment is being examined.
    const size_t eoiPos = FindPrimaryEoi(jpeg, size);
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
            continue;  // G3: malformed MPF segment — skip it, a later
                       // MPF segment may still be valid
        }
        bool bigEndian = false;
        if (tiff[0] == 'M' && tiff[1] == 'M') {
            bigEndian = true;
        } else if (tiff[0] == 'I' && tiff[1] == 'I') {
            bigEndian = false;
        } else {
            continue;  // not a TIFF header
        }
        auto rd16 = [&](const uint8_t* p) -> uint16_t {
            return bigEndian ? ReadU16Be(p) : ReadU16Le(p);
        };
        auto rd32 = [&](const uint8_t* p) -> uint32_t {
            return bigEndian ? ReadU32Be(p) : ReadU32Le(p);
        };
        if (rd16(tiff + 2) != 42) {
            continue;  // bad TIFF magic
        }
        uint32_t ifdOff = rd32(tiff + 4);
        // Two-step bound check: ifdOff is untrusted u32, so check it
        // against tiffSize before adding (no wraparound on 32-bit).
        if (ifdOff > tiffSize || ifdOff + 2 > tiffSize) {
            continue;
        }
        uint16_t entryCount = rd16(tiff + ifdOff);
        if (ifdOff + 2 + static_cast<size_t>(entryCount) * 12 > tiffSize) {
            continue;
        }
        // Find 0xB001 (NumberOfImages, LONG x1) and 0xB002 (MP entry
        // list, UNDEFINED, 16 bytes per image).
        bool haveCount = false;
        bool haveList = false;
        // G3: set when the B002 entry is unusable (count <= 4); the
        // flag is checked after the loop because `continue` here
        // would only continue the IFD-entry scan, not the segment scan.
        bool badMpf = false;
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
                    badMpf = true;
                    break;
                }
                listOff = rd32(e + 8);
                listBytes = count;
                haveList = true;
            }
        }
        if (badMpf) {
            continue;
        }
        if (!haveCount || !haveList || numImages < 2 || listBytes < 32) {
            continue;
        }
        if (listOff > tiffSize || listOff + 32 > tiffSize) {
            continue;
        }
        // Entry 1 is the gain map (entry 0 is the primary):
        // attr u32, size u32, data_offset u32 (relative to the TIFF
        // header start), then 4 bytes reserved.
        const uint8_t* entry1 = tiff + listOff + 16;
        uint32_t imgSize = rd32(entry1 + 4);
        uint32_t dataOff = rd32(entry1 + 8);
        if (dataOff > size || tiffStart > size - dataOff) {
            continue;
        }
        size_t absOff = tiffStart + dataOff;
        if (imgSize < 2 || imgSize > size - absOff) {
            continue;
        }
        if (jpeg[absOff] != 0xFF || jpeg[absOff + 1] != 0xD8) {
            continue;  // gain-map bytes are not a JPEG
        }
        // G5: the gain image must start strictly after the primary
        // image's EOI. eoiPos is the offset of the 0xFF byte of the
        // first FFD9, so eoiPos + 2 is the earliest legal gain offset
        // (real Apple files have EOI at 44819 and gain at 44821 =
        // EOI + 2). A gain image nominated inside the primary scan
        // data — or in a file with no EOI at all — is not a real
        // gain map. `continue`, not `return false` (G3): a later
        // MPF segment may still be valid.
        if (eoiPos == SIZE_MAX || absOff < eoiPos + 2) {
            continue;
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

bool FindXmpPacket(const unsigned char* jpeg, int sizebytes, const unsigned char*& xmp, size_t& xmpSize) {
	xmp = NULL;
	xmpSize = 0;
	if (jpeg == NULL || sizebytes < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
		return false;
	}
	static const char kXmpPrefix[] = "http://ns.adobe.com/xap/1.0/";
	const size_t kPrefixLen = 28;  // + NUL = 29 bytes before the packet
	const size_t size = (size_t)sizebytes;
	size_t pos = 2;
	while (pos + 1 < size) {
		if (jpeg[pos] != 0xFF) {
			return false;
		}
		const unsigned char marker = jpeg[pos + 1];
		if (marker == 0xFF) {
			++pos;  // fill byte before the marker code
			continue;
		}
		pos += 2;
		if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
			(marker >= 0xD0 && marker <= 0xD7) || marker == 0xDA) {
			return false;  // standalone marker or SOS: no more segments
		}
		if (pos + 2 > size) {
			return false;
		}
		const size_t len = (size_t)(((size_t)jpeg[pos] << 8) | (size_t)jpeg[pos + 1]);
		if (len < 2 || pos + len > size) {
			return false;  // truncated segment
		}
		if (marker == 0xE1 && len - 2 >= kPrefixLen + 1 &&
			std::memcmp(jpeg + pos + 2, kXmpPrefix, kPrefixLen) == 0 &&
			jpeg[pos + 2 + kPrefixLen] == 0) {
			xmp = jpeg + pos + 2 + kPrefixLen + 1;
			xmpSize = len - 2 - (kPrefixLen + 1);
			return true;
		}
		pos += len;
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
    double boostTable[256], srgbTable[256];
    if (!BuildGainTables(meta, weightFactor, boostTable, srgbTable)) {
        return false;
    }

    // Upsample the gain map to base resolution when it differs.
    const uint8_t* gain = gainMap;
    std::vector<uint8_t> upsampled;
    if (gainW != width || gainH != height) {
        upsampled.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        BilinearUpsampleGray(gainMap, gainW, gainH, upsampled.data(), width, height);
        gain = upsampled.data();
    }

    const size_t pixelCount =
        static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixelCount; ++i) {
        // gain[i] is a byte, so it is already in [0,1] after /255;
        // the old clamp was a no-op and the table index is the byte.
        double boost = boostTable[gain[i]];
        for (int c = 0; c < 3; ++c) {
            double sdr = srgbTable[baseSrgb[i * 3 + static_cast<size_t>(c)]];
            double hdr = (sdr + meta.baseOffset) * boost - meta.alternateOffset;
            outLinear[i * 3 + static_cast<size_t>(c)] = static_cast<float>(hdr);
        }
    }
    return true;
}

bool ApplyGainMapToBgr(const uint8_t* baseBgr, int width, int height, size_t baseStride,
                       const uint8_t* gainMap, int gainW, int gainH,
                       const Metadata& meta, double weightFactor,
                       uint8_t* outBgr, size_t outStride) {
    return FusedApply(baseBgr, width, height, baseStride, gainMap, gainW, gainH,
                      meta, weightFactor, outBgr, outStride, 3);
}

bool ApplyGainMapToBgra(const uint8_t* baseBgra, int width, int height, size_t baseStride,
                        const uint8_t* gainMap, int gainW, int gainH,
                        const Metadata& meta, double weightFactor,
                        uint8_t* outBgra, size_t outStride) {
    return FusedApply(baseBgra, width, height, baseStride, gainMap, gainW, gainH,
                      meta, weightFactor, outBgra, outStride, 4);
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

// Upper bound for a rotated gain-map buffer (bytes). Gain maps are
// capped at 4x the base per side by the extractors, so a rotated gain
// is far below this in practice; the cap only stops a hostile geometry
// from attempting a multi-GB allocation (the helpers never throw).
static const size_t kMaxRotatedGainBytes = (size_t)1 << 31;

bool RotateGrayQuarterTurns(const uint8_t* src, int srcW, int srcH, int quarterTurns,
                            std::vector<uint8_t>& dst, int& dstW, int& dstH) {
    dstW = 0;
    dstH = 0;
    if (src == nullptr || srcW <= 0 || srcH <= 0) {
        return false;
    }
    int q = ((quarterTurns % 4) + 4) % 4;
    int outW = (q % 2 == 1) ? srcH : srcW;
    int outH = (q % 2 == 1) ? srcW : srcH;
    size_t bytes = static_cast<size_t>(outW) * static_cast<size_t>(outH);
    if (bytes == 0 || bytes > kMaxRotatedGainBytes) {
        return false;
    }
    std::vector<uint8_t> out;
    try {
        out.assign(bytes, 0);
    } catch (...) {
        return false;
    }
    if (q == 0) {
        std::memcpy(out.data(), src, bytes);
    } else if (q == 2) {
        // 180 deg: dst(x, y) = src(srcW-1-x, srcH-1-y).
        for (int y = 0; y < srcH; ++y) {
            for (int x = 0; x < srcW; ++x) {
                out[static_cast<size_t>(srcH - 1 - y) * static_cast<size_t>(srcW) +
                    static_cast<size_t>(srcW - 1 - x)] =
                    src[static_cast<size_t>(y) * static_cast<size_t>(srcW) + static_cast<size_t>(x)];
            }
        }
    } else if (q == 1) {
        // 90 deg clockwise: dst(tx = srcH-1-y, ty = x) = src(x, y).
        for (int y = 0; y < srcH; ++y) {
            for (int x = 0; x < srcW; ++x) {
                out[static_cast<size_t>(x) * static_cast<size_t>(srcH) +
                    static_cast<size_t>(srcH - 1 - y)] =
                    src[static_cast<size_t>(y) * static_cast<size_t>(srcW) + static_cast<size_t>(x)];
            }
        }
    } else {
        // 270 deg clockwise: dst(tx = y, ty = srcW-1-x) = src(x, y).
        for (int y = 0; y < srcH; ++y) {
            for (int x = 0; x < srcW; ++x) {
                out[static_cast<size_t>(srcW - 1 - x) * static_cast<size_t>(srcH) +
                    static_cast<size_t>(y)] =
                    src[static_cast<size_t>(y) * static_cast<size_t>(srcW) + static_cast<size_t>(x)];
            }
        }
    }
    dst.swap(out);
    dstW = outW;
    dstH = outH;
    return true;
}

bool RotateGrayFree(const uint8_t* src, int srcW, int srcH, double angleRad,
                    int offsetX, int offsetY, int dstW, int dstH,
                    std::vector<uint8_t>& dst) {
    if (src == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return false;
    }
    if (!std::isfinite(angleRad)) {
        return false;
    }
    size_t bytes = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
    if (bytes == 0 || bytes > kMaxRotatedGainBytes) {
        return false;
    }
    std::vector<uint8_t> out;
    try {
        out.assign(bytes, 0);
    } catch (...) {
        return false;
    }
    // Same convention as CBasicProcessing::RotateHQ_Core: the target
    // pixel (i,j) samples the source at center + R(-angle) *
    // ((i+offsetX, j+offsetY) - center), where center is the source
    // centre ((srcW-1)/2, (srcH-1)/2). Bilinear with edge clamping.
    const double cx = (static_cast<double>(srcW) - 1.0) * 0.5;
    const double cy = (static_cast<double>(srcH) - 1.0) * 0.5;
    const double ca = std::cos(angleRad);
    const double sa = std::sin(angleRad);
    for (int j = 0; j < dstH; ++j) {
        for (int i = 0; i < dstW; ++i) {
            double dx = (static_cast<double>(i) + static_cast<double>(offsetX)) - cx;
            double dy = (static_cast<double>(j) + static_cast<double>(offsetY)) - cy;
            double sx = cx + ca * dx + sa * dy;
            double sy = cy - sa * dx + ca * dy;
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            double fx = sx - static_cast<double>(x0);
            double fy = sy - static_cast<double>(y0);
            int x1 = x0 + 1;
            int y1 = y0 + 1;
            if (x0 < 0) {
                x0 = 0;
            }
            if (y0 < 0) {
                y0 = 0;
            }
            if (x1 > srcW - 1) {
                x1 = srcW - 1;
            }
            if (y1 > srcH - 1) {
                y1 = srcH - 1;
            }
            // When a coordinate was clamped, both samples are the same
            // source pixel, so the weight is harmless (same as
            // BilinearUpsampleGray).
            double v00 = src[static_cast<size_t>(y0) * static_cast<size_t>(srcW) + static_cast<size_t>(x0)];
            double v10 = src[static_cast<size_t>(y0) * static_cast<size_t>(srcW) + static_cast<size_t>(x1)];
            double v01 = src[static_cast<size_t>(y1) * static_cast<size_t>(srcW) + static_cast<size_t>(x0)];
            double v11 = src[static_cast<size_t>(y1) * static_cast<size_t>(srcW) + static_cast<size_t>(x1)];
            double v = (v00 * (1.0 - fx) + v10 * fx) * (1.0 - fy) +
                       (v01 * (1.0 - fx) + v11 * fx) * fy;
            int rounded = static_cast<int>(std::floor(v + 0.5));
            if (rounded < 0) {
                rounded = 0;
            }
            if (rounded > 255) {
                rounded = 255;
            }
            out[static_cast<size_t>(j) * static_cast<size_t>(dstW) + static_cast<size_t>(i)] =
                static_cast<uint8_t>(rounded);
        }
    }
    dst.swap(out);
    return true;
}

}  // namespace gainmap
