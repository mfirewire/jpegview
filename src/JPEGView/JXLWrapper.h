
#pragma once

#include <vector>
#include "jxl/decode.h"
#include "jxl/decode_cxx.h"
#include "jxl/resizable_parallel_runner.h"
#include "jxl/resizable_parallel_runner_cxx.h"

class JxlReader
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
		const void* buffer, // memory address containing jxl compressed data.
		int sizebytes); // size of jxl compressed data

	// R-01: instance method now - the cache below is one per load thread
	// (owned by CImageLoadThread), not a process-wide static.
	void DeleteCache();

	static void* Compress(const void* buffer, // address of image in memory, format must be 3 bytes per pixel BRGBGR with padding to 4 byte boundary
		int width, // width of image in pixels
		int height, // height of image in pixels.
		size_t& len, // returns length of compressed data
		int quality, // image quality as a percentage
		bool lossless); // use lossless compression if true

	static void* CompressJPEG(const void* buffer, // address of JPEG-encoded image in memory
		size_t input_len, // Length of input JPEG image
		size_t& output_len); // returns length of compressed data

	static void Free(void* buffer);

private:
	// R-01: the animation cache. It used to be a process-wide static, shared
	// by both load threads with no lock: one thread paging to another animated
	// JXL freed the decoder out from under the other thread's in-flight frame
	// decode (heap corruption 0xC0000374). The cache is now a member of this
	// reader, and CImageLoadThread owns one reader per thread, so each thread
	// only ever touches its own cache. The struct is inert on destruction: the
	// decoder, runner and Exif scratch buffer are raw pointers, and
	// DeleteCache() is the single place that releases them - CImageLoadThread's
	// destructor deliberately skips that release when Terminate() times out, so
	// a decode still in flight must not lose its buffers to member destruction.
	// (The old smart pointers and std::vector would have freed their resources
	// from the implicit destructor, breaking exactly that rule.)
	struct jxl_cache {
		JxlDecoder* decoder;
		void* runner;
		JxlBasicInfo info;
		uint8_t* data;
		size_t data_size;
		int prev_frame_timestamp;
		int total_frames; // F-16: frames counted on the first decode of this file (0 = not counted yet); JXL stores no frame count.
		int width;
		int height;
		void* transform;
		uint8_t* exif;
		size_t exif_size;
		size_t exif_capacity;
	};

	jxl_cache cache = {};

	bool DecodeJpegXlOneShot(const uint8_t* jxl, size_t size, std::vector<uint8_t>* pixels, int& xsize,
		int& ysize, bool& have_animation, int& frame_count, int& frame_time, std::vector<uint8_t>* icc_profile, bool& outOfMemory, bool& isSizeLimitExceeded);
	// R-01: grow the Exif scratch buffer to new_size bytes. The allocation is
	// never shrunk: libjxl may still hold the buffer pointer from the last
	// JxlDecoderSetBoxBuffer call, and a shrinking realloc could move it.
	// Returns false on allocation failure; the old buffer is left intact.
	bool ExifReserve(size_t new_size);
};
