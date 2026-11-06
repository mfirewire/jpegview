#include "stdafx.h"

#include "PAMWrapper.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"

// A PAM header line as a (pointer, length) slice, without its line
// ending and without a trailing comment.
struct SPAMHeaderLine {
	const unsigned char* p; // first byte of the line content
	size_t n; // length in bytes, comment and line ending stripped
};

// Reads one header line starting at pos. The line must end with '\n'
// (a trailing '\r' of a CRLF ending is stripped); a '#' starts a
// comment that runs to the end of the line. Returns false when the
// buffer ends before a newline or the line exceeds 256 bytes: a
// header line that long is not a PAM header, and a file with no
// newline at all is a truncated header, not a raster.
static bool ReadPAMHeaderLine(const unsigned char* pBuf, size_t sizebytes, size_t& pos, SPAMHeaderLine& line) {
	if (pos >= sizebytes)
		return false;
	size_t eol = pos;
	while (eol < sizebytes && pBuf[eol] != '\n') {
		if (eol - pos > 256)
			return false;
		eol++;
	}
	if (eol >= sizebytes)
		return false;
	size_t len = eol - pos;
	if (len > 0 && pBuf[pos + len - 1] == '\r')
		len--;
	// Strip a '#' comment (PAM comments run to end of line, like PNM).
	for (size_t i = 0; i < len; i++) {
		if (pBuf[pos + i] == '#') {
			len = i;
			break;
		}
	}
	line.p = pBuf + pos;
	line.n = len;
	pos = eol + 1; // past '\n'; the raster starts right after ENDHDR's '\n'
	return true;
}

// Trims ASCII spaces/tabs at both ends of a line slice, in place.
static void TrimPAMLine(const unsigned char*& p, size_t& n) {
	while (n > 0 && (p[0] == ' ' || p[0] == '\t')) {
		p++;
		n--;
	}
	while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
		n--;
}

// Parses an all-digit decimal integer (no sign, no trailing junk)
// with the same overflow guard as the PBM/PPM header parsers
// (v * 10 + d overflows iff v > 214748364, or v == 214748364 and d > 7;
// the guard refuses exactly those, so INT_MAX itself still parses).
static bool ParsePAMInt(const unsigned char* p, size_t n, int& v) {
	if (n == 0)
		return false;
	v = 0;
	for (size_t i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return false;
		if (v > 214748364 || (v == 214748364 && (p[i] - '0') > 7))
			return false;
		v = v * 10 + (p[i] - '0');
	}
	return true;
}

void* PAMReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 2)
		return NULL;
	// P7-only gate: the magic must be "P7". This rejects P1..P6 and
	// any non-PAM content misnamed .pam.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	if (pMagic[0] != 'P' || pMagic[1] != '7')
		return NULL;

	// Header: "P7" line, then "KEY VALUE" lines, then "ENDHDR". The
	// first line must be exactly "P7" (after comment/blank trimming),
	// which also enforces the newline after the magic.
	size_t pos = 0;
	SPAMHeaderLine line = { NULL, 0 };
	if (!ReadPAMHeaderLine(pMagic, (size_t)sizebytes, pos, line))
		return NULL;
	{
		const unsigned char* p = line.p;
		size_t n = line.n;
		TrimPAMLine(p, n);
		if (n != 2 || p[0] != 'P' || p[1] != '7')
			return NULL;
	}

	int nWidth = 0, nHeight = 0, nDepth = 0, nMaxval = 0;
	bool bSeenWidth = false, bSeenHeight = false, bSeenDepth = false, bSeenMaxval = false;
	bool bSeenTupltype = false;
	// TUPLTYPE value, kept as a slice into the input buffer: "RGB" or
	// "RGB_ALPHA". It is only compared, never copied.
	const unsigned char* pTupltype = NULL;
	size_t nTupltype = 0;
	bool bParseOk = true;
	bool bEndHdr = false;
	while (bParseOk && !bEndHdr) {
		if (!ReadPAMHeaderLine(pMagic, (size_t)sizebytes, pos, line)) {
			bParseOk = false;
			break;
		}
		const unsigned char* p = line.p;
		size_t n = line.n;
		TrimPAMLine(p, n);
		if (n == 0)
			continue; // blank line or comment-only line
		// ENDHDR has no value; it must be the whole line.
		if (n == 6 && memcmp(p, "ENDHDR", 6) == 0) {
			bEndHdr = true;
			break;
		}
		// Split into KEY and VALUE at the first whitespace run.
		size_t keyLen = 0;
		while (keyLen < n && p[keyLen] != ' ' && p[keyLen] != '\t')
			keyLen++;
		const unsigned char* pValue = p + keyLen;
		size_t nValue = n - keyLen;
		TrimPAMLine(pValue, nValue);
		if (nValue == 0) {
			bParseOk = false; // a key line without a value
			break;
		}
		if (keyLen == 5 && memcmp(p, "WIDTH", 5) == 0) {
			if (bSeenWidth || !ParsePAMInt(pValue, nValue, nWidth)) bParseOk = false;
			bSeenWidth = true;
		} else if (keyLen == 6 && memcmp(p, "HEIGHT", 6) == 0) {
			if (bSeenHeight || !ParsePAMInt(pValue, nValue, nHeight)) bParseOk = false;
			bSeenHeight = true;
		} else if (keyLen == 5 && memcmp(p, "DEPTH", 5) == 0) {
			if (bSeenDepth || !ParsePAMInt(pValue, nValue, nDepth)) bParseOk = false;
			bSeenDepth = true;
		} else if (keyLen == 6 && memcmp(p, "MAXVAL", 6) == 0) {
			if (bSeenMaxval || !ParsePAMInt(pValue, nValue, nMaxval)) bParseOk = false;
			bSeenMaxval = true;
		} else if (keyLen == 8 && memcmp(p, "TUPLTYPE", 8) == 0) {
			// A TUPLTYPE value is a single token (no whitespace inside);
			// anything else is not a tupltype this reader knows.
			bool bSingleToken = true;
			for (size_t i = 0; i < nValue; i++) {
				if (pValue[i] == ' ' || pValue[i] == '\t') {
					bSingleToken = false;
					break;
				}
			}
			if (bSeenTupltype || !bSingleToken) bParseOk = false;
			bSeenTupltype = true;
			pTupltype = pValue;
			nTupltype = nValue;
		} else {
			bParseOk = false; // unknown header key: fail closed
		}
	}
	if (!bParseOk || !bEndHdr)
		return NULL;
	if (!bSeenWidth || !bSeenHeight || !bSeenDepth || !bSeenMaxval)
		return NULL;
	// Only the sample-proven combinations: DEPTH 3 with TUPLTYPE RGB,
	// DEPTH 4 with TUPLTYPE RGB_ALPHA, MAXVAL 255 (8-bit samples).
	// 16-bit MAXVAL, DEPTH 1/2 (grayscale) and any other TUPLTYPE are
	// rejected, not guessed. A missing TUPLTYPE is assumed to match
	// the depth (RGB for 3, RGB_ALPHA for 4), as the sample's sibling
	// files do.
	if (nDepth != 3 && nDepth != 4)
		return NULL;
	if (nMaxval != 255)
		return NULL;
	if (bSeenTupltype) {
		if (nDepth == 3 && (nTupltype != 3 || memcmp(pTupltype, "RGB", 3) != 0))
			return NULL;
		if (nDepth == 4 && (nTupltype != 9 || memcmp(pTupltype, "RGB_ALPHA", 9) != 0))
			return NULL;
	}
	width = nWidth;
	height = nHeight;
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): width/height are proven > 0 above, and unqualified abs(double)
	// resolves to ::abs(int) - a product over INT_MAX narrows to a negative int
	// and abs(INT_MIN) is UB (caught by UBSan on the 65535x65535 header).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels())
		isSizeLimitExceeded = true;
	if (isSizeLimitExceeded)
		return NULL;

	// Size gate: the raster is width * DEPTH bytes per row, starting
	// right after the ENDHDR line. The file must actually hold that
	// many bytes; otherwise it is truncated.
	size_t rowBytes = (size_t)width * (size_t)nDepth;
	size_t rasterBytes = rowBytes * (size_t)height;
	if (pos > (size_t)sizebytes || rasterBytes > (size_t)sizebytes - pos)
		return NULL;

	// Output is 3-byte BGR padded to a 4-byte boundary, like PBMWrapper.
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
	// PAM raster is RGB (DEPTH 3) or RGBA (DEPTH 4), top row first;
	// alpha is dropped. JPEGView pixels are BGR.
	for (int y = 0; y < height; y++) {
		const unsigned char* pRow = pMagic + pos + (size_t)y * rowBytes;
		unsigned char* pDst = pPixelData + (size_t)y * padded_stride;
		for (int x = 0; x < width; x++) {
			const unsigned char* pSrc = pRow + (size_t)x * (size_t)nDepth;
			pDst[x * 3] = pSrc[2];
			pDst[x * 3 + 1] = pSrc[1];
			pDst[x * 3 + 2] = pSrc[0];
		}
	}
	return (void*)pPixelData;
}
