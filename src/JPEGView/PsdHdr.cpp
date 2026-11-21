// PsdHdr.cpp — see PsdHdr.h for the design and the assumptions.
//
// Pure C++ (no MFC/Windows): compiles on Linux as well as in the MSVC
// project (precompiled headers disabled for this file in the vcxproj).
// MSVC constraints observed: no uint64_t (size_t only), no variable
// directly under a case label (braces around case bodies that declare
// one), no GetTickCount64.
//
// zlib: "zlib.h" resolves to the vendored zlib 1.3.2 headers in
// libpng-apng/include on Windows (that dir is on the IncludePath for
// all configs, next to png.h) and to the system zlib.h on Linux.
// inflateInit_ only checks version[0] ('1'), so the system 1.3 lib
// inflates identically; the vendored headers match the linked 1.3.2
// zlib.lib exactly.

#include "PsdHdr.h"
#include "MaxImageDef.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "zlib.h"

namespace PsdHdr {

namespace {

// +Inf clamps to this finite value (SanitizeFloat32). Large enough to
// tone-map to white under any realistic content peak, small enough
// that the tone mapper's double math stays finite.
const float kClampedInfinity = 1.0e6f;

// Overflow-checked multiply: returns false (and leaves *out untouched)
// when a * b would overflow size_t.
bool CheckedMul(size_t a, size_t b, size_t* out) {
    if (a != 0 && b > (std::numeric_limits<size_t>::max)() / a) {
        return false;
    }
    *out = a * b;
    return true;
}

// Read one big-endian uint16 / uint32 from a bounds-checked position.
// The caller guarantees pos + size <= size.
uint16_t ReadBe16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t ReadBe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Write one big-endian uint16 (the counterpart of ReadBe16; the
// 32-bit counterpart was deleted with F1 — the 32-bit ZIP-prediction
// path no longer writes uint32 samples).
void WriteBe16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

// Reinterpret a uint32 bit pattern as a float (no aliasing violation).
float BitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Convert one decoded sample (already in host order) to linear float.
float SampleToLinear16(uint16_t v) {
    return (float)SrgbEotf16(v);
}

float SampleToLinear32(uint32_t bits) {
    return SanitizeFloat32(BitsToFloat(bits));
}

// TEMPORARY DIAGNOSTIC (2026-11-21): last InflateTo zlib return codes.
// REVERT AFTER DIAGNOSIS. Plain file-local static: the decode is
// single-threaded, so no synchronisation is needed. initRet meanings:
//   0    = inflate was never attempted for this DecodeImageData call
//   -100 = InflateTo rejected the call before touching zlib (NULL
//          pointers or a size over the 32-bit uLong limit)
//   other = the actual inflateInit_ return code (Z_OK is 0, so any
//          other value here means inflateInit failed).
struct InflateDiag {
    int initRet;
    int inflateRet;
    unsigned long totalOut;
};
static InflateDiag g_lastInflateDiag = { 0, 0, 0 };

static void SetInflateDiag(int initRet, int inflateRet, unsigned long totalOut) {
    g_lastInflateDiag.initRet = initRet;
    g_lastInflateDiag.inflateRet = inflateRet;
    g_lastInflateDiag.totalOut = totalOut;
}

// Inflate src[0..srcSize) into dst[0..dstSize). Returns false unless
// the stream ends cleanly (Z_STREAM_END) with exactly dstSize bytes
// produced. Only the stream end and the produced count are checked:
// bytes after a complete stream are ignored (inflate never reads
// past srcSize, so trailing garbage cannot affect the output), and
// a short stream fails because inflate returns Z_BUF_ERROR instead
// of Z_STREAM_END. A size mismatch is corrupt input, not a partial
// decode.
bool InflateTo(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    if (src == NULL || dst == NULL) {
        // TEMPORARY DIAGNOSTIC (2026-11-21): rejected before zlib.
        // REVERT AFTER DIAGNOSIS (see SetInflateDiag above).
        SetInflateDiag(-100, 0, 0);
        return false;
    }
    // uLong is zlib's 32-bit length type; a stream larger than that
    // cannot be inflated in one call.
    if (srcSize > (size_t)0xFFFFFFFFUL || dstSize > (size_t)0xFFFFFFFFUL) {
        // TEMPORARY DIAGNOSTIC (2026-11-21): rejected before zlib.
        // REVERT AFTER DIAGNOSIS (see SetInflateDiag above).
        SetInflateDiag(-100, 0, 0);
        return false;
    }
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = (uInt)srcSize;
    strm.next_out = dst;
    strm.avail_out = (uInt)dstSize;
    int initRet = inflateInit(&strm);
    if (initRet != Z_OK) {
        // TEMPORARY DIAGNOSTIC (2026-11-21): record the inflateInit
        // failure. REVERT AFTER DIAGNOSIS (see SetInflateDiag above).
        SetInflateDiag(initRet, 0, 0);
        return false;
    }
    int ret = inflate(&strm, Z_FINISH);
    uLong produced = strm.total_out;
    inflateEnd(&strm);
    // TEMPORARY DIAGNOSTIC (2026-11-21): record the inflate outcome.
    // REVERT AFTER DIAGNOSIS (see SetInflateDiag above).
    SetInflateDiag(Z_OK, ret, (unsigned long)produced);
    return ret == Z_STREAM_END && produced == (uLong)dstSize;
}

// PackBits-decode one row of rowBytes bytes from src[pos..rowEnd) into
// dst[0..rowBytes). A run (literal or repeat) must end at or before
// the row edge — a run crossing into the next row is corrupt (the
// ST-05 rule from the 8-bit wrapper). Consumes at most the row's
// compressed bytes; *consumed returns how many source bytes were used.
// Returns false on truncation, overrun, or a short row.
bool UnpackBitsRow(const uint8_t* src, size_t pos, size_t rowEnd,
                   uint8_t* dst, size_t rowBytes, size_t* consumed) {
    size_t count = 0;
    size_t p = pos;
    while (count < rowBytes) {
        if (p >= rowEnd) {
            return false;
        }
        uint8_t c = src[p++];
        if (c > 128) {
            // Repeat run: (257 - c) copies of the next byte.
            size_t run = (size_t)(257 - (int)c);
            if (count + run > rowBytes) {
                return false;
            }
            if (p >= rowEnd) {
                return false;
            }
            uint8_t value = src[p++];
            std::memset(dst + count, value, run);
            count += run;
        } else if (c < 128) {
            // Literal run: (c + 1) bytes follow.
            size_t run = (size_t)c + 1;
            if (count + run > rowBytes) {
                return false;
            }
            if (p + run > rowEnd) {
                return false;
            }
            std::memcpy(dst + count, src + p, run);
            p += run;
            count += run;
        } else {
            // c == 128: no-op (PackBits NOP).
        }
    }
    *consumed = p - pos;
    return true;
}

}  // namespace

double SrgbEotf16(uint16_t v) {
    double s = (double)v / 65535.0;
    if (s <= 0.04045) {
        return s / 12.92;
    }
    return std::pow((s + 0.055) / 1.055, 2.4);
}

float SanitizeFloat32(float v) {
    if (!(v >= 0.0f)) {
        // NaN fails the >= test too, so NaN and negatives both map to 0.
        return 0.0f;
    }
    if (v == std::numeric_limits<float>::infinity()) {
        return kClampedInfinity;
    }
    return v;
}

uint8_t FloatAlphaToByte(float a) {
    if (!(a >= 0.0f)) {
        return 0;
    }
    if (a > 1.0f) {
        a = 1.0f;
    }
    double v = (double)a * 255.0 + 0.5;
    if (v > 255.0) {
        v = 255.0;
    }
    return (uint8_t)(int)v;
}

uint8_t U16AlphaToByte(uint16_t a) {
    // round(a / 65535 * 255); computed in double, exact for all inputs.
    double v = (double)a * 255.0 / 65535.0 + 0.5;
    if (v > 255.0) {
        v = 255.0;
    }
    return (uint8_t)(int)v;
}

// TEMPORARY DIAGNOSTIC (2026-11-21): returns the last InflateTo call's
// zlib return codes for this DecodeImageData call (zeros when inflate
// was never attempted). REVERT AFTER DIAGNOSIS. Declared in PsdHdr.h.
void LastInflateDiag(int* initRet, int* inflateRet, unsigned long* totalOut) {
    if (initRet != NULL) {
        *initRet = g_lastInflateDiag.initRet;
    }
    if (inflateRet != NULL) {
        *inflateRet = g_lastInflateDiag.inflateRet;
    }
    if (totalOut != NULL) {
        *totalOut = g_lastInflateDiag.totalOut;
    }
}

bool DecodeImageData(const uint8_t* src, size_t srcSize,
                     int width, int height, int channels, int colorMode,
                     int bitDepth, int compression, int version,
                     float* outLinearRgb,
                     uint8_t* outAlphaOrNull, size_t outAlphaStride,
                     const ColorSpace* colorSpaceOrNull) {
    // TEMPORARY DIAGNOSTIC (2026-11-21): clear the record so a stale
    // value from a previous call cannot leak into this one.
    // REVERT AFTER DIAGNOSIS (see SetInflateDiag above).
    SetInflateDiag(0, 0, 0);
    if (src == NULL || outLinearRgb == NULL) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    // PSD/PSB dimensions are at most 300000 per side; anything larger
    // can only come from a malformed header.
    if (width > 300000 || height > 300000) {
        return false;
    }
    if (version != 1 && version != 2) {
        return false;
    }
    if (bitDepth != 16 && bitDepth != 32) {
        return false;
    }
    if (compression < kCompressionRaw || compression > kCompressionZipPrediction) {
        return false;
    }
    // Colour mode and channel count must agree; CMYK/Lab/etc. are out
    // of scope and stay rejected by the wrapper.
    switch (colorMode) {
        case kColorModeGrayscale: {
            if (channels != 1) {
                return false;
            }
            break;
        }
        case kColorModeRgb: {
            if (channels != 3 && channels != 4) {
                return false;
            }
            break;
        }
        default: {
            return false;
        }
    }
    if (outAlphaOrNull != NULL && outAlphaStride < (size_t)width) {
        return false;
    }

    const size_t sampleBytes = (bitDepth == 16) ? (size_t)2 : (size_t)4;
    size_t rowBytes = 0;
    size_t planeBytes = 0;
    size_t totalBytes = 0;
    // P3-D2: no separate pixelCount overflow check: rowBytes is
    // width * sampleBytes (>= width, sampleBytes is 2 or 4), so the
    // totalBytes chain above already overflows first if width*height
    // would. (consumed, the other dead value in this file, stays: it
    // is the dummy sink UnpackBitsRow unconditionally writes to.)
    if (!CheckedMul((size_t)width, sampleBytes, &rowBytes) ||
        !CheckedMul(rowBytes, (size_t)height, &planeBytes) ||
        !CheckedMul(planeBytes, (size_t)channels, &totalBytes)) {
        return false;
    }

    // P2-D2: bound the plane-buffer allocation by the hard byte cap
    // before resizing. totalBytes is overflow-checked above, so this
    // comparison cannot wrap; without it a header claiming e.g.
    // 30000x30000x3x2 (5.4 GB) would attempt the allocation first.
    if (totalBytes > MAX_IMAGE_BYTES) {
        return false;
    }

    // Decompress the planar data into one contiguous buffer:
    // planes in file order (R, G, B [, A] or grey), each plane
    // height rows of rowBytes bytes. Every path below fills exactly
    // totalBytes bytes or fails.
    std::vector<uint8_t> planes;
    try {
        planes.resize(totalBytes);
    } catch (...) {
        return false;
    }

    switch (compression) {
        case kCompressionRaw: {
            if (srcSize < totalBytes) {
                return false;
            }
            std::memcpy(planes.data(), src, totalBytes);
            break;
        }
        case kCompressionRle: {
            // Byte-count table: height * channels entries, 2 bytes
            // (PSD v1) or 4 bytes (PSB v2) each, big-endian. The table
            // must fit inside the image data actually supplied.
            const size_t tableEntry = (version == 2) ? (size_t)4 : (size_t)2;
            size_t tableSize = 0;
            if (!CheckedMul((size_t)height, (size_t)channels, &tableSize) ||
                !CheckedMul(tableSize, tableEntry, &tableSize)) {
                return false;
            }
            if (srcSize < tableSize) {
                return false;
            }
            size_t pos = tableSize;
            for (int ch = 0; ch < channels; ch++) {
                for (int y = 0; y < height; y++) {
                    size_t tableIndex = ((size_t)ch * (size_t)height + (size_t)y) * tableEntry;
                    size_t rowLen = 0;
                    if (version == 2) {
                        rowLen = (size_t)ReadBe32(src + tableIndex);
                    } else {
                        rowLen = (size_t)ReadBe16(src + tableIndex);
                    }
                    // The row's compressed bytes must lie inside src.
                    if (rowLen > srcSize || pos > srcSize - rowLen) {
                        return false;
                    }
                    size_t rowEnd = pos + rowLen;
                    uint8_t* dstRow = planes.data() + ((size_t)ch * (size_t)height + (size_t)y) * rowBytes;
                    size_t consumed = 0;
                    if (!UnpackBitsRow(src, pos, rowEnd, dstRow, rowBytes, &consumed)) {
                        return false;
                    }
                    // The row may carry trailing pad bytes (the spec
                    // pads odd-sized layer rows); advance past the
                    // whole table entry either way. Consuming more
                    // than the entry is already rejected inside
                    // UnpackBitsRow by the rowEnd bound.
                    pos = rowEnd;
                }
            }
            break;
        }
        case kCompressionZip:
        case kCompressionZipPrediction: {
            if (!InflateTo(src, srcSize, planes.data(), totalBytes)) {
                return false;
            }
            if (compression == kCompressionZipPrediction) {
                // Reverse the per-row delta, per plane. The first
                // sample of each row is stored as-is (delta from 0).
                // 16-bit: in place, on the big-endian bytes — each
                // sample is read, added to the running total
                // (wraparound uint16 arithmetic), and written back
                // before the next sample is read, so no temp row is
                // needed and nothing here can throw.
                // 32-bit: the prediction runs over BYTE PLANES, not
                // over interleaved float bit patterns. Each row's
                // 4-byte samples are stored deinterleaved (all byte
                // 0s, then byte 1s, byte 2s, byte 3s) and delta-encoded
                // as bytes with ONE accumulator running across the
                // whole 4*width-byte row (psd-tools decode_prediction,
                // OpenImageIO, and the PhotoshopAPI docs all agree;
                // the Adobe spec lists compression value 3 but defines
                // no prediction algorithm, so it cannot be cited
                // either way). Transposition needs a scratch row: one
                // buffer reused for every row, allocated once below
                // (guarded like planes above, so the no-throw
                // contract in PsdHdr.h holds). The old code did uint32
                // cumsum on the interleaved bit patterns, a different,
                // mutually unreadable scheme (F1).
                std::vector<uint8_t> scratch;
                if (bitDepth == 32) {
                    try {
                        scratch.resize(rowBytes);
                    } catch (...) {
                        return false;
                    }
                }
                for (int ch = 0; ch < channels; ch++) {
                    uint8_t* plane = planes.data() + (size_t)ch * planeBytes;
                    for (int y = 0; y < height; y++) {
                        uint8_t* row = plane + (size_t)y * rowBytes;
                        if (bitDepth == 16) {
                            uint32_t acc = 0;
                            for (int x = 0; x < width; x++) {
                                acc = (acc + (uint32_t)ReadBe16(row + (size_t)x * 2)) & 0xFFFFu;
                                WriteBe16(row + (size_t)x * 2, (uint16_t)acc);
                            }
                        } else {
                            // Byte-plane decode: copy the row to the
                            // scratch buffer, run the uint8 cumsum
                            // over the plane-ordered bytes, then
                            // re-interleave to big-endian float bit
                            // patterns. scratch[b*width + x] is byte b
                            // of sample x.
                            std::memcpy(scratch.data(), row, rowBytes);
                            uint32_t acc = 0;
                            for (size_t i = 0; i < rowBytes; i++) {
                                acc = (acc + scratch[i]) & 0xFFu;
                                scratch[i] = (uint8_t)acc;
                            }
                            for (int x = 0; x < width; x++) {
                                for (int b = 0; b < 4; b++) {
                                    row[(size_t)x * 4 + (size_t)b] =
                                        scratch[(size_t)b * (size_t)width + (size_t)x];
                                }
                            }
                        }
                    }
                }
            }
            break;
        }
        default: {
            return false;
        }
    }

    // Convert the planar samples to interleaved linear RGB. Plane 0
    // is R (or grey), plane 1 G, plane 2 B, plane 3 alpha.
    // When colorSpaceOrNull is NULL the loop below is exactly the
    // pre-H-02 code (same float operations in the same order), so the
    // sRGB path is byte-identical. When it is non-NULL, 16-bit samples
    // are linearized with the file's TRC tables and RGB (both depths)
    // is converted to linear sRGB with the supplied matrix; grayscale
    // gets the TRC only (grey is grey, no matrix).
    const bool isGray = (colorMode == kColorModeGrayscale);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t pixel = (size_t)y * (size_t)width + (size_t)x;
            float lin[3] = { 0.0f, 0.0f, 0.0f };
            if (bitDepth == 16) {
                for (int c = 0; c < (isGray ? 1 : 3); c++) {
                    const uint8_t* p = planes.data() + ((size_t)c * (size_t)height + (size_t)y) * rowBytes + (size_t)x * 2;
                    const uint16_t v = ReadBe16(p);
                    if (colorSpaceOrNull != NULL && colorSpaceOrNull->trc[c] != NULL) {
                        lin[c] = colorSpaceOrNull->trc[c][v];
                    } else {
                        lin[c] = SampleToLinear16(v);
                    }
                }
            } else {
                for (int c = 0; c < (isGray ? 1 : 3); c++) {
                    const uint8_t* p = planes.data() + ((size_t)c * (size_t)height + (size_t)y) * rowBytes + (size_t)x * 4;
                    lin[c] = SampleToLinear32(ReadBe32(p));
                }
            }
            if (isGray) {
                lin[1] = lin[0];
                lin[2] = lin[0];
            } else if (colorSpaceOrNull != NULL) {
                // Row-major matrix: out[i] = sum_j m[i*3+j] * lin[j].
                const float* m = colorSpaceOrNull->matrix;
                const float r = m[0] * lin[0] + m[1] * lin[1] + m[2] * lin[2];
                const float g = m[3] * lin[0] + m[4] * lin[1] + m[5] * lin[2];
                const float b = m[6] * lin[0] + m[7] * lin[1] + m[8] * lin[2];
                lin[0] = r;
                lin[1] = g;
                lin[2] = b;
            }
            outLinearRgb[pixel * 3 + 0] = lin[0];
            outLinearRgb[pixel * 3 + 1] = lin[1];
            outLinearRgb[pixel * 3 + 2] = lin[2];
            if (channels == 4 && outAlphaOrNull != NULL) {
                uint8_t a = 255;
                const uint8_t* p = planes.data() + ((size_t)3 * (size_t)height + (size_t)y) * rowBytes +
                    (size_t)x * sampleBytes;
                if (bitDepth == 16) {
                    a = U16AlphaToByte(ReadBe16(p));
                } else {
                    a = FloatAlphaToByte(BitsToFloat(ReadBe32(p)));
                }
                outAlphaOrNull[(size_t)y * outAlphaStride + (size_t)x] = a;
            }
        }
    }
    return true;
}

}  // namespace PsdHdr
