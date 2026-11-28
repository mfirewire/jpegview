#include "stdafx.h"

#include "HEIFWrapper.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "ICCProfileTransform.h"
#include "HdrToneMap.h"
#include "GainMap.h"

// HDR (Option A): convert the 16-bit RGBA pixels libheif wrote
// (heif_chroma_interleaved_RRGGBBAA_LE, values in the native bit depth
// range [0, (1<<bpp)-1] - libheif does NOT scale them to 16-bit, unlike
// libavif) to 8-bit BGRA via the HdrToneMap helper. Two passes: build a
// float RGB buffer and compute the content peak (for the
// content-adaptive EETF), then tone-map each pixel. `stride` is the byte
// stride of the 16-bit RGBA plane. Returns a new 8-bit BGRA buffer
// (caller owns it), or NULL on allocation failure. The alpha channel is
// copied from the source (scaled by the native maxVal); the tone mapper
// itself works on RGB only. (P2-B4: previously the output alpha was
// forced opaque, so a transparent HDR pixel rendered as a black block
// after the background blend.)
// NB-1 (scheme A): when pGain points at a valid extracted gain map,
// the ALTERNATE view is precomputed here from the 16-bit signal
// (ApplyGainMapToBgra16 on the Pass-1 float buffer) BEFORE the tone
// map below destroys it, and stored in pGain->alternateBgra. The
// alternate view is SDR when the base is HDR (weight -1, inverse
// gain). A failure here is recorded (R-10): the lazy 8-bit path
// recomputes the alternate on toggle only when the base is SDR;
// for an HDR base the toggle is refused and the image stays HDR.
// pGain may be NULL (no gain map wanted); an invalid gain map is
// a no-op.
static unsigned char* ConvertHeifHdrToSdr(const uint8_t* data, int stride, int width, int height, int bpp, const HdrToneMap::Cicp& cicp,
	gainmap::ExtractedGainMap* pGain)
{
	size_t rgbSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &rgbSize))
		return NULL;
	float* rgb = new(std::nothrow) float[(size_t)width * height * 3];
	if (rgb == NULL)
		return NULL;
	const double maxVal = (double)((1 << bpp) - 1);
	// Pass 1: 16-bit RGBA rows -> float RGB (0..1). RGBA order: R, G, B, A.
	// NB-1: track transparency here: the alternate-view apply below
	// needs a separate 16-bit alpha plane, built only when some pixel
	// is transparent (else NULL = opaque). Alpha is in the native
	// range [0, (1<<bpp)-1], like the samples.
	const uint32_t maxValInt = (uint32_t)((1 << bpp) - 1);
	bool bHasTransparency = false;
	for (int y = 0; y < height; y++)
	{
		const uint16_t* row = (const uint16_t*)(data + (size_t)y * stride);
		for (int x = 0; x < width; x++)
		{
			const uint16_t* px = row + x * 4;
			float* dst = rgb + ((size_t)y * width + x) * 3;
			dst[0] = (float)((double)px[0] / maxVal);
			dst[1] = (float)((double)px[1] / maxVal);
			dst[2] = (float)((double)px[2] / maxVal);
			if (px[3] != maxValInt) {
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
	// when some pixel is transparent; its full-scale value is the
	// native maxVal, not 65535.
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
			// P3 (user-added): pre-check the alpha plane against the
			// byte cap, like the rgb and 8-bit out buffers -- the rgb
			// check at the top bounds it implicitly (width*12/row),
			// but make the bound explicit so a reorder can't drop it.
			// On failure the plane stays empty; the apply is then
			// skipped and the loss is recorded (R-10), so a
			// transparent image never gets a silently opaque alternate.
			if (IsImageBufferSizeOk((size_t)width * sizeof(uint16_t), (size_t)height)) {
				try {
					alpha16.resize((size_t)width * (size_t)height);
				} catch (...) {
					alpha16.clear();
				}
			}
			if (!alpha16.empty()) {
				for (int y = 0; y < height; y++) {
					const uint16_t* row = (const uint16_t*)(data + (size_t)y * stride);
					for (int x = 0; x < width; x++) {
						alpha16[(size_t)y * width + x] = (row + x * 4)[3];
					}
				}
				pAlpha = alpha16.data();
			}
		}
		std::vector<uint8_t> alternate;
		// P3 (user-added): pre-check the alternate view against the
		// byte cap, like the 8-bit out buffer below (same width*4
		// stride). On failure it stays empty and the loss is recorded
		// (R-10); the lazy 8-bit path recomputes on toggle only for
		// an SDR base.
		if (IsImageBufferSizeOk((size_t)width * 4, (size_t)height)) {
			try {
				alternate.resize((size_t)width * (size_t)height * 4);
			} catch (...) {
				alternate.clear();
			}
		}
		// R-10: record whether the accurate 16-bit alternate was
		// attempted and lost. A lost alpha plane counts too: with
		// transparent pixels the apply would install a silently
		// opaque alternate. When the flag is set, ComputeHdrBuffer
		// refuses the lazy 8-bit recompute for an HDR base (the
		// plan section 2.4-disproved inverse gain), so the image
		// stays HDR instead. Set regardless of direction -- the
		// refusal policy gates on direction, not the flag alone.
		const bool bAlphaLost = bHasTransparency && alpha16.empty();
		bool bPrecomputed = false;
		if (!bAlphaLost && !alternate.empty())
			bPrecomputed = gainmap::ApplyGainMapToBgra16(rgb, width, height, transfer, primaries,
				pGain->gray.data(), pGain->width, pGain->height, pGain->meta, wAlt,
				pAlpha, maxValInt, alternate.data(), (size_t)width * 4);
		if (bPrecomputed)
			pGain->alternateBgra = std::move(alternate);
		else
			pGain->accurateAlternateLost = true;
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
		const uint16_t* row = (const uint16_t*)(data + (size_t)y * stride);
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
			// P2-B4: copy the source alpha, scaled by the native
			// maxVal (bpp 8..16, so (1<<bpp)-1 fits in uint32_t;
			// maxValInt is computed in Pass 1 above).
			const uint16_t* px = row + x * 4;
			dst[3] = (uint8_t)(((uint32_t)px[3] * 255 + maxValInt / 2) / maxValInt);
		}
	}
	delete[] rgb;
	return out;
}

