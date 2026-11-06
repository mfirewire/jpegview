#include "stdafx.h"

#include "JXLWrapper.h"
#include "jxl/decode.h"
#include "jxl/decode_cxx.h"
#include "jxl/encode.h"
#include "jxl/encode_cxx.h"
#include "jxl/thread_parallel_runner.h"
#include "jxl/thread_parallel_runner_cxx.h"
#include "jxl/resizable_parallel_runner.h"
#include "jxl/resizable_parallel_runner_cxx.h"
#include "Helpers.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"

static void* DoCompress(JxlEncoder* enc, size_t& len);

// R-01: grow the Exif scratch buffer to new_size bytes. The allocation is never
// shrunk: libjxl may still hold the buffer pointer from the last
// JxlDecoderSetBoxBuffer call, and a shrinking realloc could move it. Returns
// false on allocation failure; the old buffer is left intact.
bool JxlReader::ExifReserve(size_t new_size) {
	if (new_size <= cache.exif_capacity) {
		cache.exif_size = new_size;
		return true;
	}
	uint8_t* pNew = (uint8_t*)realloc(cache.exif, new_size);
	if (pNew == NULL)
		return false;
	cache.exif = pNew;
	cache.exif_capacity = new_size;
	cache.exif_size = new_size;
	return true;
}

