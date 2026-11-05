#pragma once

#include <stddef.h>

#ifdef _WIN64
const unsigned int MAX_IMAGE_PIXELS = 65535 * 65535;
#else
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#endif

// That must not be bigger than 65535 due to internal limitations
const unsigned int MAX_IMAGE_DIMENSION = 65535;

// F-15: byte cap before allocating. The pixel cap alone does not bound
// the buffer: e.g. 40000x30000 passes the pixel check, but stride*height
// in 32-bit UINT wraps (4.8 GB -> ~0.5 GB) and CopyPixels would then write
// past the buffer. Mirrors src/JPEGView/MaxImageDef.h.
#ifdef _WIN64
const size_t MAX_IMAGE_BYTES = (size_t)4 * 1024 * 1024 * 1024 - 1; // below 2^32, any 32-bit wrap is caught
#else
const size_t MAX_IMAGE_BYTES = (size_t)1536 * 1024 * 1024;
#endif

// stride must already be computed in size_t (e.g. (size_t)width * channels),
// otherwise the multiplication itself may already have wrapped.
inline bool IsImageBufferSizeOk(size_t stride, size_t height, size_t* pBytes = NULL) {
	if (stride == 0 || height == 0) {
		return false;
	}
	if (stride > (size_t)-1 / height) {
		return false;
	}
	const size_t bytes = stride * height;
	if (bytes > MAX_IMAGE_BYTES) {
		return false;
	}
	if (pBytes != NULL) {
		*pBytes = bytes;
	}
	return true;
}
