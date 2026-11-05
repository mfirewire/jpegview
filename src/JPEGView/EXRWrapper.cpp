#include "stdafx.h"

#include "EXRWrapper.h"
// tinyexr, vendored under tinyexr/ (upstream syoyo/tinyexr master
// snapshot, 2026-11-06; sha256 of tinyexr.h:
// b1d33ca1350d64e2b9a199614991cffd45d7982ff3d68f107020a1230bde38ec).
// The header has no version macro; the snapshot is identified by that hash.
// NOTE: current tinyexr master is NOT a single header: tinyexr.h
// unconditionally includes "exr_reader.hh" (which includes
// "streamreader.hh") and <miniz.h> for ZIP (TINYEXR_USE_MINIZ, default 1).
// All three headers plus miniz (miniz.h/.c, miniz_tdef/tinfl .c/.h,
// miniz_common.h; upstream richgel999/miniz master) are vendored under
// tinyexr/, and the miniz .c files are compiled by the vcxproj.
// TINYEXR_IMPLEMENTATION must be defined in exactly this translation unit.
#define TINYEXR_IMPLEMENTATION
#include "tinyexr/tinyexr.h"
#include <math.h>
#include "MaxImageDef.h"
#include "Helpers.h"
#include <new>

void* EXRReader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 4)
		return NULL;
	// EXR-only gate, before tinyexr ever sees the buffer: the magic must
	// be 0x76 0x2F 0x31 0x01. This rejects any non-EXR content misnamed
	// .exr.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	if (pMagic[0] != 0x76 || pMagic[1] != 0x2F || pMagic[2] != 0x31 || pMagic[3] != 0x01)
		return NULL;

	// Header first: version, then the single-part header. The dimensions
	// come from the data window, and the compression field is checked
	// before any pixel data is touched.
	EXRVersion version;
	if (ParseEXRVersionFromMemory(&version, pMagic, (size_t)sizebytes) != TINYEXR_SUCCESS)
		return NULL;
	if (version.multipart != 0) {
		// Multi-part EXR: the simple LoadEXRFromMemory path below only
		// handles single-part files, so reject rather than half-decode.
		return NULL;
	}
	EXRHeader header;
	InitEXRHeader(&header);
	const char* err = NULL;
	if (ParseEXRHeaderFromMemory(&header, &version, pMagic, (size_t)sizebytes, &err) != TINYEXR_SUCCESS) {
		if (err != NULL)
			FreeEXRErrorMessage(err);
		FreeEXRHeader(&header);
		return NULL;
	}
	width = header.data_window.max_x - header.data_window.min_x + 1;
	height = header.data_window.max_y - header.data_window.min_y + 1;
	// Size gate BEFORE decode (F-06/F-07 lesson): refuse absurd dimensions
	// before tinyexr allocates a decode buffer for them.
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		FreeEXRHeader(&header);
		return NULL;
	}
	if (abs((double)width * height) > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		FreeEXRHeader(&header);
		return NULL;
	}
	// Compression gate, checked before decoding: DWAA/DWAB are not
	// supported by tinyexr ("Not yet supported" in tinyexr.h), and ZFP is
	// compiled out by default (TINYEXR_USE_ZFP is 0), so files using them
	// fail cleanly here instead of inside the decoder.
	// (B44/B44A need no gate: this tinyexr snapshot ships a B44
	// decompressor wired into DecodePixelData.)
	if (header.compression_type == TINYEXR_COMPRESSIONTYPE_DWAA ||
		header.compression_type == TINYEXR_COMPRESSIONTYPE_DWAB ||
		header.compression_type == TINYEXR_COMPRESSIONTYPE_ZFP) {
		FreeEXRHeader(&header);
		return NULL;
	}
	// Whether the file has a real alpha channel. LoadEXRFromMemory fills
	// A with 1.0 when there is no A channel (RGB), but for a single
	// grayscale channel it copies the gray value into A as well -- so the
	// alpha composite below must only run when an "A" channel exists,
	// otherwise every gray pixel would be treated as semi-transparent.
	bool bHasAlpha = false;
	for (int c = 0; c < header.num_channels; c++) {
		if (strcmp(header.channels[c].name, "A") == 0) {
			bHasAlpha = true;
			break;
		}
	}
	FreeEXRHeader(&header);

	// LoadEXRFromMemory returns interleaved RGBA float (half channels are
	// read as float; a single gray channel is replicated to RGBA; tiled
	// images are reconstructed). It re-parses the header internally.
	float* pRGBA = NULL;
	int nWidth = 0, nHeight = 0;
	err = NULL;
	if (LoadEXRFromMemory(&pRGBA, &nWidth, &nHeight, pMagic, (size_t)sizebytes, &err) != TINYEXR_SUCCESS || pRGBA == NULL) {
		if (err != NULL)
			FreeEXRErrorMessage(err);
		if (pRGBA != NULL)
			free(pRGBA);
		return NULL;
	}
	// The decoded size must agree with the gated header size; if it does
	// not, the file was inconsistent and is rejected rather than trusted.
	if (nWidth != width || nHeight != height || nWidth <= 0 || nHeight <= 0) {
		free(pRGBA);
		return NULL;
	}

	// Output is 3-byte BGR padded to a 4-byte boundary, like QOIWrapper.
	// width <= MAX_IMAGE_DIMENSION (65535), so width * 3 fits in int.
	size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
	size_t allocSize = 0;
	if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
		outOfMemory = true;
		free(pRGBA);
		return NULL;
	}
	unsigned char* pPixelData = new(std::nothrow) unsigned char[allocSize];
	if (pPixelData == NULL) {
		outOfMemory = true;
		free(pRGBA);
		return NULL;
	}
	// Zero the row padding so no uninitialised bytes reach the DIB.
	memset(pPixelData, 0, allocSize);
	// Tone map: scene-linear float -> 8-bit with gamma 2.2, the same
	// choice as the HDR wrapper's stb_image path (HDRWrapper.cpp). Values
	// above 1.0 clip to white; there is no exposure control in this pass.
	// Alpha is composited over white in linear space first (white = 1.0),
	// so a transparent pixel shows as white, not black.
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			size_t srcIdx = ((size_t)y * (size_t)width + (size_t)x) * 4;
			size_t dstIdx = (size_t)y * padded_stride + (size_t)x * 3;
			float r = pRGBA[srcIdx];
			float g = pRGBA[srcIdx + 1];
			float b = pRGBA[srcIdx + 2];
			// Only composite when the file has a real A channel (see the
			// bHasAlpha note above); otherwise the pixel is opaque.
			float a = bHasAlpha ? pRGBA[srcIdx + 3] : 1.0f;
			if (a < 0.0f) a = 0.0f;
			if (a > 1.0f) a = 1.0f;
			// Composite over white, then gamma-map each channel.
			float rw = r * a + (1.0f - a);
			float gw = g * a + (1.0f - a);
			float bw = b * a + (1.0f - a);
			if (rw < 0.0f) rw = 0.0f;
			if (gw < 0.0f) gw = 0.0f;
			if (bw < 0.0f) bw = 0.0f;
			// powf(..., 1/2.2): 0 -> 0, 1 -> 255, >1 clamps to 255 below.
			float rf = powf(rw, 1.0f / 2.2f) * 255.0f;
			float gf = powf(gw, 1.0f / 2.2f) * 255.0f;
			float bf = powf(bw, 1.0f / 2.2f) * 255.0f;
			if (rf > 255.0f) rf = 255.0f;
			if (gf > 255.0f) gf = 255.0f;
			if (bf > 255.0f) bf = 255.0f;
			// JPEGView pixels are BGR.
			pPixelData[dstIdx] = (unsigned char)(bf + 0.5f);
			pPixelData[dstIdx + 1] = (unsigned char)(gf + 0.5f);
			pPixelData[dstIdx + 2] = (unsigned char)(rf + 0.5f);
		}
	}
	free(pRGBA);
	return (void*)pPixelData;
}
