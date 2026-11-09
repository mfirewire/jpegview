#include "stdafx.h"

#include "JP2Wrapper.h"
#include "ICCProfileTransform.h"
// OpenJPEG 2.5, vendored under libopenjpeg/ (official Windows binaries,
// v2.5.4): include/openjpeg-2.5/openjpeg.h, openjp2.lib in lib (Win32) /
// lib64 (x64), openjp2.dll delay-loaded via the vcxproj DelayLoadDLLs entry,
// so the DLL is only loaded when a JP2 file is actually opened.
#include "openjpeg-2.5/openjpeg.h"
#include "MaxImageDef.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include <new>

namespace {

// In-memory stream backing for OpenJPEG: the decoder reads through these
// callbacks instead of a FILE*, so no temporary file is needed.
struct JP2MemStream {
	const unsigned char* data;
	OPJ_SIZE_T size;
	OPJ_SIZE_T pos;
};

OPJ_SIZE_T JP2Read(void* p_buffer, OPJ_SIZE_T p_nb_bytes, void* p_user_data) {
	JP2MemStream* s = (JP2MemStream*)p_user_data;
	if (s->pos >= s->size)
		return (OPJ_SIZE_T)-1; // EOF
	OPJ_SIZE_T remaining = s->size - s->pos;
	OPJ_SIZE_T n = p_nb_bytes < remaining ? p_nb_bytes : remaining;
	memcpy(p_buffer, s->data + s->pos, n);
	s->pos += n;
	return n;
}

OPJ_OFF_T JP2Skip(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
	JP2MemStream* s = (JP2MemStream*)p_user_data;
	if (p_nb_bytes < 0)
		return -1;
	OPJ_SIZE_T remaining = s->size - s->pos;
	OPJ_SIZE_T n = (OPJ_SIZE_T)p_nb_bytes < remaining ? (OPJ_SIZE_T)p_nb_bytes : remaining;
	s->pos += n;
	return (OPJ_OFF_T)n;
}

OPJ_BOOL JP2Seek(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
	JP2MemStream* s = (JP2MemStream*)p_user_data;
	if (p_nb_bytes < 0 || (OPJ_SIZE_T)p_nb_bytes > s->size)
		return OPJ_FALSE;
	s->pos = (OPJ_SIZE_T)p_nb_bytes;
	return OPJ_TRUE;
}

// OpenJPEG prints decoder messages to stderr by default; silence it so a
// corrupt JP2 cannot spam the console (or a debugger) during browsing.
void JP2NoOpMsg(const char* /*msg*/, void* /*client_data*/) {
}

// One component sample scaled to 8 bit. prec is 1..16; signed samples are
// re-biased to unsigned first. (For prec < 8 the value is stretched to the
// full 0..255 range; for prec > 8 the low bits are dropped.)
inline unsigned char JP2SampleTo8(OPJ_INT32 v, OPJ_UINT32 prec, OPJ_UINT32 sgnd) {
	if (sgnd)
		v += (OPJ_INT32)(1u << (prec - 1));
	if (v < 0)
		v = 0;
	OPJ_INT32 maxVal = (OPJ_INT32)((1u << prec) - 1);
	if (v > maxVal)
		v = maxVal;
	if (prec > 8)
		return (unsigned char)(v >> (prec - 8));
	if (prec < 8)
		return (unsigned char)((v * 255) / maxVal);
	return (unsigned char)v;
}

} // namespace

