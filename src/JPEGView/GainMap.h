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
//      iPhone gain-map files. Both the form and the value mapping are
//      now VERIFIED against a real file (P2-A2 settled 2026-11-21):
//      an iPhone 15 Pro HEIC (PhotoPrism sample) carries
//      HDRGainMapHeadroom 3.847906, and the same file's MakerNote
//      (0x21/0x30) independently yields 3.847921 — agreement to
//      0.0004% — so the field is a LINEAR RATIO (brightest white /
//      SDR white), not log2 stops: gainMapMax = log2(ratio),
//      gamma from HDRGainMapGamma, offsets 0. ParseXmpMetadata sets
//      Metadata::appleHeadroomFallback so callers/tests can tell the
//      two XMP paths apart. When the XMP has the version but no
//      headroom, there is no gain map: the file shows SDR. (The
//      Batch 7 derive-from-pixels fallback was removed by user
//      decision — without a scale the gain map is uninterpretable.)
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
#include <vector>

#include "HdrToneMap.h"  // Transfer/Primaries for ApplyGainMapToBgra16 (NB-1)

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

// Parse ISO 21496-1 binary metadata WITHOUT the URN prefix: the
// payload as stored in a JXL jhgm box (see FindJxlGainMapBox /
// ParseJxlGainMapBundle below), which starts directly at min_ver.
// Same layout and validation as ParseIso21496Metadata after its
// 28-byte URN+NUL: min_ver u16 BE, writer_ver u16 BE, flags u8, then
// 7 rational pairs (num i32 BE + den u32 BE). Returns true on success
// (sets out.valid=true). Rejects (returns false, leaves
// out.valid=false): null/short payload, size < 5+7*8 = 61 bytes, a
// zero denominator, gamma <= 0, or any non-finite value. When flags
// bit 7 (multi-channel) is set only the FIRST channel set is parsed
// and multiChannel is set to true; extra channel sets are ignored.
// (R1 option A: this shares the parse core with
// ParseIso21496Metadata; the URN check stays in that function.)
bool ParseIso21496MetadataNoUrn(const uint8_t* data, size_t size, Metadata& out);

// Find the jhgm (gain-map) box in a JXL file buffer (ISO/IEC 18181-2
// container). Scans the BMFF boxes and returns the FIRST jhgm box's
// payload pointer + size into the original buffer (no copy). Returns
// false when there is no jhgm box, the buffer is a bare codestream
// (starts with the JXL codestream signature FF 0A, no boxes), or the
// box walk hits malformed data. Walk rules: per box, size u32 BE +
// 4-byte type; size == 1 means a u64 largesize follows the type
// (header 16 bytes); size == 0 means the box extends to the end of
// the buffer; a size of 2..7 (or a largesize < 16) is malformed;
// every header and body must stay inside the buffer. A malformed box
// is treated as "no jhgm" (the caller falls back to SDR), never as an
// out-of-bounds read.
bool FindJxlGainMapBox(const uint8_t* data, size_t size,
                       const uint8_t*& payload, size_t& payloadSize);

// Parse a jhgm box payload (JxlGainMapBundle layout, byte-for-byte
// compatible with libjxl's JxlGainMapReadBundle — the vendored
// Windows binaries do not export that function, so this parses the
// layout in-tree). Order: version u8 (must be 0), metadata length
// u16 BE, metadata, color-encoding length u8, color encoding,
// alt-ICC length u32 BE, alt ICC, then the gain-map codestream (the
// rest of the buffer; there is no separate codestream length field).
// The color encoding is skipped by length only, never parsed (it is
// only needed to apply the gain in the alternate color space, which
// this implementation does not do — the gain is applied in the base
// sRGB space, as for AVIF). Returns pointers + sizes into the
// original payload (no copy). Returns false (out params left null/0)
// when the version is not 0, any length runs past the payload, or
// the codestream is empty.
bool ParseJxlGainMapBundle(const uint8_t* payload, size_t payloadSize,
                           const uint8_t*& metadata, size_t& metadataSize,
                           const uint8_t*& gainCode, size_t& gainCodeSize);

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
// Apple fallback: if NO hdrgm field parses (see GainMap.cpp) but
// HDRGainMapHeadroom is, sets gainMapMax=log2(headroom),
// gainMapMin=0, gamma=HDRGainMapGamma if present else 1.0, offsets 0,
// and appleHeadroomFallback=true, and returns true. HDRGainMapHeadroom
// is a LINEAR RATIO (verified, see the header note above), so the
// log2 conversion is load-bearing: the raw value must NOT be used as
// stops. The ratio must be finite and in [1.0, 65536.0] (0 to 16
// stops): below 1.0 is physically meaningless (HDR dimmer than SDR),
// above 65536 is absurd (the ApplyGainMap clamp bound) — both are
// rejected. A ratio of exactly 1.0 is valid with gainMapMax 0.0
// (identity: no gain). Returns false (out.valid=false) when neither
// scheme yields the essential fields, or when a found gamma is <= 0.
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

