#include "stdafx.h"

#include "SaveJP2.h"
// Atomic-save helpers: the temp file is created in the target's directory
// (same volume, so ReplaceFile works) and only swapped into place on full
// success; on any failure the temp file is deleted and the target is left
// untouched. Declared in SaveImage.h, defined in SaveImage.cpp.
#include "SaveImage.h"
// OpenJPEG 2.5, vendored under libopenjpeg/ (official Windows binaries,
// v2.5.4): include/openjpeg-2.5/openjpeg.h, openjp2.lib in lib (Win32) /
// lib64 (x64), openjp2.dll delay-loaded via the vcxproj DelayLoadDLLs entry,
// so the DLL is only loaded when a JP2 file is actually saved.
#include "openjpeg-2.5/openjpeg.h"
#include <string.h>

namespace {

// OpenJPEG prints encoder messages to stderr by default; silence it so a
// save cannot spam the console (or a debugger) during browsing. Same
// handler shape as the decode side (JP2Wrapper.cpp).
void SaveJP2NoOpMsg(const char* /*msg*/, void* /*client_data*/) {
}

// .j2k/.j2c are raw codestreams; anything else (normally .jp2) is the JP2
// container. The caller only invokes this for IF_JP2, so an unexpected
// suffix falls back to the container rather than failing the save.
bool IsRawCodestreamFileName(LPCTSTR sFileName) {
	LPCTSTR sDot = _tcsrchr(sFileName, _T('.'));
	if (sDot == NULL)
		return false;
	return _tcsicmp(sDot, _T(".j2k")) == 0 || _tcsicmp(sDot, _T(".j2c")) == 0;
}

} // namespace

