
#pragma once

#ifndef WINXP
#include "png.h"
#endif

class PngReader
{
public:
#ifndef WINXP
	// Returns data in 4 byte BGRA
	void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		int& bpp,     // BYTES (not bits) PER PIXEL.
		bool& has_animation,     // if the image is animated
		int frame_index,  // M-D7: requested frame (0 restarts the animation decoder)
		int& frame_count, // number of frames
		int& frame_time, // frame duration in milliseconds
		void*& exif_chunk, // Pointer to Exif data (must be freed by caller)
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		void* buffer, // memory address containing png compressed data.
		size_t sizebytes); // size of png compressed data

	// R-01: instance method now - the cache below is one per load thread
	// (owned by CImageLoadThread), not a process-wide static.
	void DeleteCache();

	// Returns true if PNG is unsupported by GDI+
	static bool MustUseLibpng(const void* buffer, size_t sizebytes);
#endif
	// Get EXIF Block
	static void* GetEXIFBlock(void* buffer, size_t sizebytes);

#ifndef WINXP
private:
	// R-01: the animation cache. It used to be a process-wide static, shared
	// by both load threads with no lock: one thread paging to another animated
	// PNG freed the decoder out from under the other thread's in-flight frame
	// decode (heap corruption 0xC0000374). The cache is now a member of this
	// reader, and CImageLoadThread owns one reader per thread, so each thread
	// only ever touches its own cache. The struct is inert on destruction: it
	// holds raw pointers only, and DeleteCache() is the single place that
	// releases them - CImageLoadThread's destructor deliberately skips that
	// release when Terminate() times out, so a decode still in flight must not
	// lose its buffers to member destruction.
	struct png_cache {
		png_structp png_ptr;
		png_infop info_ptr;
		png_uint_32 w0;
		png_uint_32 h0;
		png_uint_32 x0;
		png_uint_32 y0;
		unsigned short delay_num;
		unsigned short delay_den;
		unsigned char dop;
		unsigned char bop;
		unsigned int first;
		png_bytepp rows_image;
		png_bytepp rows_frame;
		unsigned char* p_image;
		unsigned char* p_frame;
		unsigned char* p_temp;
		size_t size;
		unsigned int width;
		unsigned int height;
		unsigned int channels;
		unsigned int frame_index;
		png_uint_32 frame_count;
		void* buffer;
		size_t buffer_size;
		size_t buffer_offset;
		// Transform from the iCCP profile this file carries to sRGB, NULL when the
		// file has none, when it already is sRGB or when the setting is off. Built
		// once per image: every frame of an APNG goes through the same transform.
		void* icc_transform;
	};

	png_cache cache = {};

	bool BeginReading(void* buffer, size_t sizebytes, bool& outOfMemory, bool& isSizeLimitExceeded);
	bool ReadFrameData();
	void* ReadNextFrame(void** exif_chunk, unsigned int* exif_size);
	void DeleteCacheInternal(bool free_buffer);
	// libpng's read callback must be a plain function pointer, so it cannot be
	// a capturing lambda over this reader's cache. The reader instance is passed
	// as the io_ptr instead; the callback reaches the cache through it.
	static void ReadDataCallback(png_structp png_ptr, png_bytep outbuffer, png_size_t sizebytes);
#endif
};