// Find the XMP packet in a JPEG's APP1 segment (identifier
// "http://ns.adobe.com/xap/1.0/\0"). Returns a pointer into `jpeg`
// (no copy) and the packet size, or false when there is no XMP APP1
// or the markers are malformed/truncated. The packet itself may
// contain trailing padding; the XMP parser only searches it. Stops
// at SOS (scan data follows): it can only find XMP in the header of
// the single JPEG buffer it is given, so a multi-image (MPF) file
// needs one call per image — Apple gain-map JPEGs carry their
// HDRGainMapHeadroom XMP in the AUX (gain) image, not the primary
// (F8). `sizebytes` is int (the original wrapper helper's type);
// callers must pass a value that fits in int.
bool FindXmpPacket(const unsigned char* jpeg, int sizebytes,
                   const unsigned char*& xmp, size_t& xmpSize);

// Follow-up (see docs/HDR-HEIC-exploration-2026-11-21.md §5): Apple's
// own apply formula is linear in the gain — hdr = sdr*(1+(headroom-1)*g)
// — while ApplyGainMap is exponential, gain = 2^(min+(max-min)*g^(1/gamma)).
// The endpoints agree under gainMapMax = log2(headroom) (g=0 -> 1x, g=1 ->
// headroom), but the mid-tones differ. No separate linear apply path is
// implemented (no strong evidence it matches what Photos shows).
// Signed gain-map weight for a display headroom H (log2 stops).
//   f = clamp((H - base) / (alt - base), 0, 1); w = sign(alt - base) * f
// where base = meta.baseHdrHeadroom, alt = meta.alternateHdrHeadroom.
// H is a LOGICAL display headroom (JPEGView has no HDR display): the
// caller passes 0 for the SDR view and max(base, alt) for the HDR view.
// Degenerate (base == alt) is a step function at H = base: H >= base
// -> +1, H < base -> 0 -- so the SDR view (H = 0) gets +1 only when
// base == alt == 0 (pinned by the W0 test). Non-finite or negative
// headroom returns +1 outright, for both views. The degenerate check
// runs BEFORE the division, so base == alt never divides by zero.
// base == alt == 0 behaves exactly as the old weight-1.0 HDR view;
// base == alt > 0 is new behaviour (a file the old gates skipped
// now gets legacy handling -- see the NB-1 plan).
// Returns the weight in [-1, 1]: +1 full forward gain, 0 base unchanged,
// -1 full inverse gain (HDR base -> SDR view).
double ComputeGainWeight(const Metadata& meta, double displayHeadroom);

// R-10: true when the lazy 8-bit alternate must not be computed:
// the 16-bit precompute was attempted and failed (or the installed
// alternate was lost) AND the base view is HDR. False when the
// 16-bit path never ran (lazy is the only/designed path) and for
// SDR-base files (lazy forward gain from the true SDR base).
// Degenerate base == alt is legacy direction (not HDR base).
bool ShouldRefuseLazyAlternate(const Metadata& meta, bool accurateAlternateLost);