// pData24bppBGR must point to a 24 bit BGR DIB: 3 bytes per pixel, rows
// padded to a 4-byte boundary, row 0 is the top row (the same buffer
// SaveWebP/SaveQOI receive from SaveImage.cpp).
bool SaveJP2File(LPCTSTR sFileName, void* pData24bppBGR, int nWidth, int nHeight, bool bLossless) {
	// All locals are declared up here, before any goto: a goto jumping
	// forward over an initialised local is C2360 on MSVC.
	bool bSuccess = false;
	const unsigned char* pSrc = NULL;
	size_t nStride = 0;
	CString sTempFile;
	char szTempA[MAX_PATH];
	BOOL bUsedDefaultChar = FALSE;
	OPJ_CODEC_FORMAT eFormat = OPJ_CODEC_JP2;
	opj_codec_t* pCodec = NULL;
	opj_image_t* pImage = NULL;
	opj_stream_t* pStream = NULL;
	opj_cparameters_t params;
	opj_image_cmptparm_t cmptParms[3];

	if (sFileName == NULL || sFileName[0] == 0 || pData24bppBGR == NULL || nWidth <= 0 || nHeight <= 0)
		return false;
	pSrc = (const unsigned char*)pData24bppBGR;
	// R-05: byte offsets in size_t. The stride is computed in size_t
	// rather than via the int Helpers::DoPadding.
	nStride = ((size_t)nWidth * 3 + 3) & ~(size_t)3;
	eFormat = IsRawCodestreamFileName(sFileName) ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;

	// Path-only overload (GDI+ path precedent): the temp file is created
	// with CREATE_NEW and the handle closed again right away; OpenJPEG
	// opens the reserved name by file name below. The TOCTOU window that
	// leaves is the same one SaveGDIPlus already accepts.
	if (!CreateAtomicTempFileName(sFileName, sTempFile))
		return false;

	// opj_stream_create_default_file_stream takes a narrow-char file name
	// and opens it with fopen, so the path must be converted from the
	// wide temp path. CP_ACP (system ANSI code page) is used, the same
	// precedent as dcraw_mod.cpp for file names handed to C-library file
	// APIs; CP_UTF8 would be wrong here because fopen interprets the
	// bytes in the ANSI code page. Limitation: a path containing a
	// character that is not representable in the system ANSI code page
	// cannot be saved this way. WC_NO_BEST_FIT_CHARS plus
	// lpUsedDefaultChar makes that a clean failure instead of silently
	// writing to a '?' mangled name: the save is refused and the target
	// is left untouched. (The temp file name itself, ~jv<pid>_<n>.tmp, is
	// pure ASCII; only non-ASCII directory components can hit this.)
	if (::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, sTempFile, -1, szTempA, MAX_PATH, NULL, &bUsedDefaultChar) == 0 || bUsedDefaultChar) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	pCodec = opj_create_compress(eFormat);
	if (pCodec == NULL)
		goto cleanup;
	opj_set_info_handler(pCodec, SaveJP2NoOpMsg, NULL);
	opj_set_warning_handler(pCodec, SaveJP2NoOpMsg, NULL);
	opj_set_error_handler(pCodec, SaveJP2NoOpMsg, NULL);
	// Single-threaded encode, like the decode side: deterministic, and
	// immune to a stray OPJ_NUM_THREADS environment variable spawning
	// threads in the save path.
	opj_codec_set_threads(pCodec, 1);

	opj_set_default_encoder_parameters(&params);
	if (!bLossless) {
		// Lossy: irreversible 9-7 wavelet, one layer, rate allocation.
		// The other defaults (6 resolutions, 64x64 code-blocks, LRCP
		// progression, single tile, MCT on) are kept.
		params.tcp_numlayers = 1;
		params.tcp_rates[0] = 10.0f;
		params.cp_disto_alloc = 1;
		params.irreversible = 1;
	}
	// Lossless: the defaults are already lossless (reversible 5-3
	// wavelet, irreversible == 0); nothing to change.

	memset(cmptParms, 0, sizeof(cmptParms));
	for (int c = 0; c < 3; c++) {
		cmptParms[c].dx = 1;
		cmptParms[c].dy = 1;
		cmptParms[c].w = (OPJ_UINT32)nWidth;
		cmptParms[c].h = (OPJ_UINT32)nHeight;
		cmptParms[c].x0 = 0;
		cmptParms[c].y0 = 0;
		cmptParms[c].prec = 8;
		cmptParms[c].sgnd = 0;
		// bpp is deliberately untouched: it is the obsolete field ("Use
		// prec instead"); memset already zeroed it and the encoder reads
		// prec, not bpp.
	}
	// opj_image_create allocates the image AND each component's data
	// buffer (opj_image_tile_create is the variant that does not); the
	// buffers are released by opj_image_destroy.
	pImage = opj_image_create(3, cmptParms, OPJ_CLRSPC_SRGB);
	if (pImage == NULL || pImage->comps == NULL ||
		pImage->comps[0].data == NULL || pImage->comps[1].data == NULL || pImage->comps[2].data == NULL)
		goto cleanup;

	// Fill the components: BGR DIB -> RGB, row-preserving. Row 0 is the
	// top row on both sides: the input buffer is the same top-down 24bpp
	// DIB SaveWebP/SaveQOI hand to the top-down WebP/QOI encoders with no
	// flip (and CBasicProcessing::Convert32bppTo24bppDIB is only called
	// with bFlip=true for the bottom-up clipboard DIB), and OpenJPEG
	// component data is row-major top-down. This mirrors
	// QoiReaderWriter::Compress, which also copies row-preserving with a
	// BGR->RGB swap.
	for (int y = 0; y < nHeight; y++) {
		OPJ_INT32* pR = pImage->comps[0].data + (size_t)y * (size_t)nWidth;
		OPJ_INT32* pG = pImage->comps[1].data + (size_t)y * (size_t)nWidth;
		OPJ_INT32* pB = pImage->comps[2].data + (size_t)y * (size_t)nWidth;
		const unsigned char* pSrcRow = pSrc + (size_t)y * nStride;
		for (int x = 0; x < nWidth; x++) {
			pR[x] = pSrcRow[(size_t)x * 3 + 2];
			pG[x] = pSrcRow[(size_t)x * 3 + 1];
			pB[x] = pSrcRow[(size_t)x * 3 + 0];
		}
	}

	if (!opj_setup_encoder(pCodec, &params, pImage))
		goto cleanup;
	pStream = opj_stream_create_default_file_stream(szTempA, OPJ_FALSE);
	if (pStream == NULL)
		goto cleanup;
	if (!opj_start_compress(pCodec, pImage, pStream))
		goto cleanup;
	if (!opj_encode(pCodec, pStream))
		goto cleanup;
	if (!opj_end_compress(pCodec, pStream))
		goto cleanup;
	bSuccess = true;

cleanup:
	// opj_image_destroy frees the component data buffers allocated by
	// opj_image_create. opj_stream_destroy frees the stream and closes
	// the FILE* the default file stream opened (its user-data free
	// function is fclose), so the temp file is closed before the commit
	// below renames it; a still-open temp file would make ReplaceFile /
	// MoveFile fail. opj_destroy_codec frees the compressor.
	if (pImage != NULL)
		opj_image_destroy(pImage);
	if (pStream != NULL)
		opj_stream_destroy(pStream);
	if (pCodec != NULL)
		opj_destroy_codec(pCodec);
	if (!bSuccess) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	if (!CommitAtomicTempFile(sFileName, sTempFile)) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	return true;
}
