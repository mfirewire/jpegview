// PsdHdr.h — 16/32-bit PSD image-data decode core (PSD HDR support).
//
// Pure C++ (no MFC / no Windows headers): it compiles on Linux as well
// as in the MSVC project, so the math can be verified without a Windows
// run. It intentionally does NOT include stdafx.h (precompiled headers
// are disabled for this file in the vcxproj); it only needs the C++
// standard library plus zlib.h for the ZIP paths.
//
// What this does, and what it does not:
// --------------------------------------
// This decodes the *image data section* of a PSD/PSB file — the bytes
// PSDWrapper.cpp reads into pBuffer, i.e. everything after the 2-byte
// compression field — for 16-bit and 32-bit depth, and produces an
// interleaved linear-float RGB buffer (3 floats per pixel, sRGB
// primaries, row-major, RGB order), which is exactly the input layout
// HdrToneMap::ConvertLinearImage expects (linear 1.0 = SDR white).
// The caller (PSDWrapper) still owns the file header, the colour-mode /
// resource / layer sections, the compression field, the CJPEGImage
// assembly, and the tone-mapping call; this file only turns compressed
// planar channel bytes into linear floats.
//
// Assumptions, all documented so a wrong one can be caught:
//   * 16-bit PSD samples are linearized with the file's own tone
//     curve when the caller supplies a ColorSpace (built from the
//     file's ICC profile by the wrapper — see PsdIcc.h), and with the
//     sRGB EOTF (SrgbEotf16) when it does not (NULL ColorSpace, or a
//     NULL per-channel table). A supplied ColorSpace also carries a
//     3x3 matrix converting linear source RGB to linear sRGB, so a
//     16/32-bit Adobe RGB or ProPhoto file decodes with correct
//     colours instead of being linearized as if it were sRGB (H-02).
//     The matrix is built from the profile's D50 (PCS) colorants
//     against the D50 sRGB matrix, which folds the Bradford D50->D65
//     adaptation into the fixed matrix — see the D50/D65 note in
//     PsdIcc.h. An untagged file (no ICC profile) is treated as sRGB,
//     which is what Photoshop assumes for this purpose.
//   * 32-bit PSD is scene-linear already: Photoshop 32-bit mode stores
//     linear float samples, so they are used as-is (after sanitize);
//     a supplied ColorSpace still applies its primaries matrix to
//     32-bit RGB (the samples are already linear, so no TRC step).
//   * Planar channel order is R, G, B (, A) — the same order the 8-bit
//     wrapper reads (it reverses the planes into BGR itself; this core
//     outputs RGB, which is what HdrToneMap wants).
//   * Colour modes: Grayscale (1) and RGB (3) only. CMYK (4) and Lab
//     (9) 16/32-bit are rejected here; the wrapper keeps rejecting
//     those as today. Bitmap (0), Indexed (2), Multichannel (7) and
//     Duotone (8) are rejected too.
//   * ZIP with prediction (3): 16-bit rows store per-sample uint16
//     deltas (each sample minus the previous sample in the same row,
//     same channel, wraparound; first sample as-is, accumulator reset
//     per row). 32-bit rows store BYTE PLANES: each row's 4-byte samples
//     are deinterleaved (all byte 0s, then byte 1s, byte 2s, byte 3s) and
//     delta-encoded as bytes with one accumulator running across the
//     whole 4*width-byte row. The byte-plane scheme is psd-tools'
//     decode_prediction, OpenImageIO, and the PhotoshopAPI docs (all
//     three agree); the Adobe spec lists compression value 3 ("ZIP with
//     prediction") but defines no prediction algorithm, so it cannot
//     corroborate either scheme. (F1: an earlier version did uint32
//     cumsum on interleaved float bit patterns — a different, mutually
//     unreadable scheme.)
//   * RLE (1) is PackBits at the BYTE level, per row per channel,
//     exactly as the 8-bit wrapper does: a row holds width *
//     sampleBytes bytes, and a run must not cross a row boundary
//     (the ST-05 rule from the wrapper).
//
// Bounds discipline: every read is checked against the supplied buffer
// size; size_t arithmetic is overflow-checked before any allocation;
// any overrun, corrupt table, or impossible dimension returns false.
// No exception escapes this file (bad_alloc from the temp buffers is
// caught and reported as false).

#pragma once

#include <cstddef>
#include <cstdint>