void* JP2Reader::ReadImage(int& width,
	int& height,
	bool& outOfMemory,
	bool& isSizeLimitExceeded, // R-04/P2-5: pixel-cap rejection, not OOM
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	if (buffer == NULL || sizebytes < 4)
		return NULL;
	// JP2-only gate, before OpenJPEG ever sees the buffer: the magic must
	// be the JP2 signature box (00 00 00 0C 'j' 'P' ' ' ' ', 12 bytes) or
	// a raw codestream (FF 4F FF 51). This rejects any non-JP2 content
	// misnamed .jp2/.j2k/.j2c.
	const unsigned char* pMagic = (const unsigned char*)buffer;
	OPJ_CODEC_FORMAT codecFormat;
	if (sizebytes >= 12 && pMagic[0] == 0x00 && pMagic[1] == 0x00 && pMagic[2] == 0x00 && pMagic[3] == 0x0C &&
		pMagic[4] == 0x6A && pMagic[5] == 0x50 && pMagic[6] == 0x20 && pMagic[7] == 0x20) {
		codecFormat = OPJ_CODEC_JP2;
	} else if (pMagic[0] == 0xFF && pMagic[1] == 0x4F && pMagic[2] == 0xFF && pMagic[3] == 0x51) {
		codecFormat = OPJ_CODEC_J2K;
	} else {
		return NULL;
	}

	opj_codec_t* codec = opj_create_decompress(codecFormat);
	if (codec == NULL)
		return NULL;
	opj_dparameters_t params;
	opj_set_default_decoder_parameters(&params);
	// Single-threaded decode: deterministic in the load thread, and immune
	// to a stray OPJ_NUM_THREADS environment variable spawning threads.
	opj_codec_set_threads(codec, 1);
	opj_set_info_handler(codec, JP2NoOpMsg, NULL);
	opj_set_warning_handler(codec, JP2NoOpMsg, NULL);
	opj_set_error_handler(codec, JP2NoOpMsg, NULL);
	if (!opj_setup_decoder(codec, &params)) {
		opj_destroy_codec(codec);
		return NULL;
	}

	JP2MemStream memStream;
	memStream.data = pMagic;
	memStream.size = (OPJ_SIZE_T)sizebytes;
	memStream.pos = 0;
	opj_stream_t* stream = opj_stream_create(4096, OPJ_TRUE);
	if (stream == NULL) {
		opj_destroy_codec(codec);
		return NULL;
	}
	opj_stream_set_read_function(stream, JP2Read);
	opj_stream_set_skip_function(stream, JP2Skip);
	opj_stream_set_seek_function(stream, JP2Seek);
	opj_stream_set_user_data(stream, &memStream, NULL);
	opj_stream_set_user_data_length(stream, (OPJ_UINT64)sizebytes);

	opj_image_t* image = NULL;
	void* pPixelData = NULL;
	// Size gate BEFORE decode (F-06/F-07 lesson): read the dimensions
	// from the header only, and refuse absurd dimensions before
	// OpenJPEG allocates a decode buffer for them.
	if (!opj_read_header(stream, codec, &image) || image == NULL) {
		goto cleanup;
	}
	// A3: reject an inverted reference grid BEFORE the subtraction; (int)(x1-x0)
	// with x1 < x0 wraps to a value that can pass the positivity test below on a
	// 32-bit build. The post-decode re-check further down stays as it is.
	if (image->x1 <= image->x0 || image->y1 <= image->y0) {
		goto cleanup;
	}
	width = (int)(image->x1 - image->x0);
	height = (int)(image->y1 - image->y0);
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION) {
		goto cleanup;
	}
	// R-04/P2-5: exceeding the pixel cap is a size-limit rejection, not out-of-memory.
	// No abs(): width/height are proven > 0 by the check above;
	// unqualified abs(double) resolves to ::abs(int) (abs(INT_MIN) is UB).
	if ((double)width * (double)height > (double)CSettingsProvider::This().MaxImagePixels()) {
		isSizeLimitExceeded = true;
		goto cleanup;
	}
	// Only component counts we know how to map to BGR. (2 = gray+alpha,
	// alpha is dropped below.)
	if (image->numcomps < 1 || image->numcomps > 4) {
		goto cleanup;
	}
	// Palette images
	// are NOT rejected here and NOT expanded here either: the wrapper calls
	// opj_set_default_decoder_parameters() and never writes params.flags,
	// so OPJ_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG (openjpeg.h, 0x0001 -
	// set it to make the decoder ignore PCLR/CMAP/CDEF) is not set by us
	// and any PCLR/CMAP/CDEF handling is left to OpenJPEG's default
	// behaviour. That default is NOT verified against decoder source
	// (not vendored), and no palette JP2 sample exists to test against:
	// the only JP2 sample (samples/false/sample1.jp2) has jp2h boxes
	// ihdr+colr only, no pclr/cmap. If OpenJPEG does not expand the
	// palette, a palette image arrives here as a 1-component index and
	// is rendered as grey by the isGray path below. Known limitation
	// (R-21) until a palette sample is verified on a real machine.
	// CMYK is decoded below (four ink components). A CMYK flag with any
	// other component count is malformed.
	if (image->color_space == OPJ_CLRSPC_CMYK && image->numcomps != 4) {
		goto cleanup;
	}
	for (OPJ_UINT32 c = 0; c < image->numcomps; c++) {
		opj_image_comp_t* comp = &image->comps[c];
		if (comp->prec < 1 || comp->prec > 16 || comp->dx < 1 || comp->dy < 1 ||
			comp->w == 0 || comp->h == 0) {
			goto cleanup;
		}
	}
	if (!opj_decode(codec, stream, image)) {
		goto cleanup;
	}
	// opj_decode keeps the header dimensions; re-check after decode.
	if (image->x1 <= image->x0 || image->y1 <= image->y0 ||
		(int)(image->x1 - image->x0) != width || (int)(image->y1 - image->y0) != height) {
		goto cleanup;
	}
	{
		// Output is 3-byte BGR padded to a 4-byte boundary, like QOIWrapper.
		// width <= MAX_IMAGE_DIMENSION (65535), so width * 3 fits in int.
		size_t padded_stride = (size_t)Helpers::DoPadding(width * 3, 4);
		size_t allocSize = 0;
		if (!IsImageBufferSizeOk(padded_stride, (size_t)height, &allocSize)) {
			outOfMemory = true;
			goto cleanup;
		}
		pPixelData = new(std::nothrow) unsigned char[allocSize];
		if (pPixelData == NULL) {
			outOfMemory = true;
			goto cleanup;
		}
		unsigned char* pDst = (unsigned char*)pPixelData;
		// Zero the row padding so no uninitialised bytes reach the DIB.
		memset(pDst, 0, allocSize);
		bool isYCbCr = (image->color_space == OPJ_CLRSPC_SYCC || image->color_space == OPJ_CLRSPC_EYCC);
		bool isGray = (image->numcomps == 1 || image->numcomps == 2);
		// CMYK JP2 (colr enumCS 12): the components are C,M,Y,K ink amounts
		// (0 = no ink), so the naive ink formula applies directly. Unlike PSD,
		// whose channel planes store the inverted value, these are not inverted.
		bool isCMYK = (image->color_space == OPJ_CLRSPC_CMYK);
		// Prefer the bundled cmyk.icm for the ink->RGB mapping (the same
		// accuracy as the PSD path). The naive formula below is only the
		// fallback when the profile, the transform or the buffer is missing.
		void* cmykTransform = NULL;
		unsigned char* pCmyk = NULL;
		if (isCMYK) {
			cmykTransform = ICCProfileTransform::CreateTransform(NULL, 0, ICCProfileTransform::FORMAT_CMYK);
			if (cmykTransform != NULL) {
				pCmyk = new(std::nothrow) unsigned char[(size_t)width * (size_t)height * 4];
				if (pCmyk == NULL) {
					ICCProfileTransform::DeleteTransform(cmykTransform);
					cmykTransform = NULL;
				}
			}
		}
		// S-6: find the alpha component (OpenJPEG marks it) so it can be
		// composited over white instead of being dropped. The per-pixel
		// buffer holds at most 8 components; a file declaring more used
		// to overflow it, so the loop is capped as well.
		const OPJ_UINT32 nComps = (image->numcomps > 8) ? 8 : image->numcomps;
		int nAlphaComp = -1;
		for (OPJ_UINT32 c = 0; c < nComps; c++) {
			if (image->comps[c].alpha != 0) { nAlphaComp = (int)c; break; }
		}
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				// Map the output pixel to each component's sample grid.
				// Samples are at reference-grid positions x0 + i*dx, so the
				// sample index is ((x0 + x) - comp->x0) / dx, clamped; this
				// is exact for dx == dy == 1 and nearest-neighbour
				// upsampling for subsampled components.
				unsigned char v[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
				for (OPJ_UINT32 c = 0; c < nComps; c++) {
					opj_image_comp_t* comp = &image->comps[c];
					// A1-2: x0/y0 and comp->x0/comp->y0 are OPJ_UINT32; casting
					// them to int wraps at 2^31. Compute the sample index in
					// __int64 and clamp BEFORE narrowing to int: casting first
					// and clamping after would wrap a >= 2^31 index to a
					// negative int and clamp it to the wrong edge (0 instead
					// of w-1). The int clamps below stay as defence in depth.
					__int64 col64 = ((__int64)image->x0 + x - (__int64)comp->x0) / (__int64)comp->dx;
					__int64 row64 = ((__int64)image->y0 + y - (__int64)comp->y0) / (__int64)comp->dy;
					if (col64 < 0) col64 = 0;
					if (row64 < 0) row64 = 0;
					if (col64 >= comp->w) col64 = (__int64)comp->w - 1;
					if (row64 >= comp->h) row64 = (__int64)comp->h - 1;
					int col = (int)col64;
					int row = (int)row64;
					if (col < 0) col = 0;
					if (row < 0) row = 0;
					if (col >= (int)comp->w) col = (int)comp->w - 1;
					if (row >= (int)comp->h) row = (int)comp->h - 1;
					v[c] = JP2SampleTo8(comp->data[(size_t)row * comp->w + (size_t)col], comp->prec, comp->sgnd);
				}
				if (pCmyk != NULL) {
					// ICC route: interleave C,M,Y,K and let the transform write BGR.
					unsigned char* d = pCmyk + ((size_t)y * (size_t)width + (size_t)x) * 4;
					d[0] = v[0]; d[1] = v[1]; d[2] = v[2]; d[3] = v[3];
					continue;
				}
				size_t dstIdx = (size_t)y * padded_stride + (size_t)x * 3;
				int r, g, b;
				if (isGray) {
					// All three channels from the gray component.
					r = g = b = v[0];
				} else if (isCMYK) {
					const int inkK = 255 - v[3];
					r = (255 - v[0]) * inkK / 255; // R from C
					g = (255 - v[1]) * inkK / 255; // G from M
					b = (255 - v[2]) * inkK / 255; // B from Y
				} else if (isYCbCr) {
					// BT.601 YCbCr -> RGB (Cb/Cr centred on 128). EYCC uses
					// the same coefficients here; that is an approximation,
					// documented rather than silently skipped.
					int yv = v[0];
					int cb = (int)v[1] - 128;
					int cr = (int)v[2] - 128;
					r = yv + (int)(1.402 * cr);
					g = yv - (int)(0.34414 * cb) - (int)(0.71414 * cr);
					b = yv + (int)(1.772 * cb);
					if (r < 0) r = 0; else if (r > 255) r = 255;
					if (g < 0) g = 0; else if (g > 255) g = 255;
					if (b < 0) b = 0; else if (b > 255) b = 255;
				} else {
					// OpenJPEG returns RGB; JPEGView pixels are BGR.
					r = v[0];
					g = v[1];
					b = v[2];
				}
				// S-6: composite over white, like the SVG and EXR decoders.
				if (nAlphaComp >= 0 && !isCMYK) {
					const int a = v[nAlphaComp];
					if (a != 255) {
						r = (r * a + 255 * (255 - a) + 127) / 255;
						g = (g * a + 255 * (255 - a) + 127) / 255;
						b = (b * a + 255 * (255 - a) + 127) / 255;
					}
				}
				pDst[dstIdx] = (unsigned char)b;
				pDst[dstIdx + 1] = (unsigned char)g;
				pDst[dstIdx + 2] = (unsigned char)r;
			}
		}
		if (pCmyk != NULL) {
			if (!ICCProfileTransform::DoTransform(cmykTransform, pCmyk, pPixelData,
				(unsigned int)width, (unsigned int)height, (unsigned int)((size_t)width * 4))) {
				// Transform failed: convert the interleaved planes directly.
				for (int y = 0; y < height; y++) {
					const unsigned char* s = pCmyk + (size_t)y * (size_t)width * 4;
					unsigned char* d = (unsigned char*)pPixelData + (size_t)y * padded_stride;
					for (int x = 0; x < width; x++, s += 4, d += 3) {
						const int inkK = 255 - s[3];
						d[0] = (unsigned char)((255 - s[2]) * inkK / 255);
						d[1] = (unsigned char)((255 - s[1]) * inkK / 255);
						d[2] = (unsigned char)((255 - s[0]) * inkK / 255);
					}
				}
			}
			delete[] pCmyk;
			ICCProfileTransform::DeleteTransform(cmykTransform);
		}
	}

cleanup:
	if (image != NULL)
		opj_image_destroy(image);
	opj_stream_destroy(stream);
	opj_destroy_codec(codec);
	return pPixelData;
}
