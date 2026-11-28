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
#include "HdrToneMap.h"
#include "GainMap.h"
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
// HDR (Option A): convert the 16-bit RGBA pixels libjxl wrote (depth 16,
// values scaled to [0, 65535]) to 8-bit BGRA via the HdrToneMap helper.
// Two passes: build a float RGB buffer and compute the content peak
// (for the content-adaptive EETF), then tone-map each pixel. Returns a
// new 8-bit BGRA buffer (caller owns it), or NULL on allocation failure.
// The alpha channel is copied from the 16-bit source (scaled /257);
// the tone mapper itself works on RGB only. (P2-B4: previously the
// output alpha was forced opaque, so a transparent HDR pixel rendered
// as a black block after the background blend.)
// NB-1 (scheme A): when pGain points at a valid extracted gain map,
// the ALTERNATE view is precomputed here from the 16-bit signal
// (ApplyGainMapToBgra16 on the Pass-1 float buffer) BEFORE the tone
// map below destroys it, and stored in pGain->alternateBgra. The
// alternate view is SDR when the base is HDR (weight -1, inverse
// gain). A failure here is not fatal: the lazy 8-bit path
// (CJPEGImage::ComputeHdrBuffer) recomputes it on toggle. pGain may
// be NULL (no gain map wanted); an invalid gain map is a no-op.
static unsigned char* ConvertJxlHdrToSdr(const uint8_t* pixels16, int width, int height, const HdrToneMap::Cicp& cicp,
	gainmap::ExtractedGainMap* pGain)
{
	size_t rgbSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &rgbSize))
		return NULL;
	float* rgb = new(std::nothrow) float[(size_t)width * height * 3];
	if (rgb == NULL)
		return NULL;
	// Pass 1: 16-bit RGBA -> float RGB (0..1). RGBA order: R, G, B, A.
	// NB-1: track transparency here: the alternate-view apply below
	// needs a separate 16-bit alpha plane, built only when some pixel
	// is transparent (else NULL = opaque).
	const uint16_t* p16 = (const uint16_t*)pixels16;
	bool bHasTransparency = false;
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			const uint16_t* px = p16 + ((size_t)y * width + x) * 4;
			float* dst = rgb + ((size_t)y * width + x) * 3;
			dst[0] = (float)((double)px[0] / 65535.0);
			dst[1] = (float)((double)px[1] / 65535.0);
			dst[2] = (float)((double)px[2] / 65535.0);
			if (px[3] != 65535) {
				bHasTransparency = true;
			}
		}
	}
	HdrToneMap::Transfer transfer = HdrToneMap::TransferFromCode(cicp.transfer);
	HdrToneMap::Primaries primaries = HdrToneMap::PrimariesFromCode(cicp.primaries);
	// NB-1 (scheme A): precompute the alternate view from the 16-bit
	// signal (the float rgb buffer IS the signal
	// ApplyGainMapToBgra16 takes) before the tone map below. The
	// weight comes from the signed-weight formula at the alternate
	// headroom (0 for an HDR base -> SDR view, max(base, alt) for an
	// SDR base -> HDR view). The alpha plane is deinterleaved only
	// when some pixel is transparent.
	if (pGain != NULL && pGain->valid) {
		const bool bBaseIsHdr = pGain->meta.baseHdrHeadroom > pGain->meta.alternateHdrHeadroom;
		// H_alt: 0 for an HDR base (the alternate view is SDR), else
		// max(base, alt) -- which IS the alternate headroom here,
		// since !bBaseIsHdr means base <= alt.
		const double hAlt = bBaseIsHdr ? 0.0 : pGain->meta.alternateHdrHeadroom;
		const double wAlt = gainmap::ComputeGainWeight(pGain->meta, hAlt);
		std::vector<uint16_t> alpha16;
		const uint16_t* pAlpha = NULL;
		if (bHasTransparency) {
			try {
				alpha16.resize((size_t)width * (size_t)height);
			} catch (...) {
				alpha16.clear();
			}
			if (!alpha16.empty()) {
				for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
					alpha16[i] = p16[i * 4 + 3];
				}
				pAlpha = alpha16.data();
			}
		}
		std::vector<uint8_t> alternate;
		try {
			alternate.resize((size_t)width * (size_t)height * 4);
		} catch (...) {
			alternate.clear();
		}
		if (!alternate.empty() && gainmap::ApplyGainMapToBgra16(rgb, width, height, transfer, primaries,
				pGain->gray.data(), pGain->width, pGain->height, pGain->meta, wAlt,
				pAlpha, 65535, alternate.data(), (size_t)width * 4)) {
			pGain->alternateBgra = std::move(alternate);
		}
	}
	double peakNits = HdrToneMap::ComputePeakNits(rgb, (size_t)width * height, transfer, primaries);
	// P2-C3: precompute the EETF parameters once per image (the
	// per-pixel ConvertPixel call recomputed them every pixel).
	HdrToneMap::Bt2390Params eetfParams = HdrToneMap::Bt2390MakeParams(peakNits);
	// Pass 2: tone-map each pixel to 8-bit BGRA.
	size_t outSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 4, (size_t)height, &outSize)) {
		delete[] rgb;
		return NULL;
	}
	unsigned char* out = new(std::nothrow) unsigned char[outSize];
	if (out == NULL) {
		delete[] rgb;
		return NULL;
	}
	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			const float* src = rgb + ((size_t)y * width + x) * 3;
			unsigned char* dst = out + ((size_t)y * width + x) * 4;
			uint8_t r = 0, g = 0, b = 0;
			HdrToneMap::ConvertPixelWithParams((double)src[0], (double)src[1], (double)src[2],
				transfer, primaries, eetfParams, r, g, b);
			dst[0] = b;
			dst[1] = g;
			dst[2] = r;
			// P2-B4: copy the source alpha (16-bit -> 8-bit, /257 maps
			// 65535 -> 255 exactly), not opaque.
			const uint16_t* px = p16 + ((size_t)y * width + x) * 4;
			dst[3] = (uint8_t)(px[3] / 257);
		}
	}
	delete[] rgb;
	return out;
}

