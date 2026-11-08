
#pragma once

#include "webp/decode.h"
#include "webp/demux.h"

class WebpReaderWriter
{
public:
	// Returns data in 4 byte BGRA
	void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		int& bpp,     // BYTES (not bits) PER PIXEL.
		bool& has_animation,     // if the image is animated
		int frame_index,  // M-D7: requested frame (0 restarts the animation decoder)
		int& frame_count, // number of frames
		int& frame_time, // frame duration in milliseconds
		void*& exif, // Pointer to Exif data (must be freed by caller)
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing webp compressed data.
		int sizebytes); // size of webp compressed data

	// R-01: instance method now - the cache below is one per load thread
	// (owned by CImageLoadThread), not a process-wide static.
	void DeleteCache();

	// Compress image data into WEBP stream, returns compressed data.
	static void* Compress(const void* buffer, // address of image in memory, format must be 3 bytes per pixel BRGBGR with padding to 4 byte boundary
		int width, // width of image in pixels
		int height, // height of image in pixels.
		size_t& len, // returns length of compressed data
		int quality, // image quality as a percentage (ignored if lossless)
		bool lossless); // use lossless compression if true

	static void FreeMemory(void* pointer);

private:
	// R-01: the animation cache. It used to be a process-wide static, shared
	// by both load threads with no lock: one thread paging to another animated
	// WebP freed the decoder out from under the other thread's in-flight frame
	// decode (heap corruption 0xC0000374). The cache is now a member of this
	// reader, and CImageLoadThread owns one reader per thread, so each thread
	// only ever touches its own cache. The struct is inert on destruction: it
	// holds raw pointers only, and DeleteCache() is the single place that
	// releases them - CImageLoadThread's destructor deliberately skips that
	// release when Terminate() times out, so a decode still in flight must not
	// lose its buffers to member destruction.
	struct webp_cache {
		WebPAnimDecoder* decoder;
		WebPData data;
		int prev_frame_timestamp;
		int width;
		int height;
		void* transform;
	};

	webp_cache cache = {};
};