// Gain-map support (Option D, Phase 3): ISO 21496-1 / Apple HDR gain map.
//
// An iPhone HDR photo is an SDR base image plus a lower-resolution
// single-channel gain-map AUXILIARY image item (auxl reference) plus XMP
// metadata carrying the reconstruction parameters. The aux type URNs are
// "urn:com:apple:photo:2020:aux:hdrgainmap" (Apple, iOS 17+) and
// "urn:iso:std:iso:ts:21496:-1" (ISO); both are accepted (match on
// "hdrgainmap" or "21496"). NB-1: the base may be HDR (PQ/HLG) as
// well as SDR -- an HDR base carries a gain map too, and its alternate
// view is the SDR view (weight -1). The extraction runs on both paths.
//
// The C++ wrapper (heif_cxx.h) has no aux-image API, so the C API in
// heif_aux_images.h is used directly. heif::ImageHandle takes ownership
// of the raw aux handle (shared_ptr with heif_image_handle_release as
// deleter), so the raw handle must NOT be released manually. The aux
// type string is released with heif_image_handle_release_auxiliary_type.
//
// ExtractHeifGainMap fills the extracted gain map (grayscale +
// metadata) without applying it, or returns false when there is no
// gain map or anything fails -- the caller keeps the base BGRA
// unchanged in that case. A broken gain map must never fail the
// whole load.

// Find the gain-map aux image of `handle`. Returns the aux handle (as a
// heif::ImageHandle, which owns it) or an empty handle when there is
// none. When several aux images match, the one whose type contains
// "hdrgainmap" wins over one whose type only contains "21496".
static heif::ImageHandle FindGainMapAuxHandle(const heif::ImageHandle& handle)
{
	heif::ImageHandle fallback; // "21496"-only match, kept if no hdrgainmap match appears
	// LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA: gain maps are not alpha images.
	int nAux = heif_image_handle_get_number_of_auxiliary_images(handle.get_raw_image_handle(),
		LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA);
	if (nAux <= 0)
		return heif::ImageHandle();
	std::vector<heif_item_id> ids((size_t)nAux);
	int nListed = heif_image_handle_get_list_of_auxiliary_image_IDs(handle.get_raw_image_handle(),
		LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA, ids.data(), nAux);
	if (nListed <= 0)
		return heif::ImageHandle();
	for (int i = 0; i < nListed; i++) {
		heif_image_handle* rawAux = nullptr;
		heif_error err = heif_image_handle_get_auxiliary_image_handle(handle.get_raw_image_handle(), ids[(size_t)i], &rawAux);
		if (err.code != heif_error_Ok || rawAux == nullptr)
			continue;
		heif::ImageHandle auxHandle(rawAux); // takes ownership; do NOT release rawAux
		const char* type = nullptr;
		err = heif_image_handle_get_auxiliary_type(rawAux, &type);
		if (err.code != heif_error_Ok || type == nullptr)
			continue;
		std::string auxType(type);
		heif_image_handle_release_auxiliary_type(rawAux, &type);
		if (auxType.find("hdrgainmap") != std::string::npos)
			return auxHandle;
		if (fallback.empty() && auxType.find("21496") != std::string::npos)
			fallback = auxHandle;
	}
	return fallback;
}

