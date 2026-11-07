#include "stdafx.h"

#include "PPMWrapper.h"
// stb_image is vendored for PNM and HDR. JPEG/PNG/BMP/TGA/GIF/PIC
// are compiled out. HDR is deliberately NOT compiled out (B-1): the
// HDR decoder lives in this object and HDRWrapper.cpp calls it.
// PSD is deliberately NOT compiled out: stb_image
// v2.30 omits stbi__convert_format16 (and stbi__compute_y_16) when both
// STBI_NO_PNG and STBI_NO_PSD are defined, yet stbi__pnm_load still
// references convert_format16 for its 16-bit path, so defining both
// breaks the link even though our call never requests a channel
// conversion. Keeping the PSD parser in the object does not let PSD
// content through: the magic gate below rejects anything that is not
// binary P5/P6 before stb_image is called at all.
//
// stb_image PNM limits, all inherited here and documented rather than
// worked around except where noted: binary P5 (PGM) and P6 (PPM) only,
// ASCII P2/P3 and bitmap P1/P4 and PAM P7 are unsupported and rejected;
// 16-bit samples are big-endian by the PNM spec, but stb_image reads
// them as host-endian, so on little-endian Windows a spec-compliant
// 16-bit file would come out byte-swapped (0xFF00 -> 0 instead of
// 255). The wrapper therefore byte-swaps the 16-bit raster in a copy
// before calling stb_image, which restores the spec values; 8-bit
// files are passed through untouched.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_JPEG
#define STBI_NO_PNG
#define STBI_NO_BMP
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#include "stb/stb_image.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"

void* PPMReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 2)
		return NULL;
	// PPM-only gate, before stb_image ever sees the buffer: the magic
	// must be binary P5 or P6. This rejects P1..P4, P7 (PAM) and, more
	// importantly, any non-PNM content misnamed .ppm, which stb_image
	// would otherwise happily decode as JPEG/PNG/etc.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	if (pMagic[0] != 'P' || (pMagic[1] != '5' && pMagic[1] != '6'))
		return NULL;

	// 16-bit raster byte-swap (see header comment): parse the header
	// the way stb_image's stbi__pnm_info does (whitespace and '#'
	// comments between tokens, raster starting right after the single
	// delimiter byte following maxval), and if maxval > 255, swap each
	// sample pair in a copy. If parsing fails, pass the original buffer
	// through and let stb_image fail as it would have.
	const unsigned char* pLoadBuffer = pMagic;
	unsigned char* pSwappedBuffer = NULL;
	// F-07: header dimensions, filled in by the parse block below before
	// any decode runs, so a header claiming 65535x65535 is refused before
	// stb_image allocates a 13 GB raster for it. bHdrOk stays false when
	// the header is malformed; then stb_image fails on it as before.
	int nHdrWidth = 0;
	int nHdrHeight = 0;
	bool bHdrOk = false;
	{
		size_t pos = 0;
		unsigned char c = 0;
		bool bParseOk = true;
		// Mimic stb's stream: pos is the next byte to read.
		if (sizebytes < 2) bParseOk = false;
		else {
			pos = 2;
			if (pos >= (size_t)sizebytes) bParseOk = false;
			else c = pMagic[pos++];
		}
		// skip whitespace/comments, then read an integer, three times
		// (width, height, maxval). After each integer, c holds the
		// delimiter byte and pos is already past it, exactly as in stb.
		int nValues[3] = { 0, 0, 0 };
		for (int t = 0; t < 3 && bParseOk; t++) {
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
				if (v > 214748364 || (v == 214748364 && (c - '0') > 7)) { bParseOk = false; break; }
				v = v * 10 + (c - '0');
				if (pos >= (size_t)sizebytes) { bParseOk = false; break; }
				c = pMagic[pos++];
			}
			if (!bParseOk) break;
			nValues[t] = v;
		}
		if (bParseOk) {
			nHdrWidth = nValues[0];
			nHdrHeight = nValues[1];
			bHdrOk = true;
		}
		if (bParseOk && nValues[2] > 255 && nValues[2] <= 65535 && nValues[0] > 0 && nValues[1] > 0) {
			int nCh = (pMagic[1] == '6') ? 3 : 1;
			size_t nRasterBytes = (size_t)nValues[0] * (size_t)nValues[1] * (size_t)nCh * 2;
			if (pos + nRasterBytes <= (size_t)sizebytes) {
				pSwappedBuffer = new(std::nothrow) unsigned char[sizebytes];
				if (pSwappedBuffer != NULL) {
					memcpy(pSwappedBuffer, pMagic, sizebytes);
					for (size_t i = pos; i + 1 < pos + nRasterBytes; i += 2) {
						unsigned char tmp = pSwappedBuffer[i];
						pSwappedBuffer[i] = pSwappedBuffer[i + 1];
						pSwappedBuffer[i + 1] = tmp;
					}
					pLoadBuffer = pSwappedBuffer;
				}
			}
		}
	}

	// F-07 size gate, before stbi_load_from_memory: the only allocation so
	// far is a copy of the input file (bounded by the file size, not the
	// header). The byte gate is the one that bites for 65535x65535: the
	// pixel count alone sits exactly at the default pixel cap (256 MP).
	if (bHdrOk) {
		// Gate on the padded 3-byte BGR output stride (as HDRWrapper does):
		// it bounds both the stb raster and the output buffer, so even a
		// 1-channel P5 at 65535x65535 (4.29 GB raster, just under the byte
		// cap) is refused here instead of after a huge decode.
		if (nHdrWidth <= 0 || nHdrHeight <= 0 || nHdrWidth > MAX_IMAGE_DIMENSION || nHdrHeight > MAX_IMAGE_DIMENSION) {
			delete[] pSwappedBuffer;
			return NULL;
		}
		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		// No abs(): nHdrWidth/nHdrHeight are proven > 0 by the check above;
		// unqualified abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB).
		if ((double)nHdrWidth * (double)nHdrHeight > (double)CSettingsProvider::This().MaxImagePixels()) {
			isSizeLimitExceeded = true;
			delete[] pSwappedBuffer;
			return NULL;
		}
		size_t nHdrAllocSize = 0;
		size_t nHdrPaddedStride = (size_t)Helpers::DoPadding(nHdrWidth * 3, 4);
		if (!IsImageBufferSizeOk(nHdrPaddedStride, (size_t)nHdrHeight, &nHdrAllocSize)) {
			outOfMemory = true;
			delete[] pSwappedBuffer;
			return NULL;
		}
	}

	int nChannels = 0;
	unsigned char* pDecoded = stbi_load_from_memory(pLoadBuffer, sizebytes, &width, &height, &nChannels, 0);
	delete[] pSwappedBuffer;
	if (pDecoded == NULL)
		return NULL;
	// PNM yields 1 channel (PBM/PGM) or 3 channels (PPM) only. Anything
	// else means the content was not PNM after all; reject it rather
	// than passing an unexpected layout to CJPEGImage.
	if (nChannels != 1 && nChannels != 3) {
		stbi_image_free(pDecoded);
		return NULL;
	}
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): stbi_load success implies width/height > 0; unqualified
	// abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels())
		isSizeLimitExceeded = true;
	if (isSizeLimitExceeded || width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
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
			size_t srcIdx = ((size_t)y * (size_t)width + (size_t)x) * (size_t)nChannels;
			size_t dstIdx = (size_t)y * padded_stride + (size_t)x * 3;
			if (nChannels == 3) {
				// stb_image returns RGB; JPEGView pixels are BGR.
				pPixelData[dstIdx] = pDecoded[srcIdx + 2];
				pPixelData[dstIdx + 1] = pDecoded[srcIdx + 1];
				pPixelData[dstIdx + 2] = pDecoded[srcIdx];
			} else {
				unsigned char gray = pDecoded[srcIdx];
				pPixelData[dstIdx] = gray;
				pPixelData[dstIdx + 1] = gray;
				pPixelData[dstIdx + 2] = gray;
			}
		}
	}
	stbi_image_free(pDecoded);
	return (void*)pPixelData;
}
