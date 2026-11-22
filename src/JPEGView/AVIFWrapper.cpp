#include "stdafx.h"

#include "AVIFWrapper.h"
#include "avif/avif.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"
#include "HdrToneMap.h"
#include "GainMap.h"
#include <cmath>
#include <vector>

// HDR (Option A): convert the 16-bit BGRA pixels libavif wrote (depth 16,
// values scaled to [0, 65535]) to 8-bit BGRA via the HdrToneMap helper.
// Two passes: build a float RGB buffer and compute the content peak
// (for the content-adaptive EETF), then tone-map each pixel. Returns a
// new 8-bit BGRA buffer (caller owns it), or NULL on allocation failure.
// The alpha channel is copied from the 16-bit source (scaled /257);
// the tone mapper itself works on RGB only. (P2-B4: previously the
// output alpha was forced opaque, so a transparent HDR pixel rendered
// as a black block after the background blend.)
static unsigned char* ConvertAvifHdrToSdr(const uint8_t* pixels16, int width, int height, const HdrToneMap::Cicp& cicp)
{
	size_t rgbSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &rgbSize))
		return NULL;
	float* rgb = new(std::nothrow) float[(size_t)width * height * 3];
	if (rgb == NULL)
		return NULL;
	// Pass 1: 16-bit BGRA -> float RGB (0..1). BGRA order: B, G, R, A.
	const uint16_t* p16 = (const uint16_t*)pixels16;
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			const uint16_t* px = p16 + ((size_t)y * width + x) * 4;
			float* dst = rgb + ((size_t)y * width + x) * 3;
			dst[0] = (float)((double)px[2] / 65535.0);
			dst[1] = (float)((double)px[1] / 65535.0);
			dst[2] = (float)((double)px[0] / 65535.0);
		}
	}
	HdrToneMap::Transfer transfer = HdrToneMap::TransferFromCode(cicp.transfer);
	HdrToneMap::Primaries primaries = HdrToneMap::PrimariesFromCode(cicp.primaries);
	double peakNits = HdrToneMap::ComputePeakNits(rgb, (size_t)width * height, transfer, primaries);
	// P2-C3: precompute the EETF parameters once per image (the
	// per-pixel ConvertPixel call recomputed them every pixel).
	HdrToneMap::Bt2390Params eetfParams = HdrToneMap::Bt2390MakeParams(peakNits);
	// Pass 2: tone-map each pixel to 8-bit BGRA.
	size_t outSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 4, (size_t)height, &outSize)) {
		delete[] rgb;
		return NULL;
	}
	unsigned char* out = new(std::nothrow) unsigned char[outSize];
	if (out == NULL) {
		delete[] rgb;
		return NULL;
	}
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			const float* src = rgb + ((size_t)y * width + x) * 3;
			unsigned char* dst = out + ((size_t)y * width + x) * 4;
			uint8_t r = 0, g = 0, b = 0;
			HdrToneMap::ConvertPixelWithParams((double)src[0], (double)src[1], (double)src[2],
				transfer, primaries, eetfParams, r, g, b);
			dst[0] = b;
			dst[1] = g;
			dst[2] = r;
			// P2-B4: copy the source alpha (16-bit -> 8-bit, /257 maps
			// 65535 -> 255 exactly), not opaque.
			const uint16_t* px = p16 + ((size_t)y * width + x) * 4;
			dst[3] = (uint8_t)(px[3] / 257);
		}
	}
	delete[] rgb;
	return out;
}

