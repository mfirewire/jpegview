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

// this may be an artificial limitation and might make configurable, or ignore custom setting only for win32
#ifdef _WIN64
const unsigned int MAX_IMAGE_PIXELS = 65535 * 65535;
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