// based on https://github.com/libjxl/libjxl/blob/main/examples/decode_oneshot.cc
// and https://github.com/libjxl/libjxl/blob/main/examples/decode_exif_metadata.cc
bool JxlReader::DecodeJpegXlOneShot(const uint8_t* jxl, size_t size, std::vector<uint8_t>* pixels, int& xsize,
	int& ysize, bool& have_animation, int& frame_count, int& frame_time, std::vector<uint8_t>* icc_profile, bool& outOfMemory, bool& isSizeLimitExceeded) {

	// F-16: true only on the first decode of a file, when the decoder is
	// created below; the total frame count is counted on that call (JXL
	// stores no frame count) and cached for later frames.
	bool bCountThisCall = (cache.decoder == NULL);

	if (cache.decoder == NULL) {
		// R-20: take ownership of the file buffer BEFORE the fallible
		// setup below. The caller (ProcessReadJXLRequest) hands pBuffer
		// to this reader and only releases it through
		// DeleteCachedJxlDecoder() -> DeleteCache() -> delete[] cache.data,
		// so any return false before the old cache.data assignment (at
		// the end of this block) leaked one file buffer per failed open
		// (up to MAX_JXL_FILE_SIZE). With the assignment first, every
		// early return is cleaned up by the caller's DeleteCachedJxlDecoder.
		// A stale cache.data here (no decoder, so no decode can own it)
		// is freed first so a re-assignment cannot leak the old buffer;
		// on the normal path it is NULL (DeleteCache resets the cache).
		delete[] cache.data;
		cache.data = (uint8_t*)jxl;
		cache.data_size = size;
		cache.runner = JxlResizableParallelRunnerMake(nullptr).release();

		cache.decoder = JxlDecoderMake(nullptr).release();
		if (cache.decoder == NULL) {
			return false;
		}
		if (JXL_DEC_SUCCESS !=
			JxlDecoderSubscribeEvents(cache.decoder, JXL_DEC_BASIC_INFO |
				JXL_DEC_COLOR_ENCODING |
				JXL_DEC_BOX |
				JXL_DEC_FRAME |
				JXL_DEC_FULL_IMAGE)) {
			return false;
		}

		if (JXL_DEC_SUCCESS != JxlDecoderSetDecompressBoxes(cache.decoder, JXL_TRUE)) {
			return false;
		}

		if (JXL_DEC_SUCCESS != JxlDecoderSetParallelRunner(cache.decoder,
			JxlResizableParallelRunner,
			cache.runner)) {
			return false;
		}

		JxlDecoderSetInput(cache.decoder, jxl, size);
		JxlDecoderCloseInput(cache.decoder);
		// (R-20: cache.data was already assigned at the top of this
		// block, so the early-failure paths above cannot leak the buffer.)
	}

	JxlBasicInfo info;
	JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	const constexpr size_t kChunkSize = 65536;
	size_t output_pos = 0;

	bool loop_check = false;
	int nFrameEvents = 0; // F-16: JXL_DEC_FRAME events seen in this call
	bool bCountingFrames = false; // F-16: first frame seen, counting the rest
	bool bRewoundAfterCount = false; // F-16: decoder rewound after the counting pass
	for (;;) {
		JxlDecoderStatus status = JxlDecoderProcessInput(cache.decoder);

		if (status == JXL_DEC_ERROR) {
			return false;
		} else if (status == JXL_DEC_NEED_MORE_INPUT) {
			return false;
		} else if (status == JXL_DEC_BASIC_INFO) {
			if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(cache.decoder, &info)) {
				return false;
			}
			cache.info = info;
			if (cache.info.xsize > MAX_IMAGE_DIMENSION || cache.info.ysize > MAX_IMAGE_DIMENSION)
				return false;
			// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
			// No abs(): xsize/ysize are unsigned and proven <= MAX_IMAGE_DIMENSION
			// above; unqualified abs(double) resolves to ::abs(int), and a product
			// over INT_MAX would narrow to a negative int (abs(INT_MIN) is UB).
			if ((double)cache.info.xsize * (double)cache.info.ysize > (double)CSettingsProvider::This().MaxImagePixels()) {
				isSizeLimitExceeded = true;
				return false;
			}
			JxlResizableParallelRunnerSetThreads(
				cache.runner,
				JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
		} else if (status == JXL_DEC_COLOR_ENCODING) {
			// Get the ICC color profile of the pixel data
			size_t icc_size;
			if (JXL_DEC_SUCCESS !=
				JxlDecoderGetICCProfileSize(
					cache.decoder, JXL_COLOR_PROFILE_TARGET_DATA, &icc_size)) {
				return false;
			}
			icc_profile->resize(icc_size);
			if (JXL_DEC_SUCCESS != JxlDecoderGetColorAsICCProfile(
				cache.decoder,
				JXL_COLOR_PROFILE_TARGET_DATA,
				icc_profile->data(), icc_profile->size())) {
				return false;
			}
		} else if (status == JXL_DEC_FRAME) {
			nFrameEvents++; // F-16: count frames as they are decoded
			JxlFrameHeader header;
			JxlDecoderGetFrameHeader(cache.decoder, &header);
			JxlAnimationHeader animation = cache.info.animation;
			if (animation.tps_numerator)
				frame_time = (int)(1000.0 * header.duration * animation.tps_denominator / animation.tps_numerator);
			else
				frame_time = 0;
		} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			size_t buffer_size;
			if (JXL_DEC_SUCCESS !=
				JxlDecoderImageOutBufferSize(cache.decoder, &format, &buffer_size)) {
				return false;
			}
			// A1-4: xsize/ysize are uint32_t; the product must be computed in
			// size_t or it wraps above ~1073 MP and the image is mis-rejected.
			if (buffer_size != (size_t)cache.info.xsize * cache.info.ysize * 4) {
				return false;
			}
			pixels->resize((size_t)cache.info.xsize * cache.info.ysize * 4);
			void* pixels_buffer = (void*)pixels->data();
			size_t pixels_buffer_size = pixels->size() * sizeof(uint8_t);
			if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(cache.decoder, &format,
				pixels_buffer,
				pixels_buffer_size)) {
				return false;
			}
		} else if (status == JXL_DEC_FULL_IMAGE) {
			// Full frame has been decoded

			xsize = cache.info.xsize;
			ysize = cache.info.ysize;
			have_animation = cache.info.have_animation;
			if (!have_animation) {
				frame_count = 1;
				return true;
			}
			if (bCountThisCall && !bCountingFrames) {
				// F-16: first frame of an animation on the first decode of
				// this file. JXL stores no frame count, and the old
				// hardcoded 2 made the player wrap after frame 1, so
				// frames 2+ never played. Keep decoding instead: later
				// frames overwrite *pixels and are discarded, until the
				// stream ends and the total is known. The SUCCESS branch
				// below rewinds the decoder and frame 0 is decoded again.
				// Cost: one full animation decode on first open.
				bCountingFrames = true;
			} else if (bCountingFrames && bRewoundAfterCount) {
				// Re-decoded frame 0 after the counting pass rewound the
				// decoder; *pixels holds it again and the total is known.
				frame_count = cache.total_frames;
				return true;
			} else if (!bCountingFrames) {
				// Later frame of an animation whose total was counted on
				// the first decode of this file.
				frame_count = cache.total_frames;
				return true;
			}
			// Else: still counting (a later frame's FULL_IMAGE during the
			// counting pass) - fall through and keep decoding.
		} else if (status == JXL_DEC_SUCCESS) {
			// All decoding successfully finished, we are at the end of the file.
			// We must rewind the decoder to get a new frame.
			if (loop_check)
				return false; // escape infinite loop
			loop_check = true;
			// F-16: the counting pass ends here; remember the total so later
			// frames report it without recounting.
			if (bCountingFrames && cache.total_frames == 0)
				cache.total_frames = nFrameEvents;
			if (bCountingFrames)
				bRewoundAfterCount = true;
			JxlDecoderRewind(cache.decoder);
			JxlDecoderSubscribeEvents(cache.decoder, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
			JxlDecoderSetInput(cache.decoder, cache.data, cache.data_size);
			JxlDecoderCloseInput(cache.decoder);
		} else if (status == JXL_DEC_BOX) {
			if (cache.exif_size > 0) {
				size_t remaining = JxlDecoderReleaseBoxBuffer(cache.decoder);
				cache.exif_size -= remaining;
			} else {
				JxlBoxType type;
				if (JXL_DEC_SUCCESS !=
					JxlDecoderGetBoxType(cache.decoder, type, true)) {
					return false;
				}
				if (!memcmp(type, "Exif", 4)) {
					if (!ExifReserve(kChunkSize))
						return false;
					JxlDecoderSetBoxBuffer(cache.decoder, cache.exif, cache.exif_size);
				}
			}
		} else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
			size_t remaining = JxlDecoderReleaseBoxBuffer(cache.decoder);
			output_pos += kChunkSize - remaining;
			if (!ExifReserve(cache.exif_size + kChunkSize))
				return false;
			JxlDecoderSetBoxBuffer(cache.decoder, cache.exif + output_pos, cache.exif_size - output_pos);
		} else {
			return false;
		}
	}
}

void* JxlReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	has_animation = false;
	unsigned char* pPixelData = NULL;
	exif_chunk = NULL;


	std::vector<uint8_t> pixels;
	std::vector<uint8_t> icc_profile;
	if (!DecodeJpegXlOneShot((const uint8_t*)buffer, sizebytes, &pixels, width, height,
		has_animation, frame_count, frame_time, &icc_profile, outOfMemory, isSizeLimitExceeded)) {
		return NULL;
	}
	size_t size = (size_t)width * height * nchannels;
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	if (cache.transform == NULL)
		cache.transform = ICCProfileTransform::CreateTransform(icc_profile.data(), icc_profile.size(), ICCProfileTransform::FORMAT_RGBA);
	if (!ICCProfileTransform::DoTransform(cache.transform, pixels.data(), pPixelData, width, height)) {
		// RGBA -> BGRA conversion (with little-endian integers)
		uint32_t* data = (uint32_t*)pixels.data();
		for (size_t i = 0; i * sizeof(uint32_t) < size; i++) {
			((uint32_t*)pPixelData)[i] = _rotr(_byteswap_ulong(data[i]), 8);
		}
	}

	// Copy Exif data into the format understood by CEXIFReader
	if (cache.exif_size > 8 && cache.exif_size < 65532) {
		exif_chunk = malloc(cache.exif_size + 6);
		if (exif_chunk != NULL) {
			memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
			*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(cache.exif_size + 4);
			memcpy((uint8_t*)exif_chunk + 10, cache.exif + 4, cache.exif_size - 4);
		}
	}

	if (!has_animation)
		DeleteCache();

	return pPixelData;
}

void JxlReader::DeleteCache() {
	// The buffer is allocated with new[] in ProcessReadJXLRequest; freeing it with
	// free() was undefined behaviour and asserts on the debug CRT.
	delete[] cache.data;
	free(cache.exif);
	ICCProfileTransform::DeleteTransform(cache.transform);
	// R-01: explicit destroys, not smart pointers. The cache is an inert struct
	// (see JXLWrapper.h): only this function releases its resources, so that
	// CImageLoadThread's destructor can skip the release when Terminate() times
	// out instead of destroying a decoder a still-running decode is using.
	// The decoder is destroyed before its runner: it holds the runner pointer.
	if (cache.decoder != NULL) {
		JxlDecoderDestroy(cache.decoder);
	}
	if (cache.runner != NULL) {
		JxlResizableParallelRunnerDestroy(cache.runner);
	}
	cache = { 0 };
}

