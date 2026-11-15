// GainMap.h — shared gain-map (ISO 21496-1 / UltraHDR) helper for JPEGView.
//
// Pure C++, no MFC / no Windows headers, so the same code can be
// unit-tested on Linux. This is Phase 1 of the gain-map support
// (Option D): it parses the metadata and reconstructs scene-linear HDR
// float RGB from an 8-bit sRGB base image plus a single-channel 8-bit
// gain map. It does NOT decode any image format and does NOT tone-map:
// callers decode the base and gain map themselves (the gain map MUST
// be decoded to grayscale — see below) and tone-map the linear output
// with the existing HdrToneMap::ConvertLinearImage.
//
// Metadata sources, in the order callers should try them:
//   1. ISO 21496-1 binary metadata: an APP2 segment of the GAIN-MAP
//      image (JPEG UltraHDR) or the equivalent box in HEIF, starting
//      with the URN "urn:iso:std:iso:ts:21496:-1" + NUL. This is the
//      authoritative source when present.
//   2. XMP (hdrgm namespace, UltraHDR/Adobe): a fallback for files
//      that carry no ISO binary metadata.
//   3. Apple HDRGainMap XMP (HDRGainMapHeadroom): a fallback for
//      iPhone gain-map files. *** UNVERIFIED *** — no Apple sample was
//      on hand when this was written; the mapping below (headroom ->
//      gainMapMax, gamma from HDRGainMapGamma, offsets 0) is a best
//      guess from the field names and NEEDS a real iPhone file to
//      confirm. Do not treat Apple fallback output as verified until
//      that check is done. ParseXmpMetadata sets
//      Metadata::appleHeadroomFallback so callers/tests can tell the
//      two XMP paths apart.
//
// Reconstruction formula (ISO 21496-1, weightFactor = 1.0 for full HDR):
//   logRecovery = gainPixel^(1/gamma)          (gainPixel = byte/255, 0..1)
//   logBoost    = gainMapMin*(1-logRecovery) + gainMapMax*logRecovery
//   gain        = 2^(logBoost * weightFactor)
//   HDR_linear  = (SDR_linear + baseOffset) * gain - alternateOffset
// where SDR_linear is the sRGB EOTF inverse of the 8-bit sRGB base
// pixel. All of gainMapMin/gainMapMax/headroom are in log2 space.
// Verified against the Python reference (~/workspace/gainmap_reference.py)
// on kennylevinsen test_uhdr_hlg.jpg / test_uhdr_pq.jpg.
//
// Gain-map decoding warning: the gain-map JPEG is a 3-component YCbCr
// JPEG whose Cb/Cr are NOT 128, so an RGB decode gives R!=G!=B (and a
// max R of 255, which is wrong). The gain map MUST be read as
// grayscale (the Y/luma channel). ApplyGainMap takes a single-channel
// 8-bit gain-map buffer; the caller is responsible for decoding to
// grayscale. The gain map may be a different (usually lower)
// resolution than the base; it is upsampled bilinearly to base size.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gainmap {

// Gain-map metadata. `valid` is false until a parser has filled the
// struct in; ApplyGainMap refuses to run with valid == false.
struct Metadata {
    double baseHdrHeadroom = 0.0;       // log2 headroom of the base (SDR) image
    double alternateHdrHeadroom = 0.0;  // log2 headroom of the alternate (HDR) image
    double gainMapMin = 0.0;            // log2 gain at gain-map value 0
    double gainMapMax = 0.0;            // log2 gain at gain-map value 1
    double gamma = 1.0;                 // gain-map gamma (> 0)
    double baseOffset = 0.0;            // added to SDR linear before the gain
    double alternateOffset = 0.0;       // subtracted from HDR linear after the gain
    bool multiChannel = false;          // ISO flags bit 7: 3 channel sets present
    bool useBaseColorSpace = false;     // ISO flags bit 6
    bool appleHeadroomFallback = false; // true when parsed from Apple HDRGainMap XMP
    bool valid = false;                 // set by the parsers on success
};

// Parse ISO 21496-1 binary metadata. `data` points at the APP2 payload
// start (the URN "urn:iso:std:iso:ts:21496:-1\0"); `size` is the payload
// byte count. Layout after the 28-byte URN+NUL: min_ver u16 BE,
// writer_ver u16 BE, flags u8, then 7 rational pairs (num i32 BE +
// den u32 BE): [0] baseHdrHeadroom, [1] alternateHdrHeadroom,
// [2] gainMapMin, [3] gainMapMax, [4] gamma, [5] baseOffset,
// [6] alternateOffset. Returns true on success (sets out.valid=true).
// Rejects (returns false, leaves out.valid=false): null/short payload,
// URN mismatch, size < 28+5+7*8 = 89 bytes (the full 7-pair layout is
// required — every pair is needed for reconstruction), a zero
// denominator, gamma <= 0, or any non-finite value. When flags bit 7
// (multi-channel) is set only the FIRST channel set is parsed and
// multiChannel is set to true; extra channel sets are ignored.
bool ParseIso21496Metadata(const uint8_t* data, size_t size, Metadata& out);

