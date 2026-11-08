#include "stdafx.h"

// stdafx.h pulls in max/min macros via ATL/windows.h, and tinyexr.h uses
// std::numeric_limits<int>::max() / std::max(...), which the macros would
// break (C2589). Same undef dance as EXRWrapper.cpp (see there).
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include "SaveEXR.h"
// NOTE: TINYEXR_IMPLEMENTATION is defined exactly once, in
// EXRWrapper.cpp:26 (the decode side). This file only uses the
// declarations below; defining it here as well would duplicate every
// tinyexr symbol at link time.
#include "tinyexr/tinyexr.h"
#include "SaveImage.h" // CreateAtomicTempFileName / DeleteAtomicTempFile / CommitAtomicTempFile
#include "Helpers.h" // DoPadding
#include "MaxImageDef.h" // IsImageBufferSizeOk
#include <new> // std::nothrow
#include <stdio.h> // fwrite / fclose
#include <limits.h> // INT_MAX

// Saves a 24-bit BGR DIB as OpenEXR via SaveEXRToMemory + FILE*.
//
// Why SaveEXRToMemory and not SaveEXR(filename): SaveEXR takes a
// `const char*` (narrow) filename, so under the Unicode build the temp
// path would need a lossy wide -> narrow conversion and any non-ASCII
// directory would fail or land in the wrong place; it is also marked
// @deprecated in tinyexr.h (still usable, but there is no reason to
// prefer it). Encoding to memory first and writing the bytes with
// FILE* keeps the full Unicode path, the same reason SaveWebP/SaveJXL
// write through a FILE* instead of a filename API.
//
// The temp file comes from the HANDLE overload of
// CreateAtomicTempFileName, wrapped in a FILE* by the shared
// OpenTempFileForWriting (SaveImage.cpp, declared in SaveImage.h), so
// there is no create-close-reopen TOCTOU window here (P3-1).
//
// Atomicity (F-B01): the target itself is never opened for writing.
// The EXR bytes go to a temp file in the target's directory and are
// only swapped into place by CommitAtomicTempFile on full success; on
// any failure the temp file is deleted and the target is left
// byte-identical.
//
// Encoding choices (all fixed by tinyexr, nothing configurable here):
// - Components: 3 (RGB), no alpha -- the DIB has none.
// - Precision: fp16 (save_as_fp16 = 1). An 8-bit source value v is
//   stored as the fp16 nearest to its linear-light value
//   (v/255)^2.2 in [0, 1] (see Helpers::LinearFromSRGB). The
//   round trip back to 8-bit is pixel-identical: verified over all
//   256 values, every v reloads as v (the worst-case decoded value
//   sits 0.46 of an 8-bit step from a rounding boundary, so fp16 /
//   powf rounding cannot flip a value). fp32 would double the
//   file size for no extra information.
// - Compression: chosen inside tinyexr (SaveEXRToMemory,
//   tinyexr.h): ZIP (via the vendored miniz) whenever width >= 16 or
//   height >= 16, NONE when both dimensions are < 16. There is no
//   parameter to change it.
// - Dynamic range: none beyond 8-bit. The float buffer holds the
//   linear-light values (v/255)^2.2 in [0, 1]: EXR is a linear-light
//   format, so the 8-bit sRGB input must be linearised on save --
//   the same conversion SaveHDR applies (Helpers::LinearFromSRGB).
//   The earlier note claiming the round trip was "not pixel-
//   identical by design" and "a property of the EXR decode path"
//   was wrong: the asymmetry was on this writer's side (it stored
//   v/255, i.e. sRGB values labelled as linear light), while the
//   decode side correctly applied gamma 2.2. With the writer fixed
//   the round trip IS pixel-identical (see the Precision bullet).
//
// Row order / channels: the DIB is top-down (row 0 = top) and BGR.
// SaveEXRToMemory expects interleaved RGB float, row-major, pixel
// i = y * width + x, row 0 first, with no flip in its channel-split
// loop (and the EXR scanline order is increasing Y, the same
// convention the decode side relies on when it writes DIB row y from
// float row y). So DIB row y maps straight to EXR row y, with only
// the BGR -> RGB channel swap -- the same convention as the
// QOI/JXL/WebP Compress implementations, which all consume row 0
// first with no flip.
//
// Memory / release responsibilities (every path frees what it owns):
// - pFloatRGB: owned here, new(std::nothrow) float[], delete[] on
//   every exit (including the early returns below).
// - pExrBuf: allocated INTERNALLY by SaveEXRToMemory (malloc, via
//   SaveEXRImageToMemory -> SaveEXRNPartImageToMemory; the
//   SaveEXRImageToFile implementation in tinyexr.h frees its own
//   buffer with free(), proving the allocator). Owned here after a
//   successful call, freed with free() on every exit after the call.
//   On a failed call (negative return) *outbuf is not set and
//   nothing is owned.
// - pszErr: _strdup'd by tinyexr's SetErrorMessage on failure only;
//   freed with FreeEXRErrorMessage() when non-NULL.
// - fptr: owned here from OpenTempFileForWriting until fclose; a fclose failure is
//   a save failure (a buffered write can still fail on flush, same
//   rule as CompressAndSave). The catch(...) mirrors SaveJXL/SaveQOI:
//   tinyexr allocates std::vectors internally, which can throw
//   bad_alloc on huge images; the outer CSaveImage::SaveImage also
//   has a try/catch (ST-02), but this function is self-contained.
//
// Size guards (F-06/F-07 class): the float buffer is 12 bytes/pixel
// and tinyexr additionally allocates four width*height float planes
// internally (16 bytes/pixel transient), so a save near the pixel cap
// is heavy -- on Win32 it will usually fail cleanly rather than
// succeed. IsImageBufferSizeOk caps the float buffer at
// MAX_IMAGE_BYTES (x64: 4 GiB - 1, Win32: 1.5 GiB); the
// width*height <= INT_MAX guard closes the `width * height` int
// overflow inside tinyexr's split/resize (tinyexr.h computes it in
// int before casting to size_t). A guard failure returns false with
// the target untouched, like any other save failure.
bool SaveEXRFile(LPCTSTR sFileName, void* pData24bppBGR, int nWidth, int nHeight) {
	if (sFileName == NULL || sFileName[0] == 0 || pData24bppBGR == NULL || nWidth <= 0 || nHeight <= 0) {
		return false;
	}
	// tinyexr computes `width * height` in int (split loop / resize);
	// refuse dimensions whose product overflows int before it can.
	if ((__int64)nWidth * (__int64)nHeight > (__int64)INT_MAX) {
		return false;
	}
	// Float buffer size guard: stride is computed in size_t first, so
	// the multiplication itself cannot wrap (R-05 class).
	size_t nFloatStride = (size_t)nWidth * 3 * sizeof(float);
	size_t nFloatBytes = 0;
	if (!IsImageBufferSizeOk(nFloatStride, (size_t)nHeight, &nFloatBytes)) {
		return false;
	}

	// Convert the 24-bit BGR DIB (row 0 = top, rows padded to a
	// 4-byte boundary) to interleaved RGB float, row 0 first.
	float* pFloatRGB = new(std::nothrow) float[(size_t)nWidth * (size_t)nHeight * 3];
	if (pFloatRGB == NULL) {
		return false;
	}
	int nSrcStride = Helpers::DoPadding(nWidth * 3, 4);
	const unsigned char* pSrc = (const unsigned char*)pData24bppBGR;
	for (int y = 0; y < nHeight; y++) {
		const unsigned char* pSrcRow = pSrc + (size_t)y * (size_t)nSrcStride;
		float* pDstRow = pFloatRGB + (size_t)y * (size_t)nWidth * 3;
		for (int x = 0; x < nWidth; x++) {
			// DIB is BGR (8-bit sRGB, display-referred); EXR wants RGB
			// linear light. Linearise with the shared Helpers helper
			// (gamma 2.2), the same conversion SaveHDR applies -- the
			// exact inverse of the decode side's pow(v, 1/2.2) * 255
			// (EXRWrapper::ReadImage), so save -> reload round-trips.
			pDstRow[x * 3 + 0] = Helpers::LinearFromSRGB(pSrcRow[x * 3 + 2]); // R
			pDstRow[x * 3 + 1] = Helpers::LinearFromSRGB(pSrcRow[x * 3 + 1]); // G
			pDstRow[x * 3 + 2] = Helpers::LinearFromSRGB(pSrcRow[x * 3 + 0]); // B
		}
	}

	// Atomic temp file (P3-1): the HANDLE overload hands out the
	// CREATE_NEW handle and OpenTempFileForWriting wraps it in a FILE*,
	// so there is no create-close-reopen window here.
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		delete[] pFloatRGB;
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		delete[] pFloatRGB;
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	unsigned char* pExrBuf = NULL;
	try {
		// components = 3 (RGB), save_as_fp16 = 1 (see the note above).
		// Returns the EXR byte count (> 0) on success, negative on
		// error; on error *outbuf is not set and err may be set.
		const char* pszErr = NULL;
		int nExrSize = SaveEXRToMemory(pFloatRGB, nWidth, nHeight, 3, 1, &pExrBuf, &pszErr);
		if (pszErr != NULL) {
			// Error text is _strdup'd by tinyexr; free it either way.
			// (No logging facility is used here; the save just fails.)
			FreeEXRErrorMessage(pszErr);
			pszErr = NULL;
		}
		// A zero/negative size, or a NULL buffer, is a failed encode:
		// writing 0 bytes would leave an empty file behind and report
		// success (the SaveWebP/SaveQOI lesson).
		bool bWriteOk = pExrBuf != NULL && nExrSize > 0 && fwrite(pExrBuf, 1, (size_t)nExrSize, fptr) == (size_t)nExrSize;
		bool bCloseOk = (fclose(fptr) == 0);
		fptr = NULL;
		bSuccess = bWriteOk && bCloseOk;
		if (pExrBuf != NULL) {
			free(pExrBuf);
			pExrBuf = NULL;
		}
	} catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
		if (pExrBuf != NULL) {
			free(pExrBuf);
			pExrBuf = NULL;
		}
	}
	delete[] pFloatRGB;
	pFloatRGB = NULL;

	// Delete the partial temp file on any failure; the target is
	// untouched. On success swap the temp file into place; on a swap
	// failure the target stays untouched and the temp file is removed.
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