// Decode the gain-map aux image to a packed single-channel 8-bit buffer
// (gainW x gainH bytes). Preferred: YCbCr + monochrome (the Y plane is
// the gain map). Fallback: RGB interleaved, R channel (R == G == B for a
// grey image). Returns false when neither decode yields a usable plane.
// `gainGray` holds the packed bytes on success. The gain map is usually
// smaller than the base; it must not be absurdly larger (sanity cap 4x
// per side — ApplyGainMap upsamples it bilinearly to base size).
static bool DecodeGainMapGray(heif::ImageHandle auxHandle, int baseWidth, int baseHeight,
                              std::vector<uint8_t>& gainGray, int& gainW, int& gainH)
{
	gainW = gainH = 0;
	// P2-B6: pre-check the NOMINAL gain-map dimensions BEFORE decoding.
	// decode_image allocates the full planes, so the post-decode 4x
	// checks below cannot bound that allocation. The nominal dims come
	// from the aux item's ispe box (libheif: "not guaranteed to match
	// the decoded plane", hence the post-decode checks are retained as
	// well). Rejects non-positive dims too (a missing ispe reads 0).
	// Real files: gain 2016x1512 vs base 4032x3024 = 0.5x (two iPhone
	// HEICs, verified with probe_gain; nominal == decoded there).
	int nominalW = auxHandle.get_width();
	int nominalH = auxHandle.get_height();
	if (nominalW <= 0 || nominalH <= 0 ||
		nominalW > baseWidth * 4 || nominalH > baseHeight * 4 ||
		!IsImageBufferSizeOk((size_t)nominalW, (size_t)nominalH)) {
		return false;
	}
	// Preferred: YCbCr monochrome — the Y plane is the gain map.
	try {
		heif::Image gainImage = auxHandle.decode_image(heif_colorspace_YCbCr, heif_chroma_monochrome);
		int gw = gainImage.get_width(heif_channel_Y);
		int gh = gainImage.get_height(heif_channel_Y);
		if (gw > 0 && gh > 0 && gw <= baseWidth * 4 && gh <= baseHeight * 4
			&& gainImage.get_bits_per_pixel(heif_channel_Y) == 8) {
			int stride = 0;
			const uint8_t* plane = gainImage.get_plane(heif_channel_Y, &stride);
			if (plane != nullptr && stride >= gw) {
				// P2-B6: bound the gainGray allocation itself (the
				// decoded dims need not match the nominal ones).
				// Returns false rather than falling through to the RGB
				// fallback, which would decode the same huge image
				// at 3 bytes/pixel.
				if (!IsImageBufferSizeOk((size_t)gw, (size_t)gh)) {
					return false;
				}
				gainGray.assign((size_t)gw * gh, 0);
				for (int y = 0; y < gh; y++)
					memcpy(&gainGray[(size_t)y * gw], plane + (size_t)y * stride, (size_t)gw);
				gainW = gw;
				gainH = gh;
				return true;
			}
		}
	} catch (...) {
		// Fall through to the RGB fallback below.
	}
	// Fallback: RGB interleaved; take the R channel (R == G == B for grey).
	try {
		heif::Image gainImage = auxHandle.decode_image(heif_colorspace_RGB, heif_chroma_interleaved_RGB);
		int gw = gainImage.get_width(heif_channel_interleaved);
		int gh = gainImage.get_height(heif_channel_interleaved);
		if (gw > 0 && gh > 0 && gw <= baseWidth * 4 && gh <= baseHeight * 4) {
			int stride = 0;
			const uint8_t* plane = gainImage.get_plane(heif_channel_interleaved, &stride);
			if (plane != nullptr && stride >= gw * 3) {
				// P2-B6: bound the gainGray allocation itself.
				if (!IsImageBufferSizeOk((size_t)gw, (size_t)gh)) {
					return false;
				}
				gainGray.assign((size_t)gw * gh, 0);
				for (int y = 0; y < gh; y++) {
					const uint8_t* srcRow = plane + (size_t)y * stride;
					uint8_t* dstRow = &gainGray[(size_t)y * gw];
					for (int x = 0; x < gw; x++)
						dstRow[x] = srcRow[(size_t)x * 3 + 0];
				}
				gainW = gw;
				gainH = gh;
				return true;
			}
		}
	} catch (...) {
	}
	return false;
}

