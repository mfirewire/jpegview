#include "stdafx.h"

#include "HEIFWrapper.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"
#include "HdrToneMap.h"

// HDR (Option A): convert the 16-bit RGBA pixels libheif wrote
// (heif_chroma_interleaved_RRGGBBAA_LE, values in the native bit depth
// range [0, (1<<bpp)-1] - libheif does NOT scale them to 16-bit, unlike
// libavif) to 8-bit BGRA via the HdrToneMap helper. Two passes: build a
// float RGB buffer and compute the content peak (for the
// content-adaptive EETF), then tone-map each pixel. `stride` is the byte
// stride of the 16-bit RGBA plane. Returns a new 8-bit BGRA buffer
// (caller owns it), or NULL on allocation failure. The alpha channel is
// ignored (the tone mapper works on RGB); the output alpha is opaque.
static unsigned char* ConvertHeifHdrToSdr(const uint8_t* data, int stride, int width, int height, int bpp, const HdrToneMap::Cicp& cicp)
{
	size_t rgbSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &rgbSize))
		return NULL;
	float* rgb = new(std::nothrow) float[(size_t)width * height * 3];
	if (rgb == NULL)
		return NULL;
	const double maxVal = (double)((1 << bpp) - 1);
	// Pass 1: 16-bit RGBA rows -> float RGB (0..1). RGBA order: R, G, B, A.
	for (int y = 0; y < height; y++)
	{
		const uint16_t* row = (const uint16_t*)(data + (size_t)y * stride);
		for (int x = 0; x < width; x++)
		{
			const uint16_t* px = row + x * 4;
			float* dst = rgb + ((size_t)y * width + x) * 3;
			dst[0] = (float)((double)px[0] / maxVal);
			dst[1] = (float)((double)px[1] / maxVal);
			dst[2] = (float)((double)px[2] / maxVal);
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

void * HeifReader::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   int &frame_count,
					   void* &exif_chunk,
					   bool &outOfMemory,
					   bool &isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
					   int frame_index,
					   const void *buffer,
					   int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;

	unsigned char* pPixelData = NULL;
	exif_chunk = NULL;

	heif::Context context;
	context.read_from_memory_without_copy(buffer, sizebytes);
	frame_count = context.get_number_of_top_level_images();
	heif_item_id item_id = context.get_list_of_top_level_image_IDs().at(frame_index);
	heif::ImageHandle handle = context.get_image_handle(item_id);
	// HDR detection (Option A): the nclx (CICP) profile comes from the
	// image handle. get_nclx_color_profile throws when there is no
	// nclx (e.g. an ICC-profiled or untagged file), which means not
	// HDR. Only PQ/HLG take the HDR path: decode to 16-bit and run
	// the tone mapper. SDR keeps the 8-bit RGBA decode and the exact
	// pre-existing path. Gain-map HEIFs (iPhone HDR photos) keep
	// showing the SDR base image: the gain map is ignored, as today.
	HdrToneMap::Cicp cicp;
	bool isHdr = false;
	try {
		heif::ColorProfile_nclx nclx = handle.get_nclx_color_profile();
		cicp.primaries = (int)nclx.get_color_primaries();
		cicp.transfer = (int)nclx.get_transfer_characteristics();
		cicp.matrix = (int)nclx.get_matrix_coefficients();
		cicp.fullRange = nclx.is_full_range() ? 1 : 0;
		isHdr = HdrToneMap::IsHdr(cicp);
	} catch (const heif::Error&) {
		// No nclx profile: not HDR (SDR path below).
	}
	// F-06: gate on the header dimensions BEFORE decoding. decode_image
	// below decodes the full frame; a 65535x65535 header would force
	// GB-scale decoder-internal allocation before the old post-decode check
	// ever ran. The post-decode checks below stay as a safety net.
	width = handle.get_width();
	height = handle.get_height();
	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): libheif only returns non-negative dimensions; unqualified
	// abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		return NULL;
	}
	// The decoded plane is 16-bit RGBA (8 bytes/pixel) for HDR and
	// 8-bit RGBA (4 bytes/pixel) for SDR; bound the plane, not just
	// the 8-bit output buffer below.
	if (!IsImageBufferSizeOk((size_t)width * (isHdr ? 8 : nchannels), (size_t)height)) {
		outOfMemory = true;
		return NULL;
	}
	heif::Image image = handle.decode_image(heif_colorspace_RGB,
		isHdr ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA);
	int stride;
	uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
	width = image.get_width(heif_channel_interleaved);
	height = image.get_height(heif_channel_interleaved);

	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): libheif only returns non-negative dimensions; unqualified
	// abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		return NULL;
	}
	if (width < 1 || height < 1 || width * nchannels > stride)
		return NULL;

	// int 乘法会在 32768x32768x4 这类尺寸下回绕成 0；用 64 位算并校验上限。
	size_t size = 0;
	if (!IsImageBufferSizeOk((size_t)width * nchannels, (size_t)height, &size)) {
		outOfMemory = true;
		return NULL;
	}
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	if (isHdr) {
		// HDR: tone-map the 16-bit RGBA plane to 8-bit BGRA. The bit
		// depth comes from the decoded image: libheif keeps the
		// native depth in the uint16_t samples (a 12-bit file gives
		// values in [0, 4095], not scaled to [0, 65535]), so the
		// helper normalises by (1<<bpp)-1. The ICC transform is
		// skipped for HDR (HDR HEIFs use nclx, not an ICC profile);
		// the tone mapper is the only colour conversion.
		int bpp = image.get_bits_per_pixel(heif_channel_interleaved);
		if (bpp < 8 || bpp > 16)
			bpp = 16;
		unsigned char* hdrPixels = ConvertHeifHdrToSdr(data, stride, width, height, bpp, cicp);
		if (hdrPixels == NULL) {
			delete[] pPixelData;
			outOfMemory = true;
			return NULL;
		}
		memcpy(pPixelData, hdrPixels, size);
		delete[] hdrPixels;
	} else {
		std::vector<uint8_t> iccp = image.get_raw_color_profile();
		void* transform = ICCProfileTransform::CreateTransform(iccp.data(), iccp.size(), ICCProfileTransform::FORMAT_RGBA);
		size_t i, j;
		if (!ICCProfileTransform::DoTransform(transform, data, pPixelData, width, height, stride=stride)) {
			unsigned int* o = (unsigned int*)pPixelData;
			for (i = 0; i < height; i++) {
				unsigned int* p = (unsigned int*)(data + i * stride);
				for (j = 0; j < width; j++) {
					// RGBA -> BGRA conversion
					*o = _rotr(_byteswap_ulong(*p), 8);
					p++;
					o++;
				}
			}
		}
		ICCProfileTransform::DeleteTransform(transform);
	}

	std::vector<heif_item_id> exif_blocks = handle.get_list_of_metadata_block_IDs("Exif");

	if (!exif_blocks.empty()) {
		std::vector<uint8_t> exif = handle.get_metadata(exif_blocks[0]);
		// 65538 magic number comes from investigations by qbnu
		// see https://github.com/sylikc/jpegview/pull/213#pullrequestreview-1494451359 for more details
		/*
		* These libraries all have their own ideas about where to start the Exif data from.
		* JPEG Exif blocks are in the format
		  FF E1 SS SS 45 78 69 66 00 00 [data]

		  The SS SS is a big-endian unsigned short representing the size of everything after FF E1.

		  libjxl gives 00 00 00 00 [data]
		  libheif gives 00 00 00 00 45 78 69 66 00 00 [data]
		  libavif, libwebp and libpng give [data], so they have different limits for size.

		  If you want I can change it to 65536 + an offset and add notes explaining why.
		*/
		if (exif.size() > 8 && exif.size() < 65538) {
			exif_chunk = malloc(exif.size());
			if (exif_chunk != NULL) {
				memcpy(exif_chunk, exif.data(), exif.size());
				*((unsigned short*)exif_chunk) = _byteswap_ushort(0xFFE1);
				*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size() - 2);
			}
		}
	}

	return (void*)pPixelData;
}
