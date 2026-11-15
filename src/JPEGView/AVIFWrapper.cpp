#include "stdafx.h"

#include "AVIFWrapper.h"
#include "avif/avif.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"
#include "HdrToneMap.h"

// HDR (Option A): convert the 16-bit BGRA pixels libavif wrote (depth 16,
// values scaled to [0, 65535]) to 8-bit BGRA via the HdrToneMap helper.
// Two passes: build a float RGB buffer and compute the content peak
// (for the content-adaptive EETF), then tone-map each pixel. Returns a
// new 8-bit BGRA buffer (caller owns it), or NULL on allocation failure.
// The alpha channel is ignored (the tone mapper works on RGB); the output
// alpha is opaque.
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
			HdrToneMap::ConvertPixel((double)src[0], (double)src[1], (double)src[2],
				transfer, primaries, peakNits, r, g, b);
			dst[0] = b;
			dst[1] = g;
			dst[2] = r;
			dst[3] = 0xff;
		}
	}
	delete[] rgb;
	return out;
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