// Parse UltraHDR/Adobe XMP (hdrgm namespace) from an XMP packet string.
// Fields (each may be a single value or an rdf:Seq of 3 rationals
// "num/den" — the FIRST is taken): hdrgm:GainMapMin, hdrgm:GainMapMax,
// hdrgm:Gamma, hdrgm:OffsetSDR, hdrgm:OffsetHDR, hdrgm:HDRCapacityMin,
// hdrgm:HDRCapacityMax. Mapped as: gainMapMin=GainMapMin,
// gainMapMax=GainMapMax, gamma=Gamma, baseOffset=OffsetSDR,
// alternateOffset=OffsetHDR, alternateHdrHeadroom=HDRCapacityMax,
// baseHdrHeadroom=HDRCapacityMin. Returns true only if GainMapMin,
// GainMapMax AND Gamma were all found (the essential fields).
// Uses a simple robust string search (find the field name, then parse
// the first number or "num/den" rational after it) — no XML parser.
// Apple fallback: if NO hdrgm field is present at all but
// HDRGainMapHeadroom is, sets gainMapMax=headroom (log2), gainMapMin=0,
// gamma=HDRGainMapGamma if present else 1.0, offsets 0, and
// appleHeadroomFallback=true, and returns true. The Apple mapping is
// UNVERIFIED (see the header note above) and needs a real iPhone file
// to confirm. Returns false (out.valid=false) when neither scheme
// yields the essential fields, or when a found gamma is <= 0.
bool ParseXmpMetadata(const std::string& xmp, Metadata& out);

// Find the gain-map JPEG embedded in an UltraHDR JPEG (MPF APP2).
// Scans the JPEG markers for the MPF APP2 ("MPF\0" + TIFF header),
// reads the TIFF IFD (0xB001 NumberOfImages, 0xB002 MP entry list)
// and returns the SECOND image's absolute file offset and size — the
// gain map (image 0 is the primary). MPF data_offset values are
// relative to the TIFF header start inside the MPF segment. Returns
// false (out params left 0) when there is no MPF segment, fewer than
// 2 images, the TIFF/IFD/entry data is malformed or out of bounds,
// offset+size runs past the end of the buffer, or the gain-map bytes
// do not start with the JPEG SOI (FFD8). A truncated MPF segment
// (its length field running past the buffer) also returns false.
bool FindGainMapJpeg(const uint8_t* jpeg, size_t size, size_t& gainOffset, size_t& gainSize);

// Find the ISO 21496-1 binary metadata APP2 inside a JPEG —
// normally the GAIN-MAP JPEG (callers get it via FindGainMapJpeg).
// Scans the JPEG markers for the APP2 whose payload starts with the
// URN "urn:iso:std:iso:ts:21496:-1\0" and returns a pointer + size
// into the original buffer (no copy). Returns false when no such
// APP2 exists or the JPEG markers are malformed/truncated. The
// finder is URN-based only: a version-only ISO segment (as in some
// primary JPEGs, 32-byte payload) also matches, and
// ParseIso21496Metadata rejects it for being short — callers must
// run the parser and fall back (e.g. to XMP) when it fails.
bool FindIso21496Payload(const uint8_t* jpeg, size_t size,
                         const uint8_t*& payload, size_t& payloadSize);

// Reconstruct scene-linear HDR float RGB from an 8-bit sRGB base image
// and a single-channel 8-bit gain map, using the ISO 21496-1 formula
// above. `baseSrgb` is width*height*3 bytes, 8-bit sRGB, RGB order.
// `gainMap` is gainW*gainH bytes, 8-bit grayscale; it is upsampled
// bilinearly to base size when gainW/gainH differ from width/height.
// `meta` must be valid (see Metadata). `weightFactor` is clamped to
// [0,1] (0 = SDR base unchanged, 1 = full HDR). `outLinear` is
// width*height*3 floats, RGB order, scene-linear. Double math is used
// internally; the output is float. Returns false on bad input (null
// pointers, non-positive dimensions, !meta.valid, invalid gamma /
// non-finite metadata values, NaN weightFactor). The gain pixel is
// clamped to [0,1] before pow.
bool ApplyGainMap(const uint8_t* baseSrgb, int width, int height,
                  const uint8_t* gainMap, int gainW, int gainH,
                  const Metadata& meta, double weightFactor, float* outLinear);

// Single-channel bilinear upsample. If src and dst are the same size
// the buffer is copied. Source coordinates use centre alignment:
// sx = (dx + 0.5) * srcW / dstW - 0.5 (same for y); edge pixels are
// handled by clamping coordinates, so the dst corners equal the src
// corners. No-op on null pointers or non-positive dimensions.
// src and dst must not overlap (except when identical, which the
// same-size copy handles).
void BilinearUpsampleGray(const uint8_t* src, int srcW, int srcH,
                          uint8_t* dst, int dstW, int dstH);

// sRGB 8-bit code -> linear float (piecewise sRGB EOTF inverse:
// c <= 0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4, c = v/255).
// Exposed for tests; ApplyGainMap uses the same formula in double.
float SrgbToLinear(uint8_t v);

}  // namespace gainmap
