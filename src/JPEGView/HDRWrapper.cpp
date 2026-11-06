#include "stdafx.h"

#include "HDRWrapper.h"
// stb_image is shared with PPMWrapper.cpp, which compiles the implementation
// (STB_IMAGE_IMPLEMENTATION) with JPEG/PNG/BMP/TGA/GIF/PIC compiled out.
// HDR must NOT be compiled out there (B-1 removed STBI_NO_HDR from
// PPMWrapper.cpp), or the calls below would fail: the HDR decoder lives
// in that same object. This file only uses the declarations, so it must
// not define STB_IMAGE_IMPLEMENTATION again.
#include "stb/stb_image.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"

void* HDRReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 2)
		return NULL;
	// HDR-only gate, before stb_image ever sees the buffer: the magic
	// must be "#?" (Radiance / RGBE). This rejects any non-HDR content
	// misnamed .hdr, which stb_image would otherwise happily decode as
	// another format.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	if (pMagic[0] != '#' || pMagic[1] != '?')
		return NULL;

	// Size gate BEFORE decode (F-06/F-07 lesson): read the dimensions
	// from the header only, and refuse absurd dimensions before
	// stb_image allocates a decode buffer for them.
	int nChannels = 0;
	if (!stbi_info_from_memory(pMagic, sizebytes, &width, &height, &nChannels))
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	if (abs((double)width * height) > (double)CSettingsProvider::This().MaxImagePixels())
		isSizeLimitExceeded = true;
	if (isSizeLimitExceeded || width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		return NULL;
	}
	// HDR is RGB only; anything else means the content was not HDR.
	if (nChannels != 3) {
		return NULL;
	}

	// 8-bit load: stb_image converts the float HDR data to 8-bit with
	// its default gamma-2.2 tone mapping (documented in the header).
	unsigned char* pDecoded = stbi_load_from_memory(pMagic, sizebytes, &width, &height, &nChannels, 0);
	if (pDecoded == NULL)
		return NULL;
	if (nChannels != 3) {
		stbi_image_free(pDecoded);
		return NULL;
	}
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		stbi_image_free(pDecoded);
		return NULL;
	}
	// Output is 3-byte BGR padded to a 4-byte boundary, like QOIWrapper.
	// width <= MAX_IMAGE_DIMENSION (65535), so width * 3 fits in int.
	size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
	size_t allocSize = 0;
	if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
		outOfMemory = true;
		stbi_image_free(pDecoded);
		return NULL;
	}
	unsigned char* pPixelData = new(std::nothrow) unsigned char[allocSize];
	if (pPixelData == NULL) {
		outOfMemory = true;
		stbi_image_free(pDecoded);
		return NULL;
	}
	// Zero the row padding so no uninitialised bytes reach the DIB.
	memset(pPixelData, 0, allocSize);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t srcIdx = ((size_t)y * (size_t)width + (size_t)x) * 3;
			size_t dstIdx = (size_t)y * padded_stride + (size_t)x * 3;
			// stb_image returns RGB; JPEGView pixels are BGR.
			pPixelData[dstIdx] = pDecoded[srcIdx + 2];
			pPixelData[dstIdx + 1] = pDecoded[srcIdx + 1];
			pPixelData[dstIdx + 2] = pDecoded[srcIdx];
		}
	}
	stbi_image_free(pDecoded);
	return (void*)pPixelData;
}