// Gain-map support (Option D, Phase 4): ISO 21496-1 AVIF gain map.
//
// A gain-map AVIF is an SDR base image plus a lower-resolution
// single-channel gain-map image, with the reconstruction parameters
// carried in the file's gain-map metadata box. libavif 1.4.2 parses
// that metadata natively and decodes the gain-map image, but ONLY
// when the decoder is told to: decoder->imageContentToDecode must
// include AVIF_IMAGE_CONTENT_GAIN_MAP before avifDecoderParse()
// (the default is colour+alpha only, so without the flag
// decoder->image->gainMap stays NULL). After a successful
// avifDecoderNthImage() the gain map is at decoder->image->gainMap:
// gainMap->image holds the decoded planes (the Y plane is the gain
// map; transformFlags is always AVIF_TRANSFORM_NONE) and
// gainMapMin/gainMapMax/gainMapGamma/baseOffset/alternateOffset/
// baseHdrHeadroom/alternateHdrHeadroom hold the metadata, per
// channel as fractions.
//
// The gain math itself is NOT re-implemented here: the metadata
// (channel 0 — the shared helper is single-channel, same convention
// as its multi-channel handling) and the Y plane (normalised from
// its native 8/10/12-bit depth to 8-bit) are fed into the shared
// gainmap::ApplyGainMap, and the linear HDR output is tone-mapped
// with HdrToneMap::ConvertLinearImage, exactly like the JPEG
// UltraHDR and HEIF gain-map paths. libavif's own
// avifImageApplyGainMap() is NOT used: it tone-maps for a display
// headroom and bakes in its own tone mapping, while JPEGView wants
// the full reconstruction (weight 1.0) plus its own BT.2390 path.
//
// TryApplyAvifGainMap runs only on the SDR path (a gain-map AVIF is
// SDR base + gain map, never PQ/HLG) and returns a NEW 8-bit BGRA
// buffer, or NULL when there is no gain map or anything fails — the
// caller keeps the base BGRA unchanged in that case. A broken gain
// map must never fail the whole load.

// avifSignedFraction/avifUnsignedFraction -> double. Returns false
// when the denominator is 0 or the value is non-finite.
static bool AvifSignedFractionToDouble(const avifSignedFraction& f, double& out)
{
	if (f.d == 0)
		return false;
	out = (double)f.n / (double)f.d;
	return std::isfinite(out);
}

static bool AvifUnsignedFractionToDouble(const avifUnsignedFraction& f, double& out)
{
	if (f.d == 0)
		return false;
	out = (double)f.n / (double)f.d;
	return std::isfinite(out);
}

// Convert libavif's per-channel gain-map metadata (channel 0 only)
// to the shared helper's Metadata. Returns false when any essential
// fraction has a zero denominator, gamma is not > 0, or a value is
// non-finite. A multi-channel gain map whose channels differ is
// approximated with channel 0.
static bool AvifGainMapToMetadata(const avifGainMap* gainMap, gainmap::Metadata& meta)
{
	meta = gainmap::Metadata();
	if (gainMap == NULL)
		return false;
	double gainMapMin = 0.0, gainMapMax = 0.0, gamma = 1.0;
	double baseOffset = 0.0, alternateOffset = 0.0;
	double baseHeadroom = 0.0, alternateHeadroom = 0.0;
	if (!AvifSignedFractionToDouble(gainMap->gainMapMin[0], gainMapMin) ||
		!AvifSignedFractionToDouble(gainMap->gainMapMax[0], gainMapMax) ||
		!AvifUnsignedFractionToDouble(gainMap->gainMapGamma[0], gamma) ||
		!AvifSignedFractionToDouble(gainMap->baseOffset[0], baseOffset) ||
		!AvifSignedFractionToDouble(gainMap->alternateOffset[0], alternateOffset) ||
		!AvifUnsignedFractionToDouble(gainMap->baseHdrHeadroom, baseHeadroom) ||
		!AvifUnsignedFractionToDouble(gainMap->alternateHdrHeadroom, alternateHeadroom))
		return false;
	if (!(gamma > 0.0))
		return false;
	meta.gainMapMin = gainMapMin;
	meta.gainMapMax = gainMapMax;
	meta.gamma = gamma;
	meta.baseOffset = baseOffset;
	meta.alternateOffset = alternateOffset;
	meta.baseHdrHeadroom = baseHeadroom;
	meta.alternateHdrHeadroom = alternateHeadroom;
	meta.useBaseColorSpace = gainMap->useBaseColorSpace ? true : false;
	meta.valid = true;
	return true;
}