namespace PsdHdr {

// PSD colour-mode codes (Photoshop File Formats Specification).
// Only kGrayscale and kRgb are accepted by DecodeImageData.
enum ColorMode : int {
    kColorModeBitmap = 0,
    kColorModeGrayscale = 1,
    kColorModeIndexed = 2,
    kColorModeRgb = 3,
    kColorModeCmyk = 4,
    kColorModeMultichannel = 7,
    kColorModeDuotone = 8,
    kColorModeLab = 9,
};

// PSD image-data compression codes (the 2-byte field before the data).
enum Compression : int {
    kCompressionRaw = 0,
    kCompressionRle = 1,
    kCompressionZip = 2,
    kCompressionZipPrediction = 3,
};

// sRGB EOTF for one 16-bit sample: v/65535, then the piecewise sRGB
// curve (linear segment at/below 0.04045, power 2.4 above). Pinned by
// the unit test against Python-computed values.
double SrgbEotf16(uint16_t v);

// Sanitize one 32-bit float sample: negative values and NaN map to 0
// (the same rule HdrToneMap uses); +Inf clamps to a large finite value
// (1.0e6f) so Inf/NaN can never reach the tone mapper. Finite values
// pass through unchanged.
float SanitizeFloat32(float v);

// Convert one alpha sample to an 8-bit byte (clamped to [0,1] first
// for float alpha, then rounded): round(a * 255) for 32-bit float,
// round(a / 65535 * 255) for 16-bit.
uint8_t FloatAlphaToByte(float a);
uint8_t U16AlphaToByte(uint16_t a);

// Colour space of the file's samples, for the HDR decode.
// A NULL ColorSpace pointer (or one with all trc[] NULL and an identity
// matrix) means sRGB: 16-bit samples are linearized with the sRGB EOTF
// (SrgbEotf16) and no primaries conversion is applied — the pre-H-02
// behaviour, byte-identical.
struct ColorSpace {
    // Per-channel linearization tables: trc[c] maps a 16-bit encoded
    // sample (0..65535) to linear light (0..1). One table per channel
    // (RGB); for grayscale only trc[0] is used. NULL means "use the
    // sRGB EOTF for this channel". The caller owns the table storage;
    // tables must outlive the DecodeImageData call. Each table is
    // 65536 floats.
    const float* trc[3];
    // 3x3 matrix (row-major) converting linear source RGB to linear
    // sRGB. Identity when the source primaries are sRGB. Only used
    // for RGB (not grayscale).
    float matrix[9];
};

// Decode the PSD image-data section.
//
//   src / srcSize : the image-data bytes after the 2-byte compression
//     field (exactly what PSDWrapper.cpp reads into pBuffer). For RLE
//     this starts with the byte-count table (height * channels entries,
//     2 bytes each for PSD v1, 4 bytes each for PSB v2, big-endian);
//     for ZIP it is the zlib stream for all channels concatenated.
//   width / height : image dimensions in pixels (must be > 0).
//   channels : number of planes present in the data (1 = grayscale,
//     3 = RGB, 4 = RGBA; must agree with colorMode, see below).
//   colorMode : PSD colour-mode code (kColorModeGrayscale or
//     kColorModeRgb; CMYK/Lab/etc. return false).
//   bitDepth : 16 or 32 (8 returns false — the 8-bit wrapper owns it).
//   compression : kCompressionRaw / kCompressionRle / kCompressionZip /
//     kCompressionZipPrediction.
//   version : 1 (PSD) or 2 (PSB); only affects the RLE table entry size.
//   outLinearRgb : caller-allocated buffer for width * height * 3
//     floats, filled with interleaved linear RGB (sRGB primaries).
//     Grayscale replicates the single channel to R = G = B.
//   outAlphaOrNull : optional caller-allocated alpha buffer (one byte
//     per pixel, row stride outAlphaStride); NULL when the caller does
//     not want alpha (the alpha plane is then decoded and discarded).
//     Only written when channels == 4 (RGBA).
//   outAlphaStride : byte stride between rows of outAlphaOrNull;
//     ignored when outAlphaOrNull is NULL, must be >= width otherwise.
//   colorSpaceOrNull : the file's colour space (see ColorSpace above),
//     built from the ICC profile by the wrapper; NULL means sRGB
//     (16-bit samples linearized with the sRGB EOTF, no primaries
//     conversion — the pre-H-02 behaviour, byte-identical). When
//     non-NULL, 16-bit samples are linearized with the supplied TRC
//     tables and RGB is converted to linear sRGB with the supplied
//     matrix; 32-bit samples (already linear) get the matrix only.
//
// Returns true on success, false on any unsupported or corrupt input
// (bad dimensions, unsupported mode/depth/compression, truncated or
// overrunning data, a PackBits run crossing a row boundary, a zlib
// stream that does not inflate to exactly the expected size). On
// false, outLinearRgb / outAlphaOrNull may be partially written.
bool DecodeImageData(const uint8_t* src, size_t srcSize,
                     int width, int height, int channels, int colorMode,
                     int bitDepth, int compression, int version,
                     float* outLinearRgb,
                     uint8_t* outAlphaOrNull, size_t outAlphaStride,
                     const ColorSpace* colorSpaceOrNull);

}  // namespace PsdHdr
