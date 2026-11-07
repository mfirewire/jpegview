#include "stdafx.h"

#include "WEBPWrapper.h"
#include "webp/decode.h"
#include "webp/encode.h"
#include "webp/demux.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "ICCProfileTransform.h"

void* WebpReaderWriter::ReadImage(int& width,
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
	uint8* pPixelData = NULL;
	WebPBitstreamFeatures features;
	width = height = 0;
	nchannels = 4;
	outOfMemory = false;
	exif_chunk = NULL;

	if (!cache.decoder || !cache.data.bytes) {
		if (!WebPGetInfo((const uint8_t*)buffer, sizebytes, &width, &height))
			return NULL;
		if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
			return NULL;
		if (WebPGetFeatures((const uint8_t*)buffer, sizebytes, &features) != VP8_STATUS_OK)
			return NULL;
		// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
		if ((double)width * height > (double)CSettingsProvider::This().MaxImagePixels()) {
			isSizeLimitExceeded = true;
			return NULL;
		}

		// Check for ICCP chunk
		WebPData data;
		data.bytes = (const uint8_t*)buffer;
		data.size = sizebytes;
		WebPDemuxer* demuxer = WebPDemux(&data);
		WebPChunkIterator chunk_iter;
		void* transform = NULL;
		
		if (WebPDemuxGetChunk(demuxer, "ICCP", 1, &chunk_iter))
			transform = ICCProfileTransform::CreateTransform(chunk_iter.chunk.bytes, chunk_iter.chunk.size, ICCProfileTransform::FORMAT_BGRA);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
		if (WebPDemuxGetChunk(demuxer, "EXIF", 1, &chunk_iter)) {
			WebPData exif = chunk_iter.chunk;
			if (exif.size > 8 && exif.size < 65528 && exif.bytes != NULL) {
				exif_chunk = malloc(exif.size + 10);
				if (exif_chunk != NULL) {
					memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
					*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size + 8);
					memcpy((uint8_t*)exif_chunk + 10, exif.bytes, exif.size);
				}
			}
		}
		WebPDemuxReleaseChunkIterator(&chunk_iter);
		WebPDemuxDelete(demuxer);

		has_animation = features.has_animation;
		if (!has_animation) {
			size_t nStride = (size_t)width * nchannels;
			size_t size = 0;
			if (!IsImageBufferSizeOk(nStride, (size_t)height, &size)) {
				outOfMemory = true;
				ICCProfileTransform::DeleteTransform(transform);
				free(exif_chunk);
				exif_chunk = NULL;
				return NULL;
			}
			pPixelData = new(std::nothrow) unsigned char[size];
			if (pPixelData == NULL) {
				outOfMemory = true;
				ICCProfileTransform::DeleteTransform(transform);
				free(exif_chunk);
				exif_chunk = NULL;
				return NULL;
			}
			// WebPDecodeBGRAInto returns NULL on a corrupt bitstream. The old code
			// ignored it and handed the caller an uninitialised buffer, which then
			// got blended and displayed as if it were a valid image.
			if (WebPDecodeBGRAInto((const uint8_t*)buffer, sizebytes, pPixelData, size, (int)nStride) == NULL) {
				delete[] pPixelData;
				ICCProfileTransform::DeleteTransform(transform);
				free(exif_chunk);
				exif_chunk = NULL;
				return NULL;
			}

			// ICCP transform in place
			ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, width, height);
			ICCProfileTransform::DeleteTransform(transform);

			return pPixelData;
		}
	
		// Cache WebP data and decoder to keep track of where we are in the file
		DeleteCache();
		WebPAnimDecoderOptions anim_config;
		WebPAnimDecoderOptionsInit(&anim_config);
		anim_config.color_mode = MODE_BGRA;
		// WebPDataClear releases this with free(), so it must not come from new[].
		// Keep the cache = { 0 } in DeleteCache() as well: it is what clears
		// data.bytes/data.size after the release.
		uint8_t* cached_webp_bytes = (uint8_t*)malloc(sizebytes);
		if (cached_webp_bytes == NULL) {
			ICCProfileTransform::DeleteTransform(transform);
			free(exif_chunk);
			exif_chunk = NULL;
			outOfMemory = true;
			return NULL;
		}
		memcpy(cached_webp_bytes, buffer, sizebytes);
		cache.data.bytes = cached_webp_bytes;
		cache.data.size = sizebytes;
		cache.decoder = WebPAnimDecoderNew(&cache.data, &anim_config);
		cache.width = width;
		cache.height = height;
		cache.transform = transform;
	}
	WebPAnimDecoder* decoder = cache.decoder;
	WebPData webp_data = cache.data;
	width = cache.width;
	height = cache.height;

	if (decoder == NULL) {
		free(exif_chunk);
		exif_chunk = NULL;
		return NULL;
	}

	// Decode frame
	int timestamp;
	uint8_t* buf;
	if (!WebPAnimDecoderHasMoreFrames(decoder))
		WebPAnimDecoderReset(decoder);
	if (!WebPAnimDecoderGetNext(decoder, &buf, &timestamp)) {
		free(exif_chunk);
		exif_chunk = NULL;
		return NULL;
	}

	// Set frametime and frame count
	WebPAnimInfo anim_info;
	WebPAnimDecoderGetInfo(decoder, &anim_info);
	frame_count = max(anim_info.frame_count, 1);
	timestamp = max(timestamp, 0);
	if (timestamp < cache.prev_frame_timestamp)
		cache.prev_frame_timestamp = 0;
	frame_time = timestamp - cache.prev_frame_timestamp;
	cache.prev_frame_timestamp = timestamp;

	// width * height * nchannels was int arithmetic; WebP caps at 16383x16383 so
	// it cannot actually wrap today, but the decoder is the same shape as the rest
	// and should not rely on a format-specific cap.
	size_t frameBytes = 0;
	if (!IsImageBufferSizeOk((size_t)width * nchannels, (size_t)height, &frameBytes)) {
		outOfMemory = true;
		free(exif_chunk);
		exif_chunk = NULL;
		return NULL;
	}
	pPixelData = new(std::nothrow) unsigned char[frameBytes];
	if (pPixelData == NULL) {
		outOfMemory = true;
		free(exif_chunk);
		exif_chunk = NULL;
		return NULL;
	}

	// Try copying with ICCP transform
	if (!ICCProfileTransform::DoTransform(cache.transform, buf, pPixelData, width, height)) {
		// Copy frame to output buffer directly otherwise
		memcpy(pPixelData, buf, frameBytes);
	}

	return pPixelData;

}

void WebpReaderWriter::DeleteCache() {
	WebPAnimDecoderDelete(cache.decoder);
	WebPDataClear(&cache.data);
	ICCProfileTransform::DeleteTransform(cache.transform);
	cache = { 0 };
}

void* WebpReaderWriter::Compress(const void* source,
	int width,
	int height,
	size_t& len,
	int quality,
	bool lossless) {

	uint8* pOutput = NULL;
	if (lossless)
		len = WebPEncodeLosslessBGR((uint8*)source, width, height, Helpers::DoPadding(width * 3, 4), &pOutput);
	else
		len = WebPEncodeBGR((uint8*)source, width, height, Helpers::DoPadding(width * 3, 4), (float)quality, &pOutput);
	return pOutput;
}

void WebpReaderWriter::FreeMemory(void* pointer) {
	free(pointer);
}