bool JxlReader::DecodeJpegXlOneShot(const uint8_t* jxl, size_t size, std::vector<uint8_t>* pixels, int& xsize,
	int& ysize, bool& have_animation, int& frame_count, int& frame_time, std::vector<uint8_t>* icc_profile, bool& outOfMemory, bool& isSizeLimitExceeded,
	bool& isHdr, HdrToneMap::Cicp& cicp) {

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
	// HDR (Option A): the COLOR_ENCODING event fires only once per
	// file, so a sequential animation frame never sees it again.
	// Seed the format and the out-params from the cache (set on the
	// first decode of this file); the event below updates them.
	isHdr = cache.isHdr;
	cicp = cache.cicp;
	if (cache.isHdr)
		format.data_type = JXL_TYPE_UINT16;
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
			// HDR (Option A): read the structured color encoding
			// first. If it declares PQ/HLG, the helper tone-maps the
			// image INSTEAD of the lcms2 ICC path: lcms2 applies the
			// ICC profile colorimetrically (no BT.2390 highlight
			// roll-off), so applying both would double-apply the
			// transfer and the result would still be too dark.
			// The ICC profile is then not fetched at all. If the
			// image has no structured profile (XYB or a custom ICC
			// profile - GetColorAsEncodedProfile fails), the ICC
			// path below is used exactly as before. JXL carries the
			// primaries and the white point separately, while CICP
			// folds the white point into the primaries code: JXL P3
			// (11) + a D65 white point is Display P3 = CICP 12, and
			// P3 + a DCI white point stays 11 (DCI-P3). The rewrite
			// below must stay: PrimariesFromCode cannot tell the two
			// apart from the code alone, and deleting it would route
			// Display P3 JXL files through the DCI matrix (P2-B1).
			JxlColorEncoding enc;
			if (JXL_DEC_SUCCESS == JxlDecoderGetColorAsEncodedProfile(
				cache.decoder, JXL_COLOR_PROFILE_TARGET_DATA, &enc)) {
				cicp.primaries = (int)enc.primaries;
				if (enc.primaries == JXL_PRIMARIES_P3 && enc.white_point == JXL_WHITE_POINT_D65)
					cicp.primaries = 12;
				cicp.transfer = (int)enc.transfer_function;
				cicp.matrix = 0; // JXL pixel data is RGB; no YUV matrix
				cicp.fullRange = 1;
				isHdr = HdrToneMap::IsHdr(cicp);
				cache.isHdr = isHdr;
				cache.cicp = cicp;
				if (isHdr) {
					// Decode to 16-bit so the PQ/HLG signal survives
					// quantization (libjxl scales the native bit depth
					// to [0, 65535]); the helper normalizes by 65535.
					// The NEED_IMAGE_OUT_BUFFER event comes after
					// this one, so the changed format takes effect.
					format.data_type = JXL_TYPE_UINT16;
				}
			}
			if (!isHdr) {
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
			}
		} else if (status == JXL_DEC_FRAME) {
			nFrameEvents++; // F-16: count frames as they are decoded
			JxlFrameHeader header = {};
			if (JXL_DEC_SUCCESS != JxlDecoderGetFrameHeader(cache.decoder, &header))
				return false;
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
			// HDR decodes to 16-bit, so the pixel is 8 bytes, not 4.
			size_t bytesPerPixel = (format.data_type == JXL_TYPE_UINT16) ? 8 : 4;
			if (buffer_size != (size_t)cache.info.xsize * cache.info.ysize * bytesPerPixel) {
				return false;
			}
			// P2-B5: BASIC_INFO only caps the dimensions (65535) and the
			// pixel cap (off by default), so a 65535x65535 header reaches
			// here and resize(34 GB) throws bad_alloc, which the outer
			// catch reports as ExceptionError. Guard it first so the
			// failure is classified as out-of-memory instead (the
			// provider's free-and-retry path). buffer_size was just
			// proven equal to xsize*ysize*bytesPerPixel above, so the
			// two forms are equivalent.
			if (!IsImageBufferSizeOk((size_t)cache.info.xsize * bytesPerPixel, (size_t)cache.info.ysize)) {
				outOfMemory = true;
				return false;
			}
			pixels->resize((size_t)cache.info.xsize * cache.info.ysize * bytesPerPixel);
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

// JXL gain map (jhgm) extraction -- plan §3.4/§3.5. Decodes a naked
// JXL codestream (the gain map inside a jhgm box) to single-channel
// 8-bit grayscale with a throwaway decoder (the main decoder in
// cache is busy with the animation state machine). Single-threaded:
// the gain map is tiny (the sample's is 33x25), so no parallel
// runner. Returns false (gray untouched) on any failure; the caller
// falls back to SDR. The loop checks the out-buffer return values
// (M-3): a 3-channel codestream with a 1-channel request makes
// ProcessInput repeat JXL_DEC_NEED_IMAGE_OUT_BUFFER while both
// buffer calls return ERROR, so an unchecked loop would hang the
// load thread. The iteration bound is the second line of defence.
static bool DecodeJxlGainMapGray(const uint8_t* code, size_t codeSize,
                                 int baseWidth, int baseHeight,
                                 std::vector<uint8_t>& gray, int& gainWidth, int& gainHeight) {
    auto decoder = JxlDecoderMake(nullptr);
    if (decoder.get() == NULL) {
        return false;
    }
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(decoder.get(),
        JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)) {
        return false;
    }
    JxlDecoderSetInput(decoder.get(), code, codeSize);
    JxlDecoderCloseInput(decoder.get());
    JxlPixelFormat format = { 1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
    gainWidth = gainHeight = 0;
    // M-3: bound the loop; a still codestream needs only a handful of
    // iterations (BASIC_INFO, NEED_IMAGE_OUT_BUFFER, FULL_IMAGE).
    const int kMaxIterations = 64;
    for (int iter = 0; iter < kMaxIterations; iter++) {
        JxlDecoderStatus status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_ERROR) {
            return false;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            return false;
        } else if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info;
            if (JXL_DEC_SUCCESS != JxlDecoderGetBasicInfo(decoder.get(), &info)) {
                return false;
            }
            // m-4: the ISO metadata carries no dimensions; the base
            // size is the bound (a gain map is never larger than its
            // base). Zero dimensions are rejected the same way.
            if (info.xsize == 0 || info.ysize == 0 ||
                info.xsize > (uint32_t)baseWidth || info.ysize > (uint32_t)baseHeight) {
                return false;
            }
            gainWidth = (int)info.xsize;
            gainHeight = (int)info.ysize;
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            // M-3: check both out-buffer calls, exactly as
            // DecodeJpegXlOneShot does (JXLWrapper.cpp:271-303) -- a
            // 3-channel codestream fails here, not in ProcessInput.
            size_t buffer_size;
            if (JXL_DEC_SUCCESS !=
                JxlDecoderImageOutBufferSize(decoder.get(), &format, &buffer_size)) {
                return false;
            }
            if (gainWidth <= 0 || gainHeight <= 0 ||
                buffer_size != (size_t)gainWidth * gainHeight) {
                return false;
            }
            gray.resize(buffer_size);
            if (JXL_DEC_SUCCESS != JxlDecoderSetImageOutBuffer(decoder.get(), &format,
                gray.data(), gray.size())) {
                return false;
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            return gainWidth > 0 && gainHeight > 0 &&
                gray.size() == (size_t)gainWidth * gainHeight;
        } else {
            return false;
        }
    }
    return false;  // iteration bound hit
}