// Reconstruct scene-linear HDR float RGB from an 8-bit sRGB base image
// and a single-channel 8-bit gain map, using the ISO 21496-1 formula
// above. `baseSrgb` is width*height*3 bytes, 8-bit sRGB, RGB order.
// `gainMap` is gainW*gainH bytes, 8-bit grayscale; it is upsampled
// bilinearly to base size when gainW/gainH differ from width/height.
// `meta` must be valid (see Metadata). `weightFactor` is clamped to
// [-1,1] (-1 = full inverse gain, 0 = SDR base unchanged, 1 = full HDR).
// `outLinear` is
// width*height*3 floats, RGB order, scene-linear. Double math is used
// internally; the output is float. Returns false on bad input (null
// pointers, non-positive dimensions, !meta.valid, invalid gamma /
// non-finite metadata values, NaN weightFactor). The per-pixel work is
// two 256-entry lookup tables indexed by byte values (boostTable from
// the gain byte, srgbTable from the sRGB byte), built once per call
// from the fixed per-image parameters — exact, not approximate, with
// no per-pixel pow. gainMapMin/gainMapMax are clamped to [-16, +16]
// stops at the entry (GainMap.cpp) before the tables are built.
bool ApplyGainMap(const uint8_t* baseSrgb, int width, int height,
                  const uint8_t* gainMap, int gainW, int gainH,
                  const Metadata& meta, double weightFactor, float* outLinear);

// Fused gain-map apply + tone map (variant B: 2D LUT + exact OETF).
// Same reconstruction as ApplyGainMap, but the BT.2390 tone mapping is
// fused in: instead of writing a float linear buffer and running
// HdrToneMap::ConvertLinearImage over it (~27 pow per pixel), the
// per-pixel curve collapses to a 256x256 double table built once per
// image (gain byte x base byte), because the only inputs are two
// bytes. The main loop is table lookup -> GamutMap -> exact SrgbOetf
// -> ToByte, with no pow and no float buffer. Bit-identical to
// ApplyGainMap + ConvertLinearImage(/ToBgra) on the same inputs (see
// TestFusedGainMapExact); the table is double, not float, so the
// fused loop feeds GamutMap the exact double the old code computes.
// `baseBgr`/`baseBgra` is the 8-bit sRGB base in JPEGView order
// (B,G,R[,A]); `baseStride`/`outStride` are the byte strides between
// rows (BGR: width*3 padded, e.g. TJPAD; BGRA: width*4). The BGRA
// variant copies the base alpha byte to the output (P2-B4). The gain
// map is upsampled as in ApplyGainMap. Returns false on bad input
// (same contract as ApplyGainMap, plus stride checks: a stride
// narrower than a row, or so large the row arithmetic could wrap
// size_t, is rejected). On false the output buffer is untouched and
// the caller keeps the SDR base, exactly as with ApplyGainMap.
bool ApplyGainMapToBgr(const uint8_t* baseBgr, int width, int height, size_t baseStride,
                       const uint8_t* gainMap, int gainW, int gainH,
                       const Metadata& meta, double weightFactor,
                       uint8_t* outBgr, size_t outStride);
bool ApplyGainMapToBgra(const uint8_t* baseBgra, int width, int height, size_t baseStride,
                        const uint8_t* gainMap, int gainW, int gainH,
                        const Metadata& meta, double weightFactor,
                        uint8_t* outBgra, size_t outStride);

// 16-bit-base gain-map apply (NB-1 signed-weight unification).
// Reconstructs the alternate (non-base) view from a 16-bit HDR base:
// `baseSignalRgb` is width*height*3 floats, RGB order, signal values
// [0,1] in the source transfer/gamut (the same domain
// HdrToneMap::ConvertPixelWithParams takes). `gainMap` is gainW*gainH
// bytes, 8-bit grayscale (upsampled bilinearly when it differs from
// width/height). `weightFactor` is clamped to [-1,1] (-1 = full inverse
// gain for an HDR base, +1 = full forward gain). `alpha16` is an
// optional width*height 16-bit alpha plane (NULL = fully opaque);
// `alphaMaxVal` is its full-scale value (65535 for AVIF/JXL,
// (1<<bpp)-1 for HEIF); the output alpha is alpha16*255/alphaMaxVal
// (rounded). `outBgra` is width*height*4 bytes, BGRA order, with
// `outStride` bytes between rows.
// Per pixel: signal -> display-linear (PQ: PqSignalToNits/203; HLG:
// HlgSignalToSceneLinear + HlgOotfForward; other transfers pass the
// signal through as linear) WITHOUT any EETF -- the EETF runs only once,
// on the output side via ConvertLinearPixelWithParams -- then the gain
// ((linear + baseOffset) * boost - alternateOffset), then the content
// peak (max source-gamut luminance of the post-gain linear values,
// negatives/NaN as 0, non-finite -> 1.0) drives the EETF, then the
// gamut inverse (NULL-checked: sRGB is the identity) and the sRGB
// OETF. Returns false on bad input (same contract as ApplyGainMap,
// plus stride checks: a stride narrower than a row, or so large the
// row arithmetic could wrap size_t, is rejected). On false the output
// buffer is untouched and the caller keeps the base view.
bool ApplyGainMapToBgra16(const float* baseSignalRgb, int width, int height,
                          HdrToneMap::Transfer transfer, HdrToneMap::Primaries primaries,
                          const uint8_t* gainMap, int gainW, int gainH,
                          const Metadata& meta, double weightFactor,
                          const uint16_t* alpha16, uint32_t alphaMaxVal,
                          uint8_t* outBgra, size_t outStride);

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