// Try to parse gain-map metadata from the XMP ("mime",
// application/rdf+xml) metadata blocks of `handle`. Returns true and
// fills `meta` on the first XMP packet that parses; false when there is
// no XMP or none parses.
static bool TryParseGainMapXmp(const heif::ImageHandle& handle, gainmap::Metadata& meta)
{
	std::vector<heif_item_id> ids = handle.get_list_of_metadata_block_IDs("mime");
	for (size_t i = 0; i < ids.size(); i++) {
		std::string contentType = handle.get_metadata_content_type(ids[i]);
		if (contentType != "application/rdf+xml")
			continue;
		std::vector<uint8_t> raw;
		try {
			raw = handle.get_metadata(ids[i]);
		} catch (...) {
			continue;
		}
		if (raw.empty())
			continue;
		std::string xmp(raw.begin(), raw.end());
		if (gainmap::ParseXmpMetadata(xmp, meta))
			return true;
	}
	return false;
}

// Extract the gain map of a HEIF base image (SDR or HDR), if it has
// one. Fills `outGain` (grayscale + metadata) WITHOUT applying it; the
// load thread attaches it to the image and applies it on demand, or
// at load when GainMapDefault() asks for the non-base view. Returns
// false when there is no gain map or anything fails -- the caller
// keeps the base unchanged in that case. XMP is tried on the primary
// image first,
// then on the gain-map item itself (the gain-map item may carry its
// own XMP). Any exception from the libheif C++ wrapper
// (decode_image / get_metadata throw heif::Error) is caught here
// and treated as "no gain map". The fused apply moved out of this
// function; ApplyGainMapToBgra is now called from
// CJPEGImage::ComputeHdrBuffer. The 2026-11-21 early-return disable
// is gone: HEIF gain maps are re-enabled under the unified HDR
// toggle (INI default HDR, Ctrl+H to flip to SDR).
static bool ExtractHeifGainMap(const heif::ImageHandle& handle,
	int width, int height, gainmap::ExtractedGainMap& outGain)
{
	try {
		if (width <= 0 || height <= 0)
			return false;
		heif::ImageHandle auxHandle = FindGainMapAuxHandle(handle);
		if (auxHandle.empty())
			return false;
		std::vector<uint8_t> gainGray;
		int gainW = 0, gainH = 0;
		if (!DecodeGainMapGray(auxHandle, width, height, gainGray, gainW, gainH))
			return false;
		gainmap::Metadata meta;
		// XMP on the primary image first, then on the gain-map item.
		if (!TryParseGainMapXmp(handle, meta) && !TryParseGainMapXmp(auxHandle, meta)) {
			// No headroom metadata on either handle: the gain map has
			// no scale, so it cannot be applied -- show the SDR base.
			// (The Batch 7 derive-from-pixels fallback was removed by
			// user decision: without a scale the gain map is
			// uninterpretable, and Apple Photos shows these files
			// as SDR. Do not re-add a pixel-derived headroom.)
			return false;
		}
		outGain.gray = std::move(gainGray);
		outGain.width = gainW;
		outGain.height = gainH;
		outGain.meta = meta;
		outGain.valid = true;
		return true;
	} catch (...) {
		return false;
	}
}