// JXL gain map (jhgm) extraction -- plan §3. Scans the file buffer
// for the jhgm box, parses the bundle + ISO metadata (URN-less), and
// decodes the nested codestream to grayscale. Returns false (leaves
// *pGainOut untouched, valid stays false) when there is no gain map
// or anything fails; the caller keeps the base view. Runs after
// DecodeJpegXlOneShot and before the HDR conversion, so the 16-bit
// signal is still available for the alternate view (scheme A), and
// before DeleteCache(), so the file buffer is still alive. Never
// returns true with an invalid map. NB-1: the base may be HDR as
// well as SDR -- the old isHdr gate and the baseHdrHeadroom > 0 gate
// are both removed, so an HDR base with a gain map is extracted too
// (its alternate view is the SDR view, weight -1).
static bool ExtractJxlGainMap(const uint8_t* fileData, size_t fileSize,
                              int baseWidth, int baseHeight,
                              gainmap::ExtractedGainMap* pGainOut) {
    if (fileData == NULL || pGainOut == NULL) {
        return false;
    }
    const uint8_t* jhgm = NULL;
    size_t jhgmSize = 0;
    if (!gainmap::FindJxlGainMapBox(fileData, fileSize, jhgm, jhgmSize)) {
        return false;
    }
    const uint8_t* metadata = NULL;
    size_t metadataSize = 0;
    const uint8_t* gainCode = NULL;
    size_t gainCodeSize = 0;
    if (!gainmap::ParseJxlGainMapBundle(jhgm, jhgmSize, metadata, metadataSize,
            gainCode, gainCodeSize)) {
        return false;
    }
    gainmap::Metadata meta;
    if (!gainmap::ParseIso21496MetadataNoUrn(metadata, metadataSize, meta)) {
        return false;
    }
    gainmap::ExtractedGainMap gain;
    if (!DecodeJxlGainMapGray(gainCode, gainCodeSize, baseWidth, baseHeight,
                              gain.gray, gain.width, gain.height)) {
        return false;
    }
    gain.meta = meta;
    gain.valid = true;
    *pGainOut = std::move(gain);
    return true;
}

void* JxlReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int frame_index, // M-D7: requested frame (0 restarts the animation decoder)
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes,
	gainmap::ExtractedGainMap* pGainOut)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	has_animation = false;
	unsigned char* pPixelData = NULL;
	exif_chunk = NULL;
	// M-D7: frame 0 restarts the animation. The decoder and the compressed
	// stream live in cache, so rewind and re-subscribe here; the following
	// DecodeJpegXlOneShot() then returns frame 0 (total_frames is already
	// known from F-16, so no recount happens).
	// R-A4: DecodeJpegXlOneShot only advances one frame per call, so a
	// request that is not the immediate successor of the last returned
	// frame (a step back, a forward jump, or a fresh decoder) must rewind
	// and step forward to the target instead. cache.frame_index tracks
	// the last returned frame (-1 = none yet, or unknown after a failed
	// decode, in which case the rewind below re-syncs the decoder before
	// any frame is read).
	bool bSequential = (cache.decoder != NULL && cache.frame_index >= 0
		&& cache.frame_index == frame_index - 1);
	int nSteps = 1;
	if (!bSequential) {
		if (cache.decoder != NULL && cache.data != NULL) {
			JxlDecoderRewind(cache.decoder);
			JxlDecoderSubscribeEvents(cache.decoder, JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
			JxlDecoderSetInput(cache.decoder, cache.data, cache.data_size);
			JxlDecoderCloseInput(cache.decoder);
		}
		cache.frame_index = -1;
		nSteps = frame_index + 1;
	}

	std::vector<uint8_t> pixels;
	std::vector<uint8_t> icc_profile;
	bool isHdr = false;
	HdrToneMap::Cicp cicp;
	// Decode the requested frame: one call for the sequential fast path,
	// frame_index + 1 calls after a (re)wind (the first returns frame 0).
	// The intermediate results overwrite the same vectors; only the last
	// call's pixels, frame time and frame count are kept. A failed call
	// leaves the decoder mid-stream, so -1 forces the next request to
	// rewind before decoding again.
	for (int i = 0; i < nSteps; i++) {
		if (!DecodeJpegXlOneShot((const uint8_t*)buffer, sizebytes, &pixels, width, height,
			has_animation, frame_count, frame_time, &icc_profile, outOfMemory, isSizeLimitExceeded,
			isHdr, cicp)) {
			cache.frame_index = -1;
			return NULL;
		}
	}
	cache.frame_index = frame_index;
	size_t size = (size_t)width * height * nchannels;
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	// NB-1: extract the gain map BEFORE the HDR conversion below:
	// the conversion tone-maps the 16-bit signal to 8-bit SDR, and
	// the alternate view (scheme A, computed in ConvertJxlHdrToSdr)
	// needs the 16-bit signal. The gain is assigned to *pGainOut at
	// the old location below. P2-A1 (AVIF): gain-map work allocates,
	// so a bad_alloc here must not fail the whole load -- fall back
	// to the base view.
	gainmap::ExtractedGainMap gain;
	if (pGainOut != NULL) {
		try {
			ExtractJxlGainMap(cache.data, cache.data_size, width, height, &gain);
		} catch (...) {
			// Keep the base image; the gain map is optional.
		}
	}
	if (isHdr) {
		// HDR: the pixels are 16-bit RGBA; tone-map them to 8-bit
		// BGRA with the helper INSTEAD of the lcms2 ICC path (the
		// ICC profile was not even fetched for HDR images). The
		// helper is the only colour conversion on this path. The gain
		// (when valid) rides along so the alternate view is
		// precomputed from the 16-bit signal before it is destroyed.
		unsigned char* hdrPixels = ConvertJxlHdrToSdr(pixels.data(), width, height, cicp, &gain);
		if (hdrPixels == NULL) {
			delete[] pPixelData;
			outOfMemory = true;
			return NULL;
		}
		memcpy(pPixelData, hdrPixels, size);
		delete[] hdrPixels;
	} else if (cache.transform == NULL)
		cache.transform = ICCProfileTransform::CreateTransform(icc_profile.data(), icc_profile.size(), ICCProfileTransform::FORMAT_RGBA);
	if (!isHdr && !ICCProfileTransform::DoTransform(cache.transform, pixels.data(), pPixelData, width, height)) {
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

	// Gain-map (HDR toggle): the gain was extracted before the HDR
	// conversion above (the 16-bit signal is gone by now); assign it
	// here, at the old extraction location, so the load thread
	// attaches it (with the precomputed alternate view, when present)
	// and applies it on demand, or at load when GainMapDefault() asks
	// for the non-base view. It runs before DeleteCache() (the file
	// buffer is still alive). (The SDR alternate view is computed
	// lazily on toggle, from the sRGB base, as before.)
	if (pGainOut != NULL && gain.valid) {
		*pGainOut = std::move(gain);
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
