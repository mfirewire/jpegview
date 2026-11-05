#pragma once

#include <stddef.h>

// Sizes are in bytes

#ifdef _WIN64
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_JXL_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_JXL_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_HEIF_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_HEIF_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_PSD_FILE_SIZE = 1024 * 1024 * 500;
#else
const unsigned int MAX_PSD_FILE_SIZE = 1024 * 1024 * 100;
#endif

#ifdef _WIN64
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 500;
#else
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 100;
#endif

// P2-5: the pixel cap. It used to be 65535*65535 (4.29e9) on x64, which is
// effectively no cap: a 40000x30000 JPEG passed every header check and then
// sat in the UI wait for minutes while the decoder chewed on it. The default
// is now 100 megapixels on both architectures (the old Win32 value). It is
// configurable with the MaxImagePixelsMP INI setting (megapixels, default
// 100); CSettingsProvider::MaxImagePixels() returns the effective cap and
// the load paths that know the setting use that, while this constant stays
// the default for the decoders that only see this header. MAX_IMAGE_BYTES
// below stays the hard byte cap whatever the pixel cap is set to.
#ifdef _WIN64
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#else
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#endif

// That must not be bigger than 65535 due to internal limitations
//
// unbounding (>65535) this causes crashes if HighQualityResampling=true
// but if it's false, some images load (so far png tested was corrupted)
const unsigned int MAX_IMAGE_DIMENSION = 65535;

// 分配前的字节数上限。单边 65535 是允许的，但 width*height*channels 在 32 位
// int 里会回绕——例如 65535x21846x3 = 4.29GB 回绕成 128KB，分配过小之后解码器
// 仍按真实尺寸逐行写入，就是堆溢出。所有解码器都必须用下面的 helper 算缓冲大小。
#ifdef _WIN64
const size_t MAX_IMAGE_BYTES = (size_t)4 * 1024 * 1024 * 1024 - 1; // 低于 2^32，任何 32 位回绕都会被拦下
#else
const size_t MAX_IMAGE_BYTES = (size_t)1536 * 1024 * 1024;         // 32 位进程地址空间下留余量
#endif

// stride 必须已经用 size_t 算好（例如 (size_t)width * channels），否则乘法本身
// 可能已经回绕。返回 false 表示尺寸不可信或超出上限，调用方应报“内存不足”。
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