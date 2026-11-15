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

// Inflate src[0..srcSize) into dst[0..dstSize). Returns false unless
// the stream ends cleanly (Z_STREAM_END) with exactly dstSize bytes
// produced — a short stream, trailing garbage, or a size mismatch is
// corrupt input, not a partial decode.
bool InflateTo(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    if (src == NULL || dst == NULL) {
        return false;
    }
    // uLong is zlib's 32-bit length type; a stream larger than that
    // cannot be inflated in one call.
    if (srcSize > (size_t)0xFFFFFFFFUL || dstSize > (size_t)0xFFFFFFFFUL) {
        return false;
    }
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = (uInt)srcSize;
    strm.next_out = dst;
    strm.avail_out = (uInt)dstSize;
    if (inflateInit(&strm) != Z_OK) {
        return false;
    }
    int ret = inflate(&strm, Z_FINISH);
    uLong produced = strm.total_out;
    inflateEnd(&strm);
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

void ReverseDelta16(uint16_t* samples, int count) {
    if (samples == NULL || count <= 0) {
        return;
    }
    // Wraparound uint16 arithmetic: the delta encoding is defined on
    // the stored bit patterns, so the sum wraps mod 65536.
    uint32_t acc = 0;
    for (int i = 0; i < count; i++) {
        acc = (acc + (uint32_t)samples[i]) & 0xFFFFu;
        samples[i] = (uint16_t)acc;
    }
}

void ReverseDelta32(uint32_t* samples, int count) {
    if (samples == NULL || count <= 0) {
        return;
    }
    // Wraparound uint32 arithmetic on the float bit patterns.
    uint32_t acc = 0;
    for (int i = 0; i < count; i++) {
        acc = acc + samples[i];
        samples[i] = acc;
    }
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

bool DecodeImageData(const uint8_t* src, size_t srcSize,
                     int width, int height, int channels, int colorMode,
                     int bitDepth, int compression, int version,
                     float* outLinearRgb,
                     uint8_t* outAlphaOrNull, size_t outAlphaStride) {
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
    size_t pixelCount = 0;
    if (!CheckedMul((size_t)width, sampleBytes, &rowBytes) ||
        !CheckedMul(rowBytes, (size_t)height, &planeBytes) ||
        !CheckedMul(planeBytes, (size_t)channels, &totalBytes) ||
        !CheckedMul((size_t)width, (size_t)height, &pixelCount)) {
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
                // Work on host-order values in a temp row: the
                // inflated bytes are big-endian, and the delta is
                // defined on the sample values, not the byte stream.
                for (int ch = 0; ch < channels; ch++) {
                    uint8_t* plane = planes.data() + (size_t)ch * planeBytes;
                    for (int y = 0; y < height; y++) {
                        uint8_t* row = plane + (size_t)y * rowBytes;
                        if (bitDepth == 16) {
                            std::vector<uint16_t> samples((size_t)width);
                            for (int x = 0; x < width; x++) {
                                samples[(size_t)x] = ReadBe16(row + (size_t)x * 2);
                            }
                            ReverseDelta16(samples.data(), width);
                            for (int x = 0; x < width; x++) {
                                uint16_t v = samples[(size_t)x];
                                row[(size_t)x * 2 + 0] = (uint8_t)(v >> 8);
                                row[(size_t)x * 2 + 1] = (uint8_t)(v & 0xFF);
                            }
                        } else {
                            std::vector<uint32_t> samples((size_t)width);
                            for (int x = 0; x < width; x++) {
                                samples[(size_t)x] = ReadBe32(row + (size_t)x * 4);
                            }
                            ReverseDelta32(samples.data(), width);
                            for (int x = 0; x < width; x++) {
                                uint32_t v = samples[(size_t)x];
                                row[(size_t)x * 4 + 0] = (uint8_t)(v >> 24);
                                row[(size_t)x * 4 + 1] = (uint8_t)((v >> 16) & 0xFF);
                                row[(size_t)x * 4 + 2] = (uint8_t)((v >> 8) & 0xFF);
                                row[(size_t)x * 4 + 3] = (uint8_t)(v & 0xFF);
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
    const bool isGray = (colorMode == kColorModeGrayscale);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t pixel = (size_t)y * (size_t)width + (size_t)x;
            float lin[3] = { 0.0f, 0.0f, 0.0f };
            if (bitDepth == 16) {
                for (int c = 0; c < (isGray ? 1 : 3); c++) {
                    const uint8_t* p = planes.data() + ((size_t)c * (size_t)height + (size_t)y) * rowBytes + (size_t)x * 2;
                    lin[c] = SampleToLinear16(ReadBe16(p));
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
