#include "stdafx.h"

#include "HEIFWrapper.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"

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
	// F-06: gate on the header dimensions BEFORE decoding. decode_image
	// below decodes the full frame; a 65535x65535 header would force
	// GB-scale decoder-internal allocation before the old post-decode check
	// ever ran. The post-decode checks below stay as a safety net.
	width = handle.get_width();
	height = handle.get_height();
	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	if (abs((double)width * height) > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		return NULL;
	}
	if (!IsImageBufferSizeOk((size_t)width * nchannels, (size_t)height)) {
		outOfMemory = true;
		return NULL;
	}
	heif::Image image = handle.decode_image(heif_colorspace_RGB, heif_chroma_interleaved_RGBA);
	int stride;
	uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
	width = image.get_width(heif_channel_interleaved);
	height = image.get_height(heif_channel_interleaved);

	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	if (abs((double)width * height) > (double)CSettingsProvider::This().MaxImagePixels()) {
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