void * HeifReader::ReadImage(int &width,
					   int &height,
					   int &nchannels,
					   int &frame_count,
					   void* &exif_chunk,
					   bool &outOfMemory,
					   bool &isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
					   int frame_index,
					   const void *buffer,
					   int sizebytes,
					   gainmap::ExtractedGainMap* pGainOut)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;

	unsigned char* pPixelData = NULL;
	exif_chunk = NULL;

	heif::Context context;
	context.read_from_memory_without_copy(buffer, sizebytes);
	frame_count = context.get_number_of_top_level_images();
	heif_item_id item_id = context.get_list_of_top_level_image_IDs().at(frame_index);
	heif::ImageHandle handle = context.get_image_handle(item_id);
	// HDR detection (Option A): the nclx (CICP) profile comes from the
	// image handle via the C API (heif::ImageHandle has no
	// get_nclx_color_profile; only heif::Image does, and the image
	// only exists after decode). heif_image_handle_get_nclx_color_
	// profile returns heif_error_Color_profile_does_not_exist when
	// there is no nclx (e.g. an ICC-profiled or untagged file), which
	// means not HDR. Only PQ/HLG take the HDR path: decode to 16-bit
	// and run the tone mapper. SDR keeps the 8-bit RGBA decode and
	// the exact pre-existing path. Gain-map HEIFs (iPhone HDR photos)
	// usually take the SDR path, but an HDR base can carry a gain map
	// too; ExtractHeifGainMap (below) extracts it on both paths, the
	// load thread attaches it and applies it on demand, or at load
	// when GainMapDefault() asks for the non-base view.
	HdrToneMap::Cicp cicp;
	bool isHdr = false;
	{
		heif_color_profile_nclx* nclx = nullptr;
		heif_error err = heif_image_handle_get_nclx_color_profile(handle.get_raw_image_handle(), &nclx);
		if (err.code == heif_error_Ok && nclx != nullptr) {
			cicp.primaries = (int)nclx->color_primaries;
			cicp.transfer = (int)nclx->transfer_characteristics;
			cicp.matrix = (int)nclx->matrix_coefficients;
			cicp.fullRange = nclx->full_range_flag ? 1 : 0;
			isHdr = HdrToneMap::IsHdr(cicp);
			heif_nclx_color_profile_free(nclx);
		}
	}
	// F-06: gate on the header dimensions BEFORE decoding. decode_image
	// below decodes the full frame; a 65535x65535 header would force
	// GB-scale decoder-internal allocation before the old post-decode check
	// ever ran. The post-decode checks below stay as a safety net.
	width = handle.get_width();
	height = handle.get_height();
	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): libheif only returns non-negative dimensions; unqualified
	// abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		return NULL;
	}
	// The decoded plane is 16-bit RGBA (8 bytes/pixel) for HDR and
	// 8-bit RGBA (4 bytes/pixel) for SDR; bound the plane, not just
	// the 8-bit output buffer below.
	if (!IsImageBufferSizeOk((size_t)width * (isHdr ? 8 : nchannels), (size_t)height)) {
		outOfMemory = true;
		return NULL;
	}
	heif::Image image = handle.decode_image(heif_colorspace_RGB,
		isHdr ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA);
	int stride;
	uint8_t* data = image.get_plane(heif_channel_interleaved, &stride);
	width = image.get_width(heif_channel_interleaved);
	height = image.get_height(heif_channel_interleaved);

	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
		return NULL;
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): libheif only returns non-negative dimensions; unqualified
	// abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB on overflow).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		return NULL;
	}
	if (width < 1 || height < 1 || width * (isHdr ? 8 : nchannels) > stride)
		return NULL;

	// int 乘法会在 32768x32768x4 这类尺寸下回绕成 0；用 64 位算并校验上限。
	size_t size = 0;
	if (!IsImageBufferSizeOk((size_t)width * nchannels, (size_t)height, &size)) {
		outOfMemory = true;
		return NULL;
	}
	pPixelData = new(std::nothrow) unsigned char[size];
	if (pPixelData == NULL) {
		outOfMemory = true;
		return NULL;
	}
	// NB-1: extract the gain map BEFORE the if/else below, on both
	// paths (the old code extracted it only in the SDR branch). The
	// extraction reads the aux image + XMP, not the base pixels, so
	// it does not depend on the HDR conversion; the HDR branch passes
	// it into ConvertHeifHdrToSdr so the alternate view is precomputed
	// from the 16-bit signal (scheme A). The gain is assigned to
	// *pGainOut after the if/else. The extraction is skipped entirely
	// when the caller does not want the gain map (pGainOut == NULL).
	gainmap::ExtractedGainMap gain;
	if (pGainOut != NULL) {
		try {
			ExtractHeifGainMap(handle, width, height, gain);
		} catch (...) {
			// Keep the base image; the gain map is optional.
		}
	}
	if (isHdr) {
		// HDR: tone-map the 16-bit RGBA plane to 8-bit BGRA. The bit
		// depth comes from the decoded image: libheif keeps the
		// native depth in the uint16_t samples (a 12-bit file gives
		// values in [0, 4095], not scaled to [0, 65535]), so the
		// helper normalises by (1<<bpp)-1. The ICC transform is
		// skipped for HDR (HDR HEIFs use nclx, not an ICC profile);
		// the tone mapper is the only colour conversion.
		// get_bits_per_pixel returns the STORAGE depth (16 for the
		// uint16_t RRGGBBAA_LE plane), but libheif keeps the native
		// depth in the samples (a 12-bit file gives 0..4095, not
		// scaled to 0..65535). get_bits_per_pixel_range returns the
		// actual value depth (m_bit_depth), which is what the
		// normalisation in ConvertHeifHdrToSdr needs. Using the
		// storage depth made every HDR HEIF 16x too dark (black).
		int bpp = image.get_bits_per_pixel_range(heif_channel_interleaved);
		if (bpp < 8 || bpp > 16)
			bpp = 16;
		unsigned char* hdrPixels = ConvertHeifHdrToSdr(data, stride, width, height, bpp, cicp, &gain);
		if (hdrPixels == NULL) {
			delete[] pPixelData;
			outOfMemory = true;
			return NULL;
		}
		memcpy(pPixelData, hdrPixels, size);
		delete[] hdrPixels;
	} else {
		std::vector<uint8_t> iccp = image.get_raw_color_profile();
		void* transform = ICCProfileTransform::CreateTransform(iccp.data(), iccp.size(), ICCProfileTransform::FORMAT_RGBA);
		size_t i, j;
		if (!ICCProfileTransform::DoTransform(transform, data, pPixelData, width, height, stride=stride)) {
			unsigned int* o = (unsigned int*)pPixelData;
			for (i = 0; i < height; i++) {
				unsigned int* p = (unsigned int*)(data + i * stride);
				for (j = 0; j < width; j++) {
					// RGBA -> BGRA conversion
					*o = _rotr(_byteswap_ulong(*p), 8);
					p++;
					o++;
				}
			}
		}
		ICCProfileTransform::DeleteTransform(transform);
		// (The gain map was extracted before the if/else above; the
		// SDR alternate view is computed lazily on toggle, from this
		// sRGB base, as before.)
	}
	// NB-1: assign the extracted gain here, at the old extraction
	// location, so the load thread attaches it (with the precomputed
	// alternate view, when present).
	if (pGainOut != NULL && gain.valid) {
		*pGainOut = std::move(gain);
	}

	std::vector<heif_item_id> exif_blocks = handle.get_list_of_metadata_block_IDs("Exif");

	if (!exif_blocks.empty()) {
		std::vector<uint8_t> exif = handle.get_metadata(exif_blocks[0]);
		// 65538 magic number comes from investigations by qbnu
		// see https://github.com/sylikc/jpegview/pull/213#pullrequestreview-1494451359 for more details
		/*
		* These libraries all have their own ideas about where to start the Exif data from.
		* JPEG Exif blocks are in the format
		  FF E1 SS SS 45 78 69 66 00 00 [data]

		  The SS SS is a big-endian unsigned short representing the size of everything after FF E1.

		  libjxl gives 00 00 00 00 [data]
		  libheif gives 00 00 00 00 45 78 69 66 00 00 [data]
		  libavif, libwebp and libpng give [data], so they have different limits for size.

		  If you want I can change it to 65536 + an offset and add notes explaining why.
		*/
		if (exif.size() > 8 && exif.size() < 65538) {
			exif_chunk = malloc(exif.size());
			if (exif_chunk != NULL) {
				memcpy(exif_chunk, exif.data(), exif.size());
				*((unsigned short*)exif_chunk) = _byteswap_ushort(0xFFE1);
				*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size() - 2);
			}
		}
	}

	return (void*)pPixelData;
}
