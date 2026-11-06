#include "stdafx.h"

#include "PNGWrapper.h"

#ifndef WINXP
#include "png.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"
#include <stdexcept>
#include <new>

// Uncomment to build without APNG support
//#undef PNG_APNG_SUPPORTED

/*
 * Modified from "load4apng.c"
 * Original: https://sourceforge.net/projects/apng/files/libpng/examples/
 *
 * loads APNG from memory one frame at a time (32bpp), restarts when done
 * including frames composition.
 *
 * needs apng-patched libpng.
 *
 * Copyright (c) 2012-2014 Max Stepin
 * maxst at users.sourceforge.net
 *
 * zlib license
 * ------------
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 */

#ifdef PNG_APNG_SUPPORTED
void BlendOver(unsigned char** rows_dst, unsigned char** rows_src, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
{
	unsigned int  i, j;
	int u, v, al;

	for (j = 0; j < h; j++)
	{
		unsigned char* sp = rows_src[j];
		unsigned char* dp = rows_dst[j + y] + x * 4;

		for (i = 0; i < w; i++, sp += 4, dp += 4)
		{
			if (sp[3] == 255)
				memcpy(dp, sp, 4);
			else if (sp[3] != 0)
			{
				if (dp[3] != 0)
				{
					u = sp[3] * 255;
					v = (255 - sp[3]) * dp[3];
					al = u + v;
					dp[0] = (sp[0] * u + dp[0] * v) / al;
					dp[1] = (sp[1] * u + dp[1] * v) / al;
					dp[2] = (sp[2] * u + dp[2] * v) / al;
					dp[3] = al / 255;
				}
				else
					memcpy(dp, sp, 4);
			}
		}
	}
}
#endif

// libpng's read callback: a plain function, so it recovers the reader from the
// io_ptr (see BeginReading) and reads through that thread's cache.
void PngReader::ReadDataCallback(png_structp png_ptr, png_bytep outbuffer, png_size_t sizebytes)
{
	PngReader* pThis = (PngReader*)png_get_io_ptr(png_ptr);
	if (pThis == NULL)
		png_error(png_ptr, "png_get_io_ptr returned NULL");
	else if (pThis->cache.buffer_offset + sizebytes > pThis->cache.buffer_size)
		png_error(png_ptr, "Attempted to read out of bounds");
	else
	{
		memcpy(outbuffer, (char*)pThis->cache.buffer + pThis->cache.buffer_offset, sizebytes);
		pThis->cache.buffer_offset += sizebytes;
	}
}

// The libpng calls that can raise a png_error live here so that setjmp and the
// call that longjmps are in the same stack frame. Before this the setjmp sat in
// BeginReading, which had already returned by the time png_read_image ran, so a
// corrupt frame jumped back into a dead stack frame (undefined behaviour).
bool PngReader::ReadFrameData()
{
	if (setjmp(png_jmpbuf(cache.png_ptr)))
	{
		return false;
	}
#ifdef PNG_APNG_SUPPORTED
	if (png_get_valid(cache.png_ptr, cache.info_ptr, PNG_INFO_acTL))
	{
		png_read_frame_head(cache.png_ptr, cache.info_ptr);
		png_get_next_frame_fcTL(cache.png_ptr, cache.info_ptr, &cache.w0, &cache.h0, &cache.x0, &cache.y0, &cache.delay_num, &cache.delay_den, &cache.dop, &cache.bop);
	}
	if (cache.frame_index == cache.first)
	{
		cache.bop = PNG_BLEND_OP_SOURCE;
		if (cache.dop == PNG_DISPOSE_OP_PREVIOUS)
			cache.dop = PNG_DISPOSE_OP_BACKGROUND;
	}
#endif
	png_read_image(cache.png_ptr, cache.rows_frame);
	return true;
}

void* PngReader::ReadNextFrame(void** exif_chunk, png_uint_32* exif_size)
{
	unsigned int j;
	if (exif_chunk != NULL && exif_size != NULL) {
		png_get_eXIf_1(cache.png_ptr, cache.info_ptr, exif_size, (png_bytep*)exif_chunk);
	}
	if (!ReadFrameData())
	{
		// A corrupt or truncated frame. The cache stays consistent enough for the
		// caller to close the file; the next BeginReading resets it.
		return NULL;
	}

#ifdef PNG_APNG_SUPPORTED
	if (cache.dop == PNG_DISPOSE_OP_PREVIOUS)
		memcpy(cache.p_temp, cache.p_image, cache.size);

	if (cache.bop == PNG_BLEND_OP_OVER)
		BlendOver(cache.rows_image, cache.rows_frame, cache.x0, cache.y0, cache.w0, cache.h0);
	else
#endif
		for (j = 0; j < cache.h0; j++)
			memcpy(cache.rows_image[j + cache.y0] + cache.x0 * 4, cache.rows_frame[j], cache.w0 * 4);

	void* pixels = NULL;
	{
		// cache.width * cache.height * cache.channels is unsigned int arithmetic and
		// wraps for large APNG canvases; the copy loops below then write the real
		// size. Compute in size_t and reject anything that wrapped or exceeds the cap.
		size_t pixelsSize = 0;
		if (!IsImageBufferSizeOk((size_t)cache.width * cache.channels, (size_t)cache.height, &pixelsSize))
			return NULL;
		// CJPEGImage takes ownership and releases it with delete[]; the buffer
		// used to come from malloc, which is a mismatch the release CRT happens
		// to tolerate and the debug CRT asserts on.
		pixels = new(std::nothrow) unsigned char[pixelsSize];
	}
	if (pixels == NULL)
		return NULL;
	for (j = 0; j < cache.height; j++)
		memcpy((char*)pixels + j * cache.width * cache.channels, cache.rows_image[j], cache.width * cache.channels);

#ifdef PNG_APNG_SUPPORTED
	if (cache.dop == PNG_DISPOSE_OP_PREVIOUS)
		memcpy(cache.p_image, cache.p_temp, cache.size);
	else
		if (cache.dop == PNG_DISPOSE_OP_BACKGROUND)
			for (j = 0; j < cache.h0; j++)
				memset(cache.rows_image[j + cache.y0] + cache.x0 * 4, 0, cache.w0 * 4);
#endif
	cache.frame_index++;
	cache.frame_index %= cache.frame_count;
	if (cache.icc_transform != NULL) {
		// BGRA, rows tightly packed, which is what lcms2 writes as well
		ICCProfileTransform::DoTransform(cache.icc_transform, pixels, pixels,
			cache.width, cache.height, cache.width * cache.channels);
	}
	return pixels;
}

bool PngReader::BeginReading(void* buffer, size_t sizebytes, bool& outOfMemory, bool& isSizeLimitExceeded)
{
	unsigned int    width, height, channels, rowbytes, j;
	size_t          size;
	png_bytepp      rows_image;
	png_bytepp      rows_frame;
	unsigned char*  p_image;
	unsigned char*  p_frame;
	unsigned char*  p_temp;


	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop   info_ptr = png_create_info_struct(png_ptr);

	if (png_ptr && info_ptr)
	{
		// don't cache ptrs until image is valid or else causes a double-free later when destroying object in DeleteCacheInternal()

		if (setjmp(png_jmpbuf(png_ptr)))
		{
			// This frame's png_ptr/info_ptr are not stored into cache until much
			// further down, so DeleteCache would free the previous file's
			// structures and leak the pair created for this one.
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			throw std::runtime_error("Image contains errors.");
		}
		// skip png signature since we already checked it
		png_set_sig_bytes(png_ptr, 8);
		// custom read function so we can read from memory
		// The compressed bytes live in cache.buffer; the callback reaches them
		// (and the read offset) through the reader passed as io_ptr.
		png_set_read_fn(png_ptr, this, ReadDataCallback);
		png_read_info(png_ptr, info_ptr);
		png_set_expand(png_ptr);
		png_set_strip_16(png_ptr);
		png_set_gray_to_rgb(png_ptr);
		png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
		png_set_bgr(png_ptr);

		// gAMA / sRGB chunks used to be ignored here: the libpng path displayed the
		// raw values as sRGB while the GDI+ path applied the gamma, so the same file
		// looked different depending on the route. Apply the file gamma unless an
		// iCCP profile is present, in which case lcms2 handles the conversion.
		{
			double dFileGamma = 0.0;
			const bool bHasSRGB = png_get_valid(png_ptr, info_ptr, PNG_INFO_sRGB) != 0;
			const bool bHasICCP = png_get_valid(png_ptr, info_ptr, PNG_INFO_iCCP) != 0;
			const bool bHasGAMA = png_get_valid(png_ptr, info_ptr, PNG_INFO_gAMA) != 0;
			if (bHasGAMA) {
				png_get_gAMA(png_ptr, info_ptr, &dFileGamma);
			}
			if (!bHasICCP && (bHasSRGB || (bHasGAMA && dFileGamma > 0.0))) {
				// Screen gamma 2.2 approximates sRGB; an sRGB chunk means the file
				// gamma is 1/2.2 regardless of what gAMA says.
				png_set_gamma(png_ptr, 2.2, bHasSRGB ? 0.45455 : dFileGamma);
			}
		}

		(void)png_set_interlace_handling(png_ptr);
		png_read_update_info(png_ptr, info_ptr);
		width = png_get_image_width(png_ptr, info_ptr);
		height = png_get_image_height(png_ptr, info_ptr);
		if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			return false;
		}
		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		// No abs(): width/height are unsigned int; unqualified abs(double)
		// resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
		if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			isSizeLimitExceeded = true;
			return false;
		}
		channels = png_get_channels(png_ptr, info_ptr);
		if (channels != 4)
		{
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			return false;
		}
		rowbytes = (unsigned int)png_get_rowbytes(png_ptr, info_ptr);
		// height * rowbytes used to be multiplied as unsigned int: 65535 x 262140
		// wraps to 1.43GB while png_read_image still writes 5.73GB.
		if (!IsImageBufferSizeOk((size_t)rowbytes, (size_t)height, &size)) {
			png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
			outOfMemory = true;
			return false;
		}
		p_image = (unsigned char*)malloc(size);
		p_frame = (unsigned char*)malloc(size);
		p_temp = (unsigned char*)malloc(size);
		rows_image = (png_bytepp)malloc(height * sizeof(png_bytep));
		rows_frame = (png_bytepp)malloc(height * sizeof(png_bytep));
		if (p_image && p_frame && p_temp && rows_image && rows_frame)
		{
			png_uint_32     frames = 1;
			png_uint_32     x0 = 0;
			png_uint_32     y0 = 0;
			png_uint_32     w0 = width;
			png_uint_32     h0 = height;
#ifdef PNG_APNG_SUPPORTED
			png_uint_32     plays = 0;
			unsigned short  delay_num = 1;
			unsigned short  delay_den = 10;
			unsigned char   dop = 0;
			unsigned char   bop = 0;
			unsigned int    first = (png_get_first_frame_is_hidden(png_ptr, info_ptr) != 0) ? 1 : 0;
			if (png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL))
				png_get_acTL(png_ptr, info_ptr, &frames, &plays);
#endif
			for (j = 0; j < height; j++)
				rows_image[j] = p_image + j * rowbytes;

			for (j = 0; j < height; j++)
				rows_frame[j] = p_frame + j * rowbytes;

#ifdef PNG_APNG_SUPPORTED
			cache.bop = bop;
			// cache.plays = plays
			cache.delay_den = delay_den;
			cache.delay_num = delay_num;
			cache.dop = dop;
			cache.first = first;
#endif
			cache.channels = channels;
			cache.h0 = h0;
			cache.height = height;
			// cache ptrs here, only if valid
			cache.info_ptr = info_ptr;
			cache.png_ptr = png_ptr;
			cache.p_image = p_image;
			cache.p_frame = p_frame;
			cache.p_temp = p_temp;
			cache.rows_frame = rows_frame;
			cache.rows_image = rows_image;
			cache.size = size;
			cache.w0 = w0;
			cache.width = width;
			cache.x0 = x0;
			cache.y0 = y0;
			cache.frame_count = frames;
			if (frames > 0) {
#ifdef PNG_iCCP_SUPPORTED
				// libpng has already inflated the iCCP chunk by the time we get
				// here, so this is the profile itself, not its deflate stream.
				if (CSettingsProvider::This().UseEmbeddedColorProfiles()) {
					png_charp profile_name = NULL;
					int compression = 0;
					png_bytep profile = NULL;
					png_uint_32 profile_len = 0;
					if (png_get_iCCP(png_ptr, info_ptr, &profile_name, &compression, &profile, &profile_len) == PNG_INFO_iCCP
						&& profile != NULL && profile_len > 0
						&& !ICCProfileTransform::IsNoTransformNeeded(profile, profile_len)) {
						cache.icc_transform = ICCProfileTransform::CreateTransform(profile, profile_len, ICCProfileTransform::FORMAT_BGRA);
					}
				}
#endif
				return true;
			}
			return false;
		}
		free(p_image);
		free(rows_image);
		free(rows_frame);
		free(p_frame);
		free(p_temp);
	}
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	return false;
}

void* PngReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	void* buffer,
	size_t sizebytes)
{
	exif_chunk = NULL;
	if (!cache.buffer) {
		if (sizebytes < 8)
			return NULL;
		cache.buffer = malloc(sizebytes-8);
		if (!cache.buffer)
			return NULL;
		// copy everything except the PNG signature (first 8 bytes)
		memcpy(cache.buffer, (char*)buffer+8, sizebytes-8);
		cache.buffer_size = sizebytes-8;
	}
	buffer = cache.buffer;
	sizebytes = cache.buffer_size;
	if (!cache.png_ptr || cache.frame_index == 0) {
		DeleteCacheInternal(false);
		if (!buffer || !BeginReading(buffer, sizebytes, outOfMemory, isSizeLimitExceeded)) {
			return NULL;
		}

	}

	void* exif = NULL;
	unsigned int exif_size = 0;
	bool read_two = cache.frame_index < cache.first;
	void* pixels = ReadNextFrame(&exif, &exif_size);
	if (pixels && read_two) {
		delete[] (unsigned char*)pixels; // hidden first frame is skipped; release it before reading the next
		pixels = ReadNextFrame(&exif, &exif_size);
	}
	
	width = cache.width;
	height = cache.height;
	nchannels = cache.channels;
	has_animation = (cache.frame_count > 1);
	frame_count = cache.frame_count;

	// https://wiki.mozilla.org/APNG_Specification
	// "If the denominator is 0, it is to be treated as if it were 100"
	if (!cache.delay_den)
		cache.delay_den = 100;
	frame_time = (int)(1000.0 * cache.delay_num / cache.delay_den);

	if (exif_size > 8 && exif_size < 65528 && exif != NULL) {
		exif_chunk = malloc(exif_size + 10);
		if (exif_chunk != NULL) {
			memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
			*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif_size + 8);
			memcpy((uint8_t*)exif_chunk + 10, exif, exif_size);
		}
		
	}
	if (!has_animation)
		DeleteCache();
	return pixels;
}