// Extracted (not yet applied) gain map: the native-resolution gray gain
// plus its metadata, as produced by a format reader's Extract step. The
// load thread attaches it to the CJPEGImage; the image applies it on
// demand (ToggleHDR) or at load when GainMapDefault() is HDR. `valid` is
// false until a reader has filled the struct in.
struct ExtractedGainMap {
    std::vector<uint8_t> gray;
    int width = 0;
    int height = 0;
    Metadata meta;
    bool valid = false;
    // Precomputed alternate view (NB-1 scheme A): the non-base view
    // (8-bit BGRA, baseWidth*baseHeight*4 bytes) computed by the
    // wrapper from the 16-bit base at load, for HDR (PQ/HLG) bases
    // with a gain map. Empty when the wrapper did not precompute it
    // (SDR base, or the 16-bit apply failed); the lazy 8-bit path
    // (CJPEGImage::ComputeHdrBuffer) then computes it on toggle --
    // but only when the 16-bit path never ran (see
    // accurateAlternateLost below). The buffer is at the
    // PRE-rotation dimensions; the load-time rotation is mirrored
    // onto it (CJPEGImage::Rotate / RotateOriginalPixels), and the
    // reader alpha-blends it like the base before handing it over.
    std::vector<uint8_t> alternateBgra;
    // R-10: true when the accurate 16-bit alternate view was
    // attempted and is now unavailable -- the wrapper's 16-bit
    // precompute ran and failed, the install copy/size check in
    // SetRetainedGainMap failed, or the installed alternate was
    // lost in the load-time rotation mirror. False when the 16-bit
    // path never ran (SDR base, sRGB/untagged transfer, JPEG).
    // ComputeHdrBuffer refuses the lazy 8-bit recompute for
    // HDR-base files when this is set: the lazy path would be the
    // plan section 2.4-disproved inverse gain from tone-mapped
    // 8-bit pixels, so the image stays HDR instead.
    bool accurateAlternateLost = false;
};

// Rotate a single-channel gray image by `quarterTurns` clockwise
// 90-degree steps (exact permutation, same direction as
// CBasicProcessing::Rotate32bpp: 1 = 90 deg clockwise). `quarterTurns`
// is normalized mod 4; 0 copies the input. Returns false on bad input
// or allocation failure; never throws.
bool RotateGrayQuarterTurns(const uint8_t* src, int srcW, int srcH, int quarterTurns,
                            std::vector<uint8_t>& dst, int& dstW, int& dstH);

// Rotate a single-channel gray image by an arbitrary angle (radians,
// same direction and sampling convention as CBasicProcessing::RotateHQ:
// the target pixel (i,j) samples the source at center + R(-angle) *
// ((i+offsetX, j+offsetY) - center)). Bilinear sampling with edge
// clamping (out-of-image areas take the nearest edge value: a gain map
// has no background colour). `offsetX/Y` and `dstW/H` come from the
// caller's GetSizeAfterFreeRotation at gain resolution. Returns false on
// bad input or allocation failure; never throws.
bool RotateGrayFree(const uint8_t* src, int srcW, int srcH, double angleRad,
                    int offsetX, int offsetY, int dstW, int dstH,
                    std::vector<uint8_t>& dst);

}  // namespace gainmap
