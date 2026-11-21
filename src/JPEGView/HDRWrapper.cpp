#include "stdafx.h"

#include "HDRWrapper.h"
// stb_image is shared with PPMWrapper.cpp, which compiles the implementation
// (STB_IMAGE_IMPLEMENTATION) with JPEG/PNG/BMP/TGA/GIF/PIC compiled out.
// HDR must NOT be compiled out there (B-1 removed STBI_NO_HDR from
// PPMWrapper.cpp), or the calls below would fail: the HDR decoder lives
// in that same object. This file only uses the declarations, so it must
// not define STB_IMAGE_IMPLEMENTATION again.
#include "stb/stb_image.h"
#include <math.h> // pow (HDR gamma conversion)
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "HdrToneMap.h"

// Parses the GAMMA header line (e.g. "GAMMA=0.454545") from a Radiance
// header. Returns the value, or -1 when the header has no GAMMA line.
// The scan stops at the blank line / resolution line, so pixel data is
// never scanned; it is also bounded by the buffer size.
static double ParseHDRGamma(const unsigned char* pBuffer, int nSize) {
	int nPos = 0;
	while (nPos < nSize) {
		int nLineEnd = nPos;
		while (nLineEnd < nSize && pBuffer[nLineEnd] != '\n') nLineEnd++;
		int nLineLen = nLineEnd - nPos;
		if (nLineLen == 0) {
			break; // blank line: end of header
		}
		// The resolution line ("-Y 768 +X 1024") ends the header too.
		if (pBuffer[nPos] == '-' || pBuffer[nPos] == '+') {
			break;
		}
		if (nLineLen >= 6 && memcmp(pBuffer + nPos, "GAMMA=", 6) == 0) {
			// Copy the value into a small NUL-terminated buffer for strtod.
			char szValue[32];
			int nValueLen = nLineLen - 6;
			if (nValueLen >= (int)sizeof(szValue)) nValueLen = (int)sizeof(szValue) - 1;
			memcpy(szValue, pBuffer + nPos + 6, nValueLen);
			szValue[nValueLen] = 0;
			return strtod(szValue, NULL);
		}
		nPos = nLineEnd + 1;
	}
	return -1.0; // no GAMMA header
}

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
	// No abs(): stb only returns non-negative dimensions, and the check
	// below rejects width/height <= 0; unqualified abs(double) resolves
	// to ::abs(int), and a product over INT_MAX would narrow to a negative
	// int (abs(INT_MIN) is UB).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels())
		isSizeLimitExceeded = true;
	if (isSizeLimitExceeded || width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		return NULL;
	}
	// HDR is RGB only; anything else means the content was not HDR.
	if (nChannels != 3) {
		return NULL;
	}

	// Header-aware gamma: stb_image's 8-bit load converts the float HDR
	// data to 8-bit with a fixed gamma-2.2 tone mapping and ignores the
	// file's GAMMA header, so a file written for gamma 0.454545 came out
	// far too bright. Load floats instead and linearize here: GAMMA=g
	// means the stored values were encoded with exponent g (stored =
	// linear^g), so linear = stored^(1/g). g=0.454545 gives stored^2.2,
	// g=1.0 (or no GAMMA header) is a no-op. A missing or absurd value
	// falls back to g=1.0. The linearized floats are then tone-mapped
	// by HdrToneMap::ConvertLinearImage (BT.2390, linear 1.0 = SDR
	// white), which rolls highlights off instead of clipping them.
	double dGamma = ParseHDRGamma(pMagic, sizebytes);
	double dInvGamma = 1.0;
	// P2-C8: the lower bound is 0.25, the mirror of the upper bound
	// (1/4.0), not a Radiance spec value. A tiny but finite gamma
	// (e.g. 1e-300) would invert to 1e300 and blow the linearize
	// exponent up; below 0.25 it now falls back to g=1.0 like a
	// missing/absurd value. (1/DBL_MAX is ~5.56e-309, so overflow to
	// Inf needs dGamma below that; the 0.25 bound is far above it.)
	if (dGamma >= 0.25 && dGamma <= 4.0) {
		dInvGamma = 1.0 / dGamma;
	}
	float* pDecodedFloat = stbi_loadf_from_memory(pMagic, sizebytes, &width, &height, &nChannels, 0);
	if (pDecodedFloat == NULL)
		return NULL;
	if (nChannels != 3) {
		stbi_image_free(pDecodedFloat);
		return NULL;
	}
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		stbi_image_free(pDecodedFloat);
		return NULL;
	}
	// Output is 3-byte BGR padded to a 4-byte boundary, like QOIWrapper.
	// width <= MAX_IMAGE_DIMENSION (65535), so width * 3 fits in int.
	size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
	size_t allocSize = 0;
	if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
		outOfMemory = true;
		stbi_image_free(pDecodedFloat);
		return NULL;
	}
	unsigned char* pPixelData = new(std::nothrow) unsigned char[allocSize];
	if (pPixelData == NULL) {
		outOfMemory = true;
		stbi_image_free(pDecodedFloat);
		return NULL;
	}
	// Zero the row padding so no uninitialised bytes reach the DIB.
	memset(pPixelData, 0, allocSize);
	// Linearize in place (GAMMA=g: linear = stored^(1/g)). For the
	// common g=1.0 this is a no-op and is skipped. Negative/NaN stored
	// values become NaN here; ConvertLinearImage clamps them to 0.
	if (dInvGamma != 1.0) {
		size_t nFloats = (size_t)width * (size_t)height * 3;
		for (size_t i = 0; i < nFloats; i++) {
			pDecodedFloat[i] = (float)pow((double)pDecodedFloat[i], dInvGamma);
		}
	}
	// Tone-map the scene-linear floats to 8-bit sRGB BGR (BT.2390,
	// linear 1.0 = SDR white). stb_image returns RGB; the helper
	// writes BGR (JPEGView pixel order).
	HdrToneMap::ConvertLinearImage(pDecodedFloat, width, height, (size_t)width * 3, pPixelData, padded_stride);
	stbi_image_free(pDecodedFloat);
	return (void*)pPixelData;
}