void PngReader::DeleteCacheInternal(bool free_buffer)
{
	// png_read_end(cache.png_ptr, cache.info_ptr);
	free(cache.rows_frame);
	free(cache.rows_image);
	free(cache.p_temp);
	free(cache.p_frame);
	free(cache.p_image);
	if (cache.icc_transform != NULL) {
		ICCProfileTransform::DeleteTransform(cache.icc_transform);
	}
	png_destroy_read_struct(&cache.png_ptr, &cache.info_ptr, NULL);
	void* temp_buffer = cache.buffer;
	size_t temp_buffer_size = cache.buffer_size;
	cache = { 0 };
	if (free_buffer) {
		free(temp_buffer);
	} else {
		cache.buffer = temp_buffer;
		cache.buffer_size = temp_buffer_size;
	}
}

void PngReader::DeleteCache() {
	DeleteCacheInternal(true);
}

bool PngReader::MustUseLibpng(const void* bufferIn, size_t sizebytes) {
	const char* buffer = (const char*)bufferIn;

	// GDI+ fails if the uncompressed image size is over INT_MAX
	if (sizebytes < 24) {
		return false;
	}
	unsigned int width = _byteswap_ulong(*(unsigned int*)(buffer + 16));
	unsigned int height = _byteswap_ulong(*(unsigned int*)(buffer + 20));
	if (4.0 * width * height > INT_MAX) {
		return true;
	}

	// GDI+ does not support APNG
	// Valid APNGs must have an acTL chunk before the first IDAT chunk, this lets us quickly determine if a PNG is animated
	size_t offset = 8; // skip PNG signature
	while (offset + 7 < sizebytes) {
		if (memcmp(buffer + offset + 4, "acTL", 4) == 0)
			return true;
		if (memcmp(buffer + offset + 4, "IDAT", 4) == 0)
			return false;
		unsigned int chunksize = *(unsigned int*)(buffer + offset);

		// PNG chunk sizes are big-endian and must be converted to little-endian
		chunksize = _byteswap_ulong(chunksize);

		// Prevent infinite loop
		if (chunksize > PNG_UINT_31_MAX) return false;

		// 12 comes from 4 bytes for chunk size, 4 for chunk name, and 4 for CRC32
		offset += chunksize + 12;
	}
	return false;
}
#else
#define PNG_UINT_31_MAX (0x7fffffff)
#endif

void* PngReader::GetEXIFBlock(void* buffer, size_t sizebytes) {
	size_t offset = 8; // skip PNG signature
	while (offset + 7 < sizebytes) {
		unsigned int chunksize = *(unsigned int*)((char*)buffer + offset);
		// PNG chunk sizes are big-endian and must be converted to little-endian
		chunksize = _byteswap_ulong(chunksize);

		if (memcmp((char*)buffer + offset + 4, "eXIf", 4) == 0 && chunksize < 65528 && offset + chunksize + 7 < sizebytes) {
			void* exif_chunk = malloc(chunksize + 10);
			if (exif_chunk != NULL) {
				memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
				*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(chunksize + 8);
				memcpy((char*)exif_chunk + 10, (char*)buffer + offset + 8, chunksize);
			}
			return exif_chunk;
		}

		// Prevent infinite loop
		if (chunksize > PNG_UINT_31_MAX) return NULL;

		// 12 comes from 4 bytes for chunk size, 4 for chunk name, and 4 for CRC32
		offset += chunksize + 12;
	}
	return NULL;
}
