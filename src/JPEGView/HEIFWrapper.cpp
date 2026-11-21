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
// ignored (the tone mapper works on RGB); the output alpha is opaque.
static unsigned char* ConvertHeifHdrToSdr(const uint8_t* data, int stride, int width, int height, int bpp, const HdrToneMap::Cicp& cicp)
{
	size_t rgbSize = 0;
	if (!IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &rgbSize))
		return NULL;
	float* rgb = new(std::nothrow) float[(size_t)width * height * 3];
	if (rgb == NULL)
		return NULL;
	const double maxVal = (double)((1 << bpp) - 1);
	// Pass 1: 16-bit RGBA rows -> float RGB (0..1). RGBA order: R, G, B, A.
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
		}
	}
	HdrToneMap::Transfer transfer = HdrToneMap::TransferFromCode(cicp.transfer);
	HdrToneMap::Primaries primaries = HdrToneMap::PrimariesFromCode(cicp.primaries);
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
			dst[3] = 0xff;
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
// "hdrgainmap" or "21496"). A gain-map HEIF is SDR (isHdr == false), so
// this runs only on the SDR path; PQ/HLG HEIFs are untouched.
//
// The C++ wrapper (heif_cxx.h) has no aux-image API, so the C API in
// heif_aux_images.h is used directly. heif::ImageHandle takes ownership
// of the raw aux handle (shared_ptr with heif_image_handle_release as
// deleter), so the raw handle must NOT be released manually. The aux
// type string is released with heif_image_handle_release_auxiliary_type.
//
// TryApplyHeifGainMap returns a NEW 8-bit BGRA buffer with the gain map
// applied and tone-mapped via HdrToneMap, or NULL when there is no gain
// map or anything fails — the caller keeps the base BGRA unchanged in
// that case. A broken gain map must never fail the whole load.

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

// Apply the gain map of an SDR HEIF base image, if it has one. `baseBgra`
// is the already-decoded 8-bit BGRA base (width x height). Returns a NEW
// 8-bit BGRA buffer with the gain map applied and tone-mapped via
// HdrToneMap::ConvertLinearImage, or NULL when there is no gain map or
// anything fails — the caller keeps `baseBgra` unchanged in that case.
// XMP is tried on the primary image first, then on the gain-map item
// itself (the gain-map item may carry its own XMP). Any exception from
// the libheif C++ wrapper (decode_image / get_metadata throw
// heif::Error) is caught here and treated as "no gain map".
static unsigned char* TryApplyHeifGainMap(const heif::ImageHandle& handle,
                                          const unsigned char* baseBgra,
                                          int width, int height)
{
	unsigned char* outBgra = NULL;
	try {
		if (baseBgra == nullptr || width <= 0 || height <= 0)
			return NULL;
		heif::ImageHandle auxHandle = FindGainMapAuxHandle(handle);
		if (auxHandle.empty())
			return NULL;
		std::vector<uint8_t> gainGray;
		int gainW = 0, gainH = 0;
		if (!DecodeGainMapGray(auxHandle, width, height, gainGray, gainW, gainH))
			return NULL;
		gainmap::Metadata meta;
		// XMP on the primary image first, then on the gain-map item.
		if (!TryParseGainMapXmp(handle, meta) && !TryParseGainMapXmp(auxHandle, meta))
			return NULL;
		// Base BGRA -> RGB temp buffer (drop alpha), then reconstruct
		// linear HDR floats, tone-map to BGR, and repack to BGRA
		// (alpha opaque). ConvertLinearImage outputs BGR; the HEIF
		// path needs BGRA, so the repack is explicit.
		size_t rgbSize = 0, floatSize = 0, bgraSize = 0;
		if (!IsImageBufferSizeOk((size_t)width * 3, (size_t)height, &rgbSize)
			|| !IsImageBufferSizeOk((size_t)width * 3 * sizeof(float), (size_t)height, &floatSize)
			|| !IsImageBufferSizeOk((size_t)width * 4, (size_t)height, &bgraSize))
			return NULL;
		std::vector<uint8_t> rgb(rgbSize);
		std::vector<float> linear(floatSize / sizeof(float));
		std::vector<uint8_t> bgr(rgbSize);
		for (size_t i = 0; i < (size_t)width * height; i++) {
			rgb[i * 3 + 0] = baseBgra[i * 4 + 2];
			rgb[i * 3 + 1] = baseBgra[i * 4 + 1];
			rgb[i * 3 + 2] = baseBgra[i * 4 + 0];
		}
		if (!gainmap::ApplyGainMap(rgb.data(), width, height, gainGray.data(), gainW, gainH, meta, 1.0, linear.data()))
			return NULL;
		HdrToneMap::ConvertLinearImage(linear.data(), width, height, (size_t)width * 3, bgr.data(), (size_t)width * 3);
		outBgra = new(std::nothrow) unsigned char[bgraSize];
		if (outBgra == NULL)
			return NULL;
		for (size_t i = 0; i < (size_t)width * height; i++) {
			outBgra[i * 4 + 0] = bgr[i * 3 + 0];
			outBgra[i * 4 + 1] = bgr[i * 3 + 1];
			outBgra[i * 4 + 2] = bgr[i * 3 + 2];
			outBgra[i * 4 + 3] = 0xff;
		}
		return outBgra;
	} catch (...) {
		delete[] outBgra;
		return NULL;
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
					   int sizebytes)
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
	// take the SDR path too, and TryApplyHeifGainMap (below) applies
	// the gain map when one is present.
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
	if (width < 1 || height < 1 || width * nchannels > stride)
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
		unsigned char* hdrPixels = ConvertHeifHdrToSdr(data, stride, width, height, bpp, cicp);
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
		// Gain-map (Option D, Phase 3): an SDR HEIF with a gain-map aux
		// image + XMP is an iPhone HDR photo -- reconstruct the HDR
		// image and tone-map it. TryApplyHeifGainMap returns NULL when
		// there is no gain map or anything fails; the SDR base is kept
		// unchanged in that case.
		unsigned char* gainMapped = TryApplyHeifGainMap(handle, pPixelData, width, height);
		if (gainMapped != NULL) {
			delete[] pPixelData;
			pPixelData = gainMapped;
		}
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