// based on https://github.com/libjxl/libjxl/blob/main/examples/encode_oneshot.cc
void* JxlReader::Compress(const void* buffer,
	int width,
	int height,
	size_t& len,
	int quality,
	bool lossless) {

	auto enc = JxlEncoderMake(nullptr);
	auto runner = JxlThreadParallelRunnerMake(
		nullptr,
		JxlThreadParallelRunnerDefaultNumWorkerThreads());
	if (!enc.get() || !runner.get()) {
		return NULL;
	}
	if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(),
		JxlThreadParallelRunner,
		runner.get())) {
		return NULL;
	}

	JxlPixelFormat pixel_format = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 4 };

	JxlBasicInfo basic_info;
	JxlEncoderInitBasicInfo(&basic_info);
	basic_info.xsize = width;
	basic_info.ysize = height;
	basic_info.bits_per_sample = 8;
	basic_info.uses_original_profile = lossless;
	if (JXL_ENC_SUCCESS != JxlEncoderSetBasicInfo(enc.get(), &basic_info)) {
		return NULL;
	}

	JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc.get(), NULL);
	if (frame_settings == NULL) {
		return NULL;
	}
	if (lossless) {
		if (JXL_ENC_SUCCESS != JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE)) {
			return NULL;
		};
	} else {
		float distance = JxlEncoderDistanceFromQuality(quality);
		distance = max(distance, .1f); // prevent huge file size
		if (JXL_ENC_SUCCESS != JxlEncoderSetFrameDistance(frame_settings, distance)) {
			return NULL;
		};
	}

	// TODO: remove rgb_buffer once libjxl adds channel order support
	int padded_width = Helpers::DoPadding(width * 3, 4);
	size_t buffer_size = (size_t)height * padded_width;
	uint8_t* rgb_buffer = (uint8_t*)malloc(buffer_size);
	if (rgb_buffer == NULL) {
		return NULL;
	}
	// Single-pass BGR->RGB copy (avoids separate memcpy + swap passes)
	for (int i = 0; i < height; i++) {
		const uint8_t* src = (const uint8_t*)buffer + (size_t)i * padded_width;
		uint8_t* dst = rgb_buffer + (size_t)i * padded_width;
		for (int j = 0; j < width; j++, src += 3, dst += 3) {
			dst[0] = src[2]; // R <- B
			dst[1] = src[1]; // G <- G
			dst[2] = src[0]; // B <- R
		}
	}
	if (JXL_ENC_SUCCESS !=
		JxlEncoderAddImageFrame(frame_settings, &pixel_format,
			rgb_buffer,
			buffer_size)) {
		free(rgb_buffer);
		return NULL;
	}
	JxlEncoderCloseInput(enc.get());
	free(rgb_buffer);

	return DoCompress(enc.get(), len);
}

void* JxlReader::CompressJPEG(const void* buffer, size_t input_len, size_t& output_len) {
	auto enc = JxlEncoderMake(nullptr);
	auto runner = JxlThreadParallelRunnerMake(
		nullptr,
		JxlThreadParallelRunnerDefaultNumWorkerThreads());
	if (!enc.get() || !runner.get()) {
		return NULL;
	}
	if (JXL_ENC_SUCCESS != JxlEncoderSetParallelRunner(enc.get(),
		JxlThreadParallelRunner,
		runner.get())) {
		return NULL;
	}
	JxlEncoderFrameSettings* frame_settings = JxlEncoderFrameSettingsCreate(enc.get(), NULL);
	if (frame_settings == NULL) {
		return NULL;
	}
	if (JXL_ENC_SUCCESS != JxlEncoderStoreJPEGMetadata(enc.get(), JXL_TRUE)) {
		return NULL;
	};
	if (JXL_ENC_SUCCESS != JxlEncoderAddJPEGFrame(frame_settings, (const uint8_t*)buffer, input_len)) {
		return NULL;
	}
	JxlEncoderCloseInput(enc.get());

	return DoCompress(enc.get(), output_len);
}

static void* DoCompress(JxlEncoder* enc, size_t& len) {
	std::vector<uint8_t> compressed;
	compressed.resize(1024 * 1024); // start at 1 MB to avoid repeated resize/copy
	uint8_t* next_out = compressed.data();
	size_t avail_out = compressed.size() - (next_out - compressed.data());
	JxlEncoderStatus process_result = JXL_ENC_NEED_MORE_OUTPUT;
	while (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
		process_result = JxlEncoderProcessOutput(enc, &next_out, &avail_out);
		if (process_result == JXL_ENC_NEED_MORE_OUTPUT) {
			size_t offset = next_out - compressed.data();
			compressed.resize(compressed.size() * 2);
			next_out = compressed.data() + offset;
			avail_out = compressed.size() - offset;
		}
	}
	compressed.resize(next_out - compressed.data());
	if (JXL_ENC_SUCCESS != process_result) {
		return NULL;
	}

	len = compressed.size();
	void* pOutput = malloc(len);
	if (pOutput != NULL) {
		memcpy(pOutput, compressed.data(), len);
	}

	return pOutput;
}

void JxlReader::Free(void* buffer) {
	free(buffer);
}
