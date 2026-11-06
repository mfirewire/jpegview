
#pragma once

#include <vector>
#include "avif/avif.h"

class AvifReader
{
public:
	// Returns data in 4 byte BGRA
	void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		int& bpp,     // BYTES (not bits) PER PIXEL.
		bool& has_animation,     // if the image is animated
		int frame_index, // index of frame
		int& frame_count, // number of frames
		int& frame_time, // frame duration in milliseconds
		void*& exif_chunk, // Pointer to Exif data (must be freed by caller)
		bool& outOfMemory, // set to true when no memory to read image
		bool& isSizeLimitExceeded, // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
		const void* buffer, // memory address containing jxl compressed data.
		int sizebytes); // size of jxl compressed data

	// R-01: instance method now - the cache below is one per load thread
	// (owned by CImageLoadThread), not a process-wide static.
	void DeleteCache();

private:
	// R-01: the animation cache. It used to be a process-wide static, shared
	// by both load threads with no lock: one thread paging to another animated
	// AVIF freed the decoder out from under the other thread's in-flight frame
	// decode (heap corruption 0xC0000374). The cache is now a member of this
	// reader, and CImageLoadThread owns one reader per thread, so each thread
	// only ever touches its own cache. The struct is inert on destruction: it
	// holds raw pointers only, and DeleteCache() is the single place that
	// releases them - CImageLoadThread's destructor deliberately skips that
	// release when Terminate() times out, so a decode still in flight must not
	// lose its buffers to member destruction.
	struct avif_cache {
		avifDecoder* decoder;
		avifRGBImage rgb;
		uint8_t* data;
		size_t data_size;
		void* transform;
	};

	avif_cache cache = {};
};
