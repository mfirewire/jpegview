#include "stdafx.h"

#include "PBMWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"

void* PBMReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 2)
		return NULL;
	// P4-only gate: the magic must be "P4". This rejects P1/P2/P3/P5/P6/P7
	// and any non-PBM content misnamed .pbm.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	if (pMagic[0] != 'P' || pMagic[1] != '4')
		return NULL;

	// Parse width and height (whitespace and '#' comments between tokens,
	// raster starting right after the single delimiter byte following the
	// height), mirroring stb_image's PNM header parsing as used in
	// PPMWrapper.cpp. If parsing fails, pass nothing through: a PBM with
	// a malformed header is rejected here, not decoded as garbage.
	size_t pos = 2;
	unsigned char c = 0;
	int nValues[2] = { 0, 0 };
	bool bParseOk = true;
	if (pos >= (size_t)sizebytes) bParseOk = false;
	else c = pMagic[pos++];
	for (int t = 0; t < 2 && bParseOk; t++) {
		// skip whitespace and comments
		for (;;) {
			while (pos <= (size_t)sizebytes && (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r')) {
				if (pos >= (size_t)sizebytes) { bParseOk = false; break; }
				c = pMagic[pos++];
			}
			if (!bParseOk) break;
			if (c != '#') break;
			while (pos <= (size_t)sizebytes && c != '\n' && c != '\r') {
				if (pos >= (size_t)sizebytes) { bParseOk = false; break; }
				c = pMagic[pos++];
			}
			if (!bParseOk) break;
		}
		if (!bParseOk) break;
		if (c < '0' || c > '9') { bParseOk = false; break; }
		int v = 0;
		while (c >= '0' && c <= '9') {
			if (v > 214748364) { bParseOk = false; break; }
			v = v * 10 + (c - '0');
			if (pos >= (size_t)sizebytes) { bParseOk = false; break; }
			c = pMagic[pos++];
		}
		if (!bParseOk) break;
		nValues[t] = v;
	}
	if (!bParseOk)
		return NULL;
	width = nValues[0];
	height = nValues[1];
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	if (abs((double)width * height) > MAX_IMAGE_PIXELS)
		outOfMemory = true;
	if (outOfMemory)
		return NULL;

	// Size gate: the raster is ceil(width/8) bytes per row. The file
	// must actually hold that many bytes; otherwise it is truncated.
	size_t rowBytes = ((size_t)width + 7) / 8;
	size_t rasterBytes = rowBytes * (size_t)height;
	if (pos > (size_t)sizebytes || rasterBytes > (size_t)sizebytes - pos)
		return NULL;

	// Output is 3-byte BGR padded to a 4-byte boundary, like QOIWrapper.
	// width <= MAX_IMAGE_DIMENSION (65535), so width * 3 fits in int.
	size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
	size_t allocSize = 0;
	if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
		outOfMemory = true;
		return NULL;
	}
	unsigned char* pPixelData = new(std::nothrow) unsigned char[allocSize];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	// Zero the row padding so no uninitialised bytes reach the DIB.
	memset(pPixelData, 0, allocSize);
	// PBM: 1 bit per pixel, MSB first, 1 = white, 0 = black. Rows are
	// padded to a byte boundary; the padding bits are ignored.
	for (int y = 0; y < height; y++) {
		const unsigned char* pRow = pMagic + pos + (size_t)y * rowBytes;
		unsigned char* pDst = pPixelData + (size_t)y * padded_stride;
		for (int x = 0; x < width; x++) {
			unsigned char bit = (unsigned char)((pRow[x / 8] >> (7 - (x % 8))) & 1);
			unsigned char gray = bit ? 255 : 0;
			pDst[x * 3] = gray;
			pDst[x * 3 + 1] = gray;
			pDst[x * 3 + 2] = gray;
		}
	}
	return (void*)pPixelData;
}