// Decode the gain-map Y plane to a packed single-channel 8-bit buffer
// (gainW x gainH bytes). The gain map is usually YUV400 (monochrome)
// but may be YUV444/422/420; in every case the Y plane is channel 0's
// gain map at full resolution. Samples are normalised from the native
// depth (8/10/12-bit, up to 16) to 8-bit with round-half-up. The gain
// map must not be absurdly larger than the base (sanity cap 4x per
// side — ApplyGainMap upsamples it bilinearly to base size).
static bool DecodeAvifGainMapGray(const avifGainMap* gainMap, int baseWidth, int baseHeight,
	std::vector<uint8_t>& gainGray, int& gainW, int& gainH)
{
	gainW = gainH = 0;
	if (gainMap == NULL || gainMap->image == NULL)
		return false;
	const avifImage* gmImage = gainMap->image;
	int depth = (int)gmImage->depth;
	if (depth < 8 || depth > 16)
		return false;
	int gw = (int)avifImagePlaneWidth(gmImage, AVIF_CHAN_Y);
	int gh = (int)avifImagePlaneHeight(gmImage, AVIF_CHAN_Y);
	if (gw <= 0 || gh <= 0 || gw > baseWidth * 4 || gh > baseHeight * 4)
		return false;
	const uint8_t* plane = avifImagePlane(gmImage, AVIF_CHAN_Y);
	uint32_t rowBytes = avifImagePlaneRowBytes(gmImage, AVIF_CHAN_Y);
	if (plane == NULL || rowBytes == 0)
		return false;
	const uint32_t maxVal = (uint32_t)((1u << depth) - 1);
	const size_t bytesPerSample = (depth > 8) ? 2 : 1;
	if (rowBytes < (uint32_t)gw * bytesPerSample)
		return false;
	if (!IsImageBufferSizeOk((size_t)gw, (size_t)gh))
		return false;
	gainGray.assign((size_t)gw * gh, 0);
	for (int y = 0; y < gh; y++) {
		const uint8_t* srcRow = plane + (size_t)y * rowBytes;
		uint8_t* dstRow = &gainGray[(size_t)y * gw];
		if (depth == 8) {
			memcpy(dstRow, srcRow, (size_t)gw);
		} else {
			// 16-bit planes are native-endian uint16_t (little-endian
			// on Windows x86/x64).
			const uint16_t* src16 = (const uint16_t*)srcRow;
			for (int x = 0; x < gw; x++) {
				uint32_t v = src16[x];
				if (v > maxVal)
					v = maxVal;
				dstRow[x] = (uint8_t)((v * 255 + maxVal / 2) / maxVal);
			}
		}
	}
	gainW = gw;
	gainH = gh;
	return true;
}

// Apply the gain map of an SDR AVIF base image, if it has one.
// `baseBgra` is the already-decoded 8-bit BGRA base (width x height,
// after the ICC transform, so it is sRGB — the shared helper's input
// format). Returns a NEW 8-bit BGRA buffer with the gain map applied
// and tone-mapped via HdrToneMap::ConvertLinearImage, or NULL when
// there is no gain map or anything fails — the caller keeps `baseBgra`
// unchanged in that case.
static unsigned char* TryApplyAvifGainMap(const avifImage* image,
	const unsigned char* baseBgra, int width, int height)
{
	unsigned char* outBgra = NULL;
	if (image == NULL || baseBgra == NULL || width <= 0 || height <= 0)
		return NULL;
	const avifGainMap* gainMap = image->gainMap;
	if (gainMap == NULL)
		return NULL;
	gainmap::Metadata meta;
	if (!AvifGainMapToMetadata(gainMap, meta))
		return NULL;
	std::vector<uint8_t> gainGray;
	int gainW = 0, gainH = 0;
	if (!DecodeAvifGainMapGray(gainMap, width, height, gainGray, gainW, gainH))
		return NULL;
	// Base BGRA -> RGB temp buffer (drop alpha), then reconstruct
	// linear HDR floats and tone-map straight to BGRA with
	// ConvertLinearImageToBgra, which copies the base alpha
	// (interleaved: baseBgra+3, stride 4). P2-B4: the old repack
	// forced alpha opaque, so a transparent gain-map pixel rendered
	// as a black block after the background blend.
	size_t rgbSize = 0, floatSize = 0, bgraSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3, (size_t)height, &rgbSize)
		|| !IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &floatSize)
		|| !IsImageBufferSizeOk((size_t)width * 4, (size_t)height, &bgraSize))
		return NULL;
	std::vector<uint8_t> rgb(rgbSize);
	std::vector<float> linear(floatSize / sizeof(float));
	for (size_t i = 0; i < (size_t)width * height; i++) {
		rgb[i * 3 + 0] = baseBgra[i * 4 + 2];
		rgb[i * 3 + 1] = baseBgra[i * 4 + 1];
		rgb[i * 3 + 2] = baseBgra[i * 4 + 0];
	}
	if (!gainmap::ApplyGainMap(rgb.data(), width, height, gainGray.data(), gainW, gainH, meta, 1.0, linear.data()))
		return NULL;
	outBgra = new(std::nothrow) unsigned char[bgraSize];
	if (outBgra == NULL)
		return NULL;
	// P2-B4: tone-map to BGRA in place, copying the base alpha.
	HdrToneMap::ConvertLinearImageToBgra(linear.data(), width, height, (size_t)width * 3,
		outBgra, (size_t)width * 4, baseBgra + 3, 4);
	return outBgra;
}

void* AvifReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int frame_index,
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	has_animation = false;
	exif_chunk = NULL;

	avifResult result;
	int nthreads = 256; // sets maximum number of active threads allowed for libavif, default is 1

	// Cache animations
	if (cache.decoder == NULL) {
		cache.data = (uint8_t*)malloc(sizebytes);
		if (cache.data == NULL) {
			outOfMemory = true;
			return NULL;
		}
		memcpy(cache.data, buffer, sizebytes);
		cache.decoder = avifDecoderCreate();
		if (cache.decoder == NULL) {
			// libavif returns NULL on OOM or when no decoder was compiled in.
			outOfMemory = true;
			DeleteCache();
			return NULL;
		}
		cache.decoder->maxThreads = nthreads;
		cache.decoder->strictFlags = AVIF_STRICT_DISABLED;
		// Gain-map (Option D, Phase 4): ask libavif to decode the gain
		// map too (imageContentToDecode is only honoured at Parse time,
		// so this must be set before avifDecoderParse below). The
		// default is colour+alpha only, which leaves
		// decoder->image->gainMap NULL even for a gain-map file. Files
		// without a gain map are unaffected (nothing extra is decoded).
		cache.decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_ALL;
		result = avifDecoderSetIOMemory(cache.decoder, cache.data, sizebytes);
		if (result != AVIF_RESULT_OK) {
			DeleteCache();
			return NULL;
		}
		result = avifDecoderParse(cache.decoder);
		if (result != AVIF_RESULT_OK) {
			DeleteCache();
			return NULL;
		}
		// P2-B6 (D6): gate the gain-map dimensions BEFORE decoding any
		// frame. avifDecoderParse already populated
		// decoder->image->gainMap->image from the gain-map item's ispe
		// box, so an absurd gain map can be rejected here; otherwise
		// avifDecoderNthImage below would decode its full planes
		// before DecodeAvifGainMapGray's 4x check ever ran. When the
		// gain map is absurd, drop it and re-parse as SDR: the gain
		// map is optional, so the file still loads (as SDR). The F-06
		// gate below runs once, on the final parse. cache.data stays
		// valid across the re-parse (the IO only points at it).
		if (cache.decoder->image != NULL && cache.decoder->image->gainMap != NULL &&
			cache.decoder->image->gainMap->image != NULL) {
			uint32_t baseW = cache.decoder->image->width;
			uint32_t baseH = cache.decoder->image->height;
			uint32_t gmW = cache.decoder->image->gainMap->image->width;
			uint32_t gmH = cache.decoder->image->gainMap->image->height;
			if (gmW < 1 || gmH < 1 || (uint64_t)gmW > (uint64_t)baseW * 4 ||
				(uint64_t)gmH > (uint64_t)baseH * 4) {
				avifDecoderDestroy(cache.decoder);
				cache.decoder = avifDecoderCreate();
				if (cache.decoder == NULL) {
					outOfMemory = true;
					DeleteCache();
					return NULL;
				}
				cache.decoder->maxThreads = nthreads;
				cache.decoder->strictFlags = AVIF_STRICT_DISABLED;
				cache.decoder->imageContentToDecode =
					(avifImageContentTypeFlags)(AVIF_IMAGE_CONTENT_ALL & ~AVIF_IMAGE_CONTENT_GAIN_MAP);
				result = avifDecoderSetIOMemory(cache.decoder, cache.data, sizebytes);
				if (result != AVIF_RESULT_OK) {
					DeleteCache();
					return NULL;
				}
				result = avifDecoderParse(cache.decoder);
				if (result != AVIF_RESULT_OK) {
					DeleteCache();
					return NULL;
				}
			}
		}
		// F-06: gate on the parsed header dimensions BEFORE decoding the
		// frame. avifDecoderNthImage below decodes the full YUV planes; a
		// 65535x65535 header would force GB-scale decoder-internal allocation
		// before the old post-decode check ever ran. All frames of an AVIF
		// sequence share the primary image dimensions, so the parsed first
		// image is the right thing to gate on. If Parse left no image
		// (should not happen), fall through: the post-decode check below
		// still applies.
		if (cache.decoder->image != NULL) {
			uint32_t nHeaderWidth = cache.decoder->image->width;
			uint32_t nHeaderHeight = cache.decoder->image->height;
			if (nHeaderWidth < 1 || nHeaderHeight < 1 ||
				nHeaderWidth > (uint32_t)MAX_IMAGE_DIMENSION || nHeaderHeight > (uint32_t)MAX_IMAGE_DIMENSION ||
				!IsImageBufferSizeOk((size_t)nHeaderWidth * nchannels, (size_t)nHeaderHeight)) {
				DeleteCache();
				return NULL;
			}
			// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory. The dimension/byte checks above stay plain rejections.
			if ((double)nHeaderWidth * nHeaderHeight > (double)CSettingsProvider::This().MaxImagePixels()) {
				isSizeLimitExceeded = true;
				DeleteCache();
				return NULL;
			}
		}
		cache.rgb = { 0 };
		
		cache.data_size = sizebytes;
	}
	
	// Decode a frame
	result = avifDecoderNthImage(cache.decoder, frame_index);
	if (result != AVIF_RESULT_OK) {
		DeleteCache();
		return NULL;
	}
	avifRGBImageSetDefaults(&cache.rgb, cache.decoder->image);
	// HDR detection (Option A): the CICP codes come straight from the
	// AVIF image header (colorPrimaries / transferCharacteristics).
	// Only PQ/HLG take the HDR path: decode to 16-bit (depth 16
	// scales the 10/12-bit values to [0, 65535]) and run the tone
	// mapper after avifImageYUVToRGB. SDR keeps depth 8 and the exact
	// pre-existing path.
	HdrToneMap::Cicp cicp;
	cicp.primaries = (int)cache.decoder->image->colorPrimaries;
	cicp.transfer = (int)cache.decoder->image->transferCharacteristics;
	cicp.matrix = (int)cache.decoder->image->matrixCoefficients;
	cicp.fullRange = (cache.decoder->image->yuvRange == AVIF_RANGE_FULL) ? 1 : 0;
	const bool isHdr = HdrToneMap::IsHdr(cicp);
	cache.rgb.depth = isHdr ? 16 : 8;
	cache.rgb.format = AVIF_RGB_FORMAT_BGRA;
	cache.rgb.maxThreads = nthreads;

	width = cache.rgb.width;
	height = cache.rgb.height;
	has_animation = cache.decoder->imageCount > 1;
	frame_count = cache.decoder->imageCount;
	frame_time = (int)(cache.decoder->imageTiming.duration * 1000.0);

	// This file used to have no dimension or pixel check at all; libavif will hand
	// back whatever the container declares.
	if (width < 1 || height < 1 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		DeleteCache();
		return NULL;
	}
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	if ((double)width * height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		DeleteCache();
		return NULL;
	}

	// width * nchannels * height is int arithmetic: 32768x32768x4 wraps to 0 and
	// allocates nothing while avifImageYUVToRGB writes 4GB. HDR uses
	// 16-bit pixels (8 bytes), so the stride doubles.
	const size_t bytesPerPixel = isHdr ? 8 : 4;
	size_t size = 0;
	if (!IsImageBufferSizeOk((size_t)width * bytesPerPixel, (size_t)height, &size)) {
		outOfMemory = true;
		DeleteCache();
		return NULL;
	}
	cache.rgb.pixels = new(std::nothrow) unsigned char[size];
	if (cache.rgb.pixels == NULL) {
		outOfMemory = true;
		return NULL;
	}
	cache.rgb.rowBytes = (uint32_t)(width * bytesPerPixel);
	result = avifImageYUVToRGB(cache.decoder->image, &cache.rgb);
	if (result != AVIF_RESULT_OK) {
		delete[] cache.rgb.pixels;
		DeleteCache();
		return NULL;
	}
	if (isHdr) {
		// HDR: tone-map the 16-bit BGRA to 8-bit BGRA now, so the
		// clap/irot/imir transforms below (which expect 32bpp) and
		// the rest of the pipeline see the same 8-bit BGRA as SDR.
		unsigned char* pixels8 = ConvertAvifHdrToSdr(cache.rgb.pixels, width, height, cicp);
		if (pixels8 == NULL) {
			delete[] cache.rgb.pixels;
			outOfMemory = true;
			DeleteCache();
			return NULL;
		}
		delete[] cache.rgb.pixels;
		cache.rgb.pixels = pixels8;
		cache.rgb.rowBytes = (uint32_t)(width * 4);
	}

	// Handle clap, irot and imir boxes
	avifTransformFlags flags = cache.decoder->image->transformFlags;
	if (flags & AVIF_TRANSFORM_CLAP) {
		avifCleanApertureBox* clap = &cache.decoder->image->clap;
		avifCropRect crop;
		avifDiagnostics diag;
		if (avifCropRectConvertCleanApertureBox(&crop, clap, width, height, cache.decoder->image->yuvFormat, &diag)) {
			POINT point = { crop.x, crop.y };
			SIZE sz = { crop.width, crop.height };
			void* pixels = CBasicProcessing::Crop32bpp(width, height, cache.rgb.pixels, CRect(point, sz));
			if (pixels != NULL) {
				delete[] cache.rgb.pixels;
				cache.rgb.pixels = (uint8_t*)pixels;
				width = crop.width;
				height = crop.height;
			}
		}
	}
	if (flags & AVIF_TRANSFORM_IROT) {
		int angle = 360 - cache.decoder->image->irot.angle * 90;
		void* pixels = CBasicProcessing::Rotate32bpp(width, height, cache.rgb.pixels, angle);
		if (pixels != NULL) {
			delete[] cache.rgb.pixels;
			cache.rgb.pixels = (uint8_t*)pixels;
			if (angle != 180) {
				int temp = width;
				width = height;
				height = temp;
			}
		}
	}
	if (flags & AVIF_TRANSFORM_IMIR) {
		void* pixels = CBasicProcessing::Mirror32bpp(width, height, cache.rgb.pixels, cache.decoder->image->imir.axis);
		if (pixels != NULL) {
			delete[] cache.rgb.pixels;
			cache.rgb.pixels = (uint8_t*)pixels;
		}
	}

	// HDR files carry no ICC profile (they use nclx/CICP); the tone
	// mapper is the only colour conversion for them. Skipping the ICC
	// transform here also guards against double-applying if one were
	// ever present alongside a PQ/HLG transfer.
	if (!isHdr) {
		avifRWData icc = cache.decoder->image->icc;
		if (cache.transform == NULL)
			cache.transform = ICCProfileTransform::CreateTransform(icc.data, icc.size, ICCProfileTransform::FORMAT_BGRA);
		ICCProfileTransform::DoTransform(cache.transform, cache.rgb.pixels, cache.rgb.pixels, width, height);
	}
	if (!isHdr && flags == AVIF_TRANSFORM_NONE) {
		// Gain-map (Option D, Phase 4): an SDR AVIF with a gain map is
		// an HDR photo — reconstruct the HDR image and tone-map it.
		// TryApplyAvifGainMap returns NULL when there is no gain map
		// or anything fails; the SDR base is kept unchanged then.
		// It runs after the ICC transform (the base it reconstructs
		// from is sRGB, like the HEIF path).
		// P2-A1: gain-map work allocates (gainGray.assign, three
		// vectors, ApplyGainMap's upsampled resize). A bad_alloc here
		// must not fail the whole load -- fall back to SDR.
		unsigned char* gainMapped = NULL;
		try {
			gainMapped = TryApplyAvifGainMap(cache.decoder->image, cache.rgb.pixels, width, height);
		} catch (...) {
			// Keep the SDR base image; the gain map is optional.
		}
		if (gainMapped != NULL) {
			delete[] cache.rgb.pixels;
			cache.rgb.pixels = gainMapped;
		}
	}

	avifRWData exif = cache.decoder->image->exif;
	if (exif.size > 8 && exif.size < 65528 && exif.data != NULL) {
		exif_chunk = malloc(exif.size + 10);
		if (exif_chunk != NULL) {
			memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
			*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size + 8);
			memcpy((uint8_t*)exif_chunk + 10, exif.data, exif.size);
		}
	}

	void* pPixelData = cache.rgb.pixels;
	if (!has_animation)
		DeleteCache();

	return pPixelData;
}

void AvifReader::DeleteCache() {
	if (cache.decoder)
		avifDecoderDestroy(cache.decoder);
	free(cache.data);
	ICCProfileTransform::DeleteTransform(cache.transform);
	cache = { 0 };
}
