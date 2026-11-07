#include "StdAfx.h"
#include "SaveImage.h"
#include "JPEGImage.h"
#include "BasicProcessing.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "ParameterDB.h"
#include "EXIFReader.h"
#include "TJPEGWrapper.h"
#include "WEBPWrapper.h"
#include "JXLWrapper.h"
#include "QOIWrapper.h"
#include "SaveJP2.h"
#include "SaveEXR.h"
#include <math.h> // pow (SaveHDR: sRGB -> linear light)

// stb_image_write (batch 5, save only): HDR + TGA. The implementation
// lives in this translation unit only, mirroring PPMWrapper.cpp's
// STB_IMAGE_IMPLEMENTATION for stb_image.h. PNM/PBM are hand-written
// below: stb_image_write v1.16 has no PNM/PBM writer (it writes
// PNG/BMP/TGA/HDR/JPEG only). Vendored file: stb/stb_image_write.h
// (v1.16, see the provenance note at its top).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#include "MaxImageDef.h"
#include <gdiplus.h>
#include <io.h> // _open_osfhandle, _fdopen (P3-1)
#include <fcntl.h> // _O_WRONLY (P3-1)

//////////////////////////////////////////////////////////////////////////////////////////////
// Helpers
//////////////////////////////////////////////////////////////////////////////////////////////

// Removes an existing comment segment (if any) and returns the new JPEG stream, NULL if no comment was found in stream
static uint8* RemoveExistingCommentSegment(void* pJPEGStream, int& nStreamLength) {
	uint8* pCommentSeg = (uint8*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE);
	if (pCommentSeg == NULL) {
		return NULL;
	}
	// FindJPEGMarker only guarantees that the two marker bytes are inside the
	// stream. Everything below reads at fixed offsets or trusts the length
	// field, so both have to be validated here: a stream ending right after
	// "FF FE" read two bytes past the end, and a length field larger than the
	// stream made both memcpy sizes wrong (one of them negative).
	const int nHeader = (int)(pCommentSeg - (uint8*)pJPEGStream);
	if (nHeader + 4 > nStreamLength) {
		return NULL;
	}
	const int nLenSegment = pCommentSeg[2] * 256 + pCommentSeg[3];
	// R-18: the length field counts itself (2 bytes) but not the marker, so
	// the whole segment is 2 (marker FF FE) + nLenSegment bytes. Deleting
	// only nLenSegment used to leave the last 2 bytes of the comment behind
	// and shift the following segment out of alignment, corrupting the file
	// on save. All four places below must use the full segment length.
	if (nLenSegment < 2 || nHeader + 2 + nLenSegment > nStreamLength) {
		return NULL;
	}
	uint8* pNewBuffer = new uint8[nStreamLength - (2 + nLenSegment)];
	memcpy(pNewBuffer, pJPEGStream, nHeader);
	memcpy(&(pNewBuffer[nHeader]), pCommentSeg + 2 + nLenSegment, nStreamLength - nHeader - (2 + nLenSegment));
	nStreamLength = nStreamLength - (2 + nLenSegment);
	return pNewBuffer;
}

// Finds first JPEG marker, returns NULL if none found (invalid JPEG stream?)
static uint8* FindFirstJPEGMarker(void* pJPEGStream, int nStreamLength) {
	uint8* pStream = (uint8*)pJPEGStream;
	if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
		return NULL; // not a JPEG
	}
	int nIndex = 2;
	do {
		if (pStream[nIndex] == 0xFF) {
			// block header found, skip padding bytes
			// ST-03: bound-check first: the old order read pStream[nIndex]
			// one past the end when the padding ran to the stream tail.
			while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
			break;
		}
		else {
			break; // block with pixel data found, start hashing from here
		}
	} while (nIndex < nStreamLength);

	// ST-03: nIndex may have reached nStreamLength (padding to the tail);
	// do not read past the end to test for a marker.
	if (nIndex < nStreamLength && pStream[nIndex] != 0) {
		return &(pStream[nIndex - 1]); // place on marker start
	}
	else {
		return NULL;
	}
}

// Inserts the specified string as a JPEG comment into the JPEG stream. Returns NULL in case of errors, a new JPEG stream otherwise
// The string is written as UTF-8
static uint8* InsertCommentBlock(void* pJPEGStream, int& nStreamLength, LPCTSTR comment) {
	const int MAX_COMMENT_LEN = 4096;
	uint8* pFirstJPGMarker = FindFirstJPEGMarker(pJPEGStream, nStreamLength);
	if (pFirstJPGMarker == NULL) {
		return NULL;
	}

	int numberOfCharsToConvert = min(MAX_COMMENT_LEN, (int)_tcslen(comment));
	int utf8Length = ::WideCharToMultiByte(CP_UTF8, 0, comment, numberOfCharsToConvert, NULL, 0, NULL, NULL);
	if (utf8Length == 0) {
		return NULL;
	}

	char* pBuffer = new char[utf8Length];
	::WideCharToMultiByte(CP_UTF8, 0, comment, numberOfCharsToConvert, pBuffer, utf8Length, 0, 0);

	uint8* pNewStream = new uint8[nStreamLength + utf8Length + 4]; // +4: two bytes marker, two bytes segment length
	
	if (pFirstJPGMarker[1] == 0xE0) {
		int nApp0Len = pFirstJPGMarker[2] * 256 + pFirstJPGMarker[3];
		pFirstJPGMarker += 2;
		pFirstJPGMarker += nApp0Len;
	}

	int nSizeToMarker = (int)(pFirstJPGMarker - (uint8*)pJPEGStream);
	memcpy(pNewStream, pJPEGStream, nSizeToMarker);

	// Comment segment marker
	pNewStream[nSizeToMarker] = 0xFF;
	pNewStream[nSizeToMarker + 1] = 0xFE;

	// Segment length
	int nCommentSegLen = 2 + utf8Length;
	pNewStream[nSizeToMarker + 2] = nCommentSegLen >> 8;
	pNewStream[nSizeToMarker + 3] = nCommentSegLen & 0xFF;

	// Comment and the rest of the JPEG stream
	memcpy(&(pNewStream[nSizeToMarker + 4]), pBuffer, utf8Length);
	memcpy(&(pNewStream[nSizeToMarker + 2 + nCommentSegLen]), &(((uint8*)pJPEGStream)[nSizeToMarker]), nStreamLength - nSizeToMarker);

	nStreamLength += nCommentSegLen + 2;

	delete[] pBuffer;

	return pNewStream;
}

// Gets the thumbnail as a 24 bpp BGR DIB, returns size of thumbnail in sizeThumb
static void* GetThumbnailDIB(CJPEGImage * pImage, CSize& sizeThumb) {
	EProcessingFlags eFlags = pImage->GetLastProcessFlags();
	eFlags = (EProcessingFlags)(eFlags | PFLAG_HighQualityResampling);
	eFlags = (EProcessingFlags)(eFlags & ~PFLAG_AutoContrastSection);
	eFlags = (EProcessingFlags)(eFlags & ~PFLAG_KeepParams);
	CImageProcessingParams params = pImage->GetLastProcessParams();
	params.Sharpen = 0.0;
	double dZoom;
	sizeThumb = Helpers::GetImageRect(pImage->OrigWidth(), pImage->OrigHeight(), 160, 160, Helpers::ZM_FitToScreenNoZoom, dZoom);
	void* pDIB32bpp = pImage->GetThumbnailDIB(sizeThumb, params, eFlags);
	if (pDIB32bpp == NULL) {
		return NULL;
	}
	// R-30 (review): same size_t + gate idiom as Clipboard::DoCopy and
	// CSaveImage::SaveImage below; the thumbnail is capped at 160x160 so
	// the gate cannot trip in practice, but the uint32 product must not wrap.
	size_t nSizeLinePadded = (size_t)Helpers::DoPadding(sizeThumb.cx*3, 4);
	size_t nSizeBytes = 0;
	if (!IsImageBufferSizeOk(nSizeLinePadded, (size_t)sizeThumb.cy, &nSizeBytes)) {
		return NULL;
	}
	char* pDIB24bpp = new char[nSizeBytes];
	CBasicProcessing::Convert32bppTo24bppDIB(sizeThumb.cx, sizeThumb.cy, pDIB24bpp, pDIB32bpp, false);
	return pDIB24bpp;
}

static int GetJFIFBlockLength(unsigned char* pJPEGStream) {
	int nJFIFLength = 0;
	if (pJPEGStream[2] == 0xFF && pJPEGStream[3] == 0xE0) {
		nJFIFLength = pJPEGStream[4]*256 + pJPEGStream[5] + 2;
	}
	return nJFIFLength;
}

// F-B01: atomic save helpers. No save function below ever opens the
// target itself for writing. It writes a temp file in the SAME directory
// as the target (same volume, so ReplaceFile works), and only on full
// success swaps the temp file into place. On any failure the temp file
// is deleted and the target is left byte-identical and untouched.
// The lossless JPEG transform (_WriteFile in JPEGLosslessTransform.cpp)
// uses these same helpers, so both save paths share the unique temp
// name (process id + counter) created with CREATE_NEW: an existing file
// can never be truncated by a save.
bool CreateAtomicTempFileName(LPCTSTR sTarget, CString& sTempOut, HANDLE& hFileOut) {
	hFileOut = INVALID_HANDLE_VALUE;
	// P3-2: resolve the target to an absolute path first. The MAX_PATH
	// check below then measures the absolute temp path, which is what
	// CreateFile actually opens; a relative target in a deep working
	// directory used to pass the check and fail later at CreateFile.
	TCHAR szAbsolute[MAX_PATH];
	DWORD nAbsoluteLen = ::GetFullPathName(sTarget, MAX_PATH, szAbsolute, NULL);
	if (nAbsoluteLen == 0 || nAbsoluteLen >= MAX_PATH) {
		::OutputDebugString(CString(_T("JPEGView: atomic save target path is empty or exceeds MAX_PATH, save aborted: ")) + CString(sTarget) + _T("\n"));
		return false;
	}
	CString sTargetStr(szAbsolute);
	int nSlashBack = sTargetStr.ReverseFind(_T('\\'));
	int nSlashFwd = sTargetStr.ReverseFind(_T('/'));
	int nSlash = max(nSlashBack, nSlashFwd);
	CString sDir = (nSlash >= 0) ? sTargetStr.Left(nSlash + 1) : CString(_T(""));
	DWORD dwPid = ::GetCurrentProcessId();
	for (int i = 0; i < 100; i++) {
		CString sCandidate;
		// Short prefix: the temp name must stay well below MAX_PATH even
		// when the target directory is already long. The process id plus
		// the counter keep the name unique within a process; a collision
		// only advances the counter, so the loop still terminates.
		sCandidate.Format(_T("%s~jv%lu_%d.tmp"), (LPCTSTR)sDir, (unsigned long)dwPid, i);
		if (sCandidate.GetLength() >= MAX_PATH) {
			// The temp path itself would exceed MAX_PATH: creating it
			// cannot succeed, so fail fast with a clear log instead of
			// retrying a name that can only get longer.
			::OutputDebugString(CString(_T("JPEGView: atomic save temp path exceeds MAX_PATH, save aborted: ")) + sCandidate + _T("\n"));
			return false;
		}
		HANDLE hFile = ::CreateFile(sCandidate, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			// P3-1: hand the handle out instead of closing it here. The
			// caller writes to this handle directly, so nothing can swap
			// another file into the path between creation and open (the
			// old create-close-reopen sequence had that TOCTOU window).
			// The caller owns the handle from here on (fclose closes it
			// once wrapped in a FILE*, see OpenTempFileForWriting).
			hFileOut = hFile;
			sTempOut = sCandidate;
			return true;
		}
		if (::GetLastError() != ERROR_FILE_EXISTS) {
			return false;
		}
		// Name collided (counter differs next round, so this terminates).
	}
	return false;
}
bool CreateAtomicTempFileName(LPCTSTR sTarget, CString& sTempOut) {
	// Kept for the callers that need the path, not the handle:
	// SaveGDIPlus below and SaveJP2.cpp: both save through filename-only
	// APIs (GDI+ Bitmap::Save, OpenJPEG's default file stream), so they
	// cannot use the overload above; see their own comments for the
	// remaining TOCTOU window. The reserved name is opened immediately
	// after creation, so the window is minimal.
	HANDLE hFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sTarget, sTempOut, hFile)) {
		return false;
	}
	::CloseHandle(hFile);
	return true;
}
void DeleteAtomicTempFile(LPCTSTR sTemp) {
	if (sTemp != NULL && sTemp[0] != 0) {
		::DeleteFile(sTemp);
	}
}
bool CommitAtomicTempFile(LPCTSTR sTarget, LPCTSTR sTemp) {
	// ReplaceFile requires an existing target and leaves it untouched on
	// failure; for a brand-new target it would fail, so move instead.
	// On success the temp file no longer exists as a separate file (it
	// becomes the target), so the caller must NOT delete it afterwards.
	// On failure the caller deletes the temp file and reports failure.
	if (::GetFileAttributes(sTarget) != INVALID_FILE_ATTRIBUTES) {
		return ::ReplaceFile(sTarget, sTemp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL) != 0;
	}
	return ::MoveFile(sTemp, sTarget) != 0;
}

// P3-1: wrap the CREATE_NEW handle from CreateAtomicTempFileName in a
// FILE* so the existing fwrite/fclose call sites are unchanged. fclose
// closes the underlying handle. On failure the handle is closed here and
// the caller deletes the (empty) temp file.
// Exported via SaveImage.h for SaveEXR.cpp (P3-1).
FILE* OpenTempFileForWriting(HANDLE hFile) {
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	int nFd = _open_osfhandle((intptr_t)hFile, _O_WRONLY);
	if (nFd == -1) {
		::CloseHandle(hFile);
		return NULL;
	}
	FILE* fptr = _fdopen(nFd, "wb");
	if (fptr == NULL) {
		_close(nFd);
		return NULL;
	}
	return fptr;
}

// Batch 5: stb_image_write callback context. stb's *_to_func writers
// report only their own success; a short fwrite inside the callback
// would otherwise be invisible, so the callback records it and the
// caller ANDs it into the save result. (Same rule as SaveWebP: a
// fclose failure is a save failure too.)
struct StbiWriteFileContext {
	FILE* fptr;
	bool bOk;
	StbiWriteFileContext(FILE* f) : fptr(f), bOk(true) {}
};
static void StbiWriteToFile(void* context, void* data, int size) {
	StbiWriteFileContext* pCtx = (StbiWriteFileContext*)context;
	if (size > 0 && fwrite(data, 1, (size_t)size, pCtx->fptr) != (size_t)size) {
		pCtx->bOk = false;
	}
}

// Returns true when the file name (basename only, same dot rule as
// Helpers::GetImageFormat) ends in the given extension, case-insensitive.
// Used for .pgm (P5 vs P6 in SavePNM).
static bool HasFileExtension(LPCTSTR sFileName, LPCTSTR sExtension) {
	LPCTSTR sBase = _tcsrchr(sFileName, _T('\\'));
	LPCTSTR sBaseAlt = _tcsrchr(sFileName, _T('/'));
	if (sBaseAlt != NULL && (sBase == NULL || sBaseAlt > sBase)) sBase = sBaseAlt;
	sBase = (sBase == NULL) ? sFileName : (sBase + 1);
	LPCTSTR sEnding = _tcsrchr(sBase, _T('.'));
	if (sEnding == NULL) {
		return false;
	}
	return _tcsicmp(sEnding + 1, sExtension) == 0;
}

// Rec.601 luma, integer, rounded. One rule for both the PGM grey value
// and the PBM black/white decision, so a .pgm and a .pbm of one image agree.
static unsigned char LuminanceFromBGR(unsigned char b, unsigned char g, unsigned char r) {
	return (unsigned char)((299 * r + 587 * g + 114 * b + 500) / 1000);
}

// sRGB (8-bit, display-referred) -> linear light, for HDR storage.
// This is the exact inverse of the gamma stb_image applies when
// loading HDR as 8-bit (stbi__hdr_to_ldr: pow(v, 1/2.2) * 255, the
// defaults), so a file saved here reloads in JPEGView with the same
// pixels; it is also the correct value for a Radiance file, which
// stores scene-referred linear light.
static float LinearFromSRGB(unsigned char v) {
	return (float)pow((double)v / 255.0, 2.2);
}

// F-H09: maps the JPEGSaveSubsampling INI value (444/422/420) to
// TurboJPEG. Anything else falls back to 420, the historical behaviour.
static TJSAMP JPEGSaveSubsamplingFromSettings() {
	int nSubsampling = CSettingsProvider::This().JPEGSaveSubsampling();
	if (nSubsampling == 444) return TJSAMP_444;
	if (nSubsampling == 422) return TJSAMP_422;
	return TJSAMP_420;
}

// CMYK ICC (F-H10 follow-up): the saved pixels are always RGB (a 24-bit
// BGR DIB), so a preserved ICC profile whose colour space is not RGB is
// wrong on the output file: lcms2 rejects the mismatched profile when
// the file is reloaded (JPEGView falls back silently), other viewers
// differ, and a CMYK profile is ~557 KB of dead weight. When the
// profile's colour space is not RGB, drop every ICC_PROFILE APP2 chunk;
// an RGB profile is kept verbatim, and a grey source is no exception
// (its output is RGB too). The signature is read straight from the ICC
// header (profile bytes 16..19), so no lcms2 is pulled into the save
// path. A profile may span several APP2 chunks; only chunk 1 carries
// the header, so the decision reads chunk 1 and the strip drops all
// ICC chunks. Anything unrecognisable (no chunk 1, truncated chunk 1,
// malformed segment chain, allocation failure) keeps the segments
// untouched: failing safe preserves metadata rather than dropping it
// on a guess. Returns true when the caller must use the filtered blob:
// *ppFilteredSegments is the new buffer, owned by the caller (NULL
// when every segment was ICC and nothing is left); false keeps the
// original blob unchanged.
static bool FilterNonRGBICCProfileSegments(const unsigned char* pSegments, int nSegmentsLen, int nSegmentsBeforeEXIF,
	unsigned char** ppFilteredSegments, int& nFilteredLen, int& nFilteredBeforeEXIF) {
	*ppFilteredSegments = NULL;
	nFilteredLen = nSegmentsLen;
	nFilteredBeforeEXIF = nSegmentsBeforeEXIF;
	if (pSegments == NULL || nSegmentsLen < 18) {
		return false;
	}
	// Pass 1: find chunk 1 and read its colour-space signature.
	// Segment layout: FF E2, length (counts itself, not the marker),
	// "ICC_PROFILE\0" (12 bytes), chunk number, chunk count, profile
	// data. The signature is profile bytes 16..19, i.e. segment
	// offsets 34..37, so chunk 1 needs at least 20 profile bytes
	// (block length >= 36).
	int nIndex = 0;
	bool bIsNonRGB = false;
	bool bFoundChunk1 = false;
	while (nIndex + 4 <= nSegmentsLen) {
		if (pSegments[nIndex] != 0xFF) {
			return false;
		}
		int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
		if (nBlockLength < 2) {
			return false;
		}
		int nSegLen = 2 + nBlockLength;
		if (nIndex + nSegLen > nSegmentsLen) {
			return false;
		}
		if (pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
			memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0 &&
			pSegments[nIndex + 16] == 1) {
			if (nBlockLength < 36) {
				return false; // chunk 1 too short for the signature
			}
			bFoundChunk1 = true;
			bIsNonRGB = memcmp(pSegments + nIndex + 34, "RGB ", 4) != 0;
			break;
		}
		nIndex += nSegLen;
	}
	if (!bFoundChunk1 || !bIsNonRGB) {
		return false; // no ICC profile, RGB profile, or no chunk 1: keep
	}
	// Pass 2: validate the whole chain and count the ICC bytes. The
	// chain was checked only up to chunk 1 above, so a malformed
	// segment after chunk 1 must still keep the original.
	int nICCBytes = 0;
	nIndex = 0;
	while (nIndex < nSegmentsLen) {
		if (nIndex + 4 > nSegmentsLen || pSegments[nIndex] != 0xFF) {
			return false;
		}
		int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
		if (nBlockLength < 2) {
			return false;
		}
		int nSegLen = 2 + nBlockLength;
		if (nIndex + nSegLen > nSegmentsLen) {
			return false;
		}
		if (pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
			memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0) {
			nICCBytes += nSegLen;
		}
		nIndex += nSegLen;
	}
	// nSegmentsLen is capped at 16 MB by the F-H10 walker and
	// nICCBytes <= nSegmentsLen, so the subtraction cannot underflow.
	nFilteredLen = nSegmentsLen - nICCBytes;
	if (nFilteredLen == 0) {
		nFilteredBeforeEXIF = 0;
		return true; // *ppFilteredSegments stays NULL: nothing left
	}
	unsigned char* pFiltered = new(std::nothrow) unsigned char[nFilteredLen];
	if (pFiltered == NULL) {
		nFilteredLen = nSegmentsLen;
		nFilteredBeforeEXIF = nSegmentsBeforeEXIF;
		return false; // allocation failed: keep the original
	}
	// Pass 3: copy every non-ICC segment. The chain was fully
	// validated in pass 2, so the lengths below are known good.
	// nSegmentsBeforeEXIF is a byte offset between segments (the
	// walker sets it at a kept-size boundary), so a kept segment lies
	// wholly before the split iff it started before it.
	nIndex = 0;
	int nDst = 0;
	nFilteredBeforeEXIF = 0;
	while (nIndex < nSegmentsLen) {
		int nBlockLength = pSegments[nIndex + 2] * 256 + pSegments[nIndex + 3];
		int nSegLen = 2 + nBlockLength;
		bool bIsICC = pSegments[nIndex + 1] == 0xE2 && nBlockLength >= 16 &&
			memcmp(pSegments + nIndex + 4, "ICC_PROFILE\0", 12) == 0;
		if (!bIsICC) {
			memcpy(pFiltered + nDst, pSegments + nIndex, nSegLen);
			if (nIndex < nSegmentsBeforeEXIF) {
				nFilteredBeforeEXIF += nSegLen;
			}
			nDst += nSegLen;
		}
		nIndex += nSegLen;
	}
	*ppFilteredSegments = pFiltered;
	return true;
}

// Returns the compressed JPEG stream that must be freed by the caller. NULL in case of error.
static void* CompressAndSave(LPCTSTR sFileName, CJPEGImage * pImage, 
							 void* pData, int nWidth, int nHeight, int nQuality, int& nJPEGStreamLen, 
							 bool& tjFreeNeeded, bool bCopyEXIF, bool bDeleteThumbnail) {
	nJPEGStreamLen = 0;
	tjFreeNeeded = true;
	bool bOutOfMemory;
	unsigned char* pTargetStream = (unsigned char*) TurboJpeg::Compress(pData, nWidth, nHeight, 
		nJPEGStreamLen, bOutOfMemory, nQuality, JPEGSaveSubsamplingFromSettings());
	if (pTargetStream == NULL) {
		return NULL;
	}

	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		TurboJpeg::Free(pTargetStream);
		return NULL;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		TurboJpeg::Free(pTargetStream);
		return NULL;
	}
	// R-11: the EXIF/comment helpers below allocate with throwing new
	// (and GetThumbnailDIB can throw), all after the temp file was
	// created. Without the catch below, a bad_alloc left the TurboJPEG
	// stream allocated, the temp FILE* open and the temp file orphaned;
	// the outer CSaveImage::SaveImage catch only frees the 24bpp buffer.
	// pEXIFStream is hoisted so the catch can free the EXIF buffer when
	// the throw happens after it was allocated but before it was handed
	// to pTargetStream (it is NULled on transfer, so no double free).
	unsigned char* pEXIFStream = NULL;
	// pStrippedSegments is hoisted for the same reason: the filter below
	// allocates it inside the try, and the catch must free it when the
	// throw happens before the segments are copied into the stream.
	unsigned char* pStrippedSegments = NULL;
	try {

	// F-H10: preserved APP/COM segments from the source JPEG (JFIF APP0,
	// XMP, ICC, IPTC, COM) in source order, excluding the EXIF APP1.
	// NULL when the source had none (or was not a JPEG).
	unsigned char* pSegments = (unsigned char*)pImage->GetJPEGSegments();
	int nSegmentsLen = pImage->GetJPEGSegmentsLength();
	int nSegmentsBeforeEXIF = pImage->GetJPEGSegmentsBeforeEXIF();

	// CMYK ICC (F-H10 follow-up): the saved pixels are always RGB, so
	// a non-RGB profile must not survive on the output file. When the
	// filter strips, pSegments points at the filtered copy below (or
	// NULL when every segment was ICC); otherwise it stays the
	// image-owned blob. The copy is freed once the segments have been
	// copied into the output stream; see also the R-11 catch.
	int nStrippedLen = 0;
	int nStrippedBeforeEXIF = 0;
	if (FilterNonRGBICCProfileSegments(pSegments, nSegmentsLen, nSegmentsBeforeEXIF,
		&pStrippedSegments, nStrippedLen, nStrippedBeforeEXIF)) {
		pSegments = pStrippedSegments;
		nSegmentsLen = nStrippedLen;
		nSegmentsBeforeEXIF = nStrippedBeforeEXIF;
	}

	// If EXIF data is present, replace any JFIF block by this EXIF block to preserve the EXIF information
	if (pImage->GetEXIFData() != NULL && bCopyEXIF) {
		const int cnAdditionalThumbBytes = 32000;
		int nEXIFBlockLenCorrection = 0;
		pEXIFStream = new unsigned char[nJPEGStreamLen + pImage->GetEXIFDataLength() + nSegmentsLen + cnAdditionalThumbBytes];
		memcpy(pEXIFStream, pTargetStream, 2); // copy SOI block
		// F-H10: segments that stood before the EXIF block in the source
		// (usually the JFIF APP0), so the original segment order survives.
		if (nSegmentsBeforeEXIF > 0) {
			memcpy(pEXIFStream + 2, pSegments, nSegmentsBeforeEXIF);
		}
		memcpy(pEXIFStream + 2 + nSegmentsBeforeEXIF, pImage->GetEXIFData(), pImage->GetEXIFDataLength()); // copy EXIF block
		
		// Set image orientation back to normal orientation, we save the pixels as displayed
		CEXIFReader exifReader(pEXIFStream + 2 + nSegmentsBeforeEXIF, IF_JPEG);
		exifReader.WriteImageOrientation(1); // 1 means default orientation (unrotated)
		if (bDeleteThumbnail) {
			exifReader.DeleteThumbnail();
		} else if (exifReader.HasJPEGCompressedThumbnail()) {
			// recreate EXIF thumbnail image
			CSize sizeThumb;
			void* pDIBThumb = GetThumbnailDIB(pImage, sizeThumb);
			if (pDIBThumb != NULL) {
				int nJPEGThumbStreamLen;
				unsigned char* pJPEGThumb = (unsigned char*) TurboJpeg::Compress(pDIBThumb, sizeThumb.cx, sizeThumb.cy, nJPEGThumbStreamLen, bOutOfMemory, 70);
				if (pJPEGThumb != NULL) {
					int nThumbJFIFLen = GetJFIFBlockLength(pJPEGThumb);
					nEXIFBlockLenCorrection = nJPEGThumbStreamLen - nThumbJFIFLen - exifReader.GetJPEGThumbStreamLen();
					if (nEXIFBlockLenCorrection <= cnAdditionalThumbBytes && pImage->GetEXIFDataLength() + nEXIFBlockLenCorrection < 65536) {
						exifReader.UpdateJPEGThumbnail(pJPEGThumb + 2 + nThumbJFIFLen, nJPEGThumbStreamLen - 2 - nThumbJFIFLen, nEXIFBlockLenCorrection, sizeThumb);
					} else {
						nEXIFBlockLenCorrection = 0;
					}
					TurboJpeg::Free(pJPEGThumb);
				}
				delete[] pDIBThumb;
			}
		}

		int nJFIFLength = GetJFIFBlockLength(pTargetStream);
		// F-H10: segments that stood after the EXIF block, then the rest
		// of the TurboJPEG stream (without its own JFIF APP0).
		int nSegmentsAfterEXIF = nSegmentsLen - nSegmentsBeforeEXIF;
		unsigned char* pTail = pEXIFStream + 2 + nSegmentsBeforeEXIF + pImage->GetEXIFDataLength() + nEXIFBlockLenCorrection;
		if (nSegmentsAfterEXIF > 0) {
			memcpy(pTail, pSegments + nSegmentsBeforeEXIF, nSegmentsAfterEXIF);
		}
		memcpy(pTail + nSegmentsAfterEXIF, pTargetStream + 2 + nJFIFLength, nJPEGStreamLen - 2 - nJFIFLength);
		TurboJpeg::Free(pTargetStream);
		pTargetStream = pEXIFStream;
		pEXIFStream = NULL; // ownership moved to pTargetStream
		tjFreeNeeded = false;
		nJPEGStreamLen = nJPEGStreamLen - nJFIFLength + pImage->GetEXIFDataLength() + nEXIFBlockLenCorrection + nSegmentsLen;
	} else if (pSegments != NULL && nSegmentsLen > 0 && bCopyEXIF) {
		// F-H10: no EXIF to preserve, but the source had other segments:
		// keep them (SOI + segments + TurboJPEG stream without its own
		// JFIF APP0, so a preserved APP0 is the only one).
		int nJFIFLength = GetJFIFBlockLength(pTargetStream);
		pEXIFStream = new unsigned char[nJPEGStreamLen + nSegmentsLen];
		memcpy(pEXIFStream, pTargetStream, 2); // copy SOI block
		memcpy(pEXIFStream + 2, pSegments, nSegmentsLen);
		memcpy(pEXIFStream + 2 + nSegmentsLen, pTargetStream + 2 + nJFIFLength, nJPEGStreamLen - 2 - nJFIFLength);
		TurboJpeg::Free(pTargetStream);
		pTargetStream = pEXIFStream;
		pEXIFStream = NULL; // ownership moved to pTargetStream
		tjFreeNeeded = false;
		nJPEGStreamLen = nJPEGStreamLen - nJFIFLength + nSegmentsLen;
	}

	// The segments were copied into pTargetStream above; the filtered
	// copy is no longer needed (the unfiltered blob is owned by the
	// image and never freed here).
	delete[] pStrippedSegments;
	pStrippedSegments = NULL;

	// Take over existing JPEG comment from the old image
	LPCTSTR sComment = pImage->GetJPEGComment();
	if (sComment != NULL && sComment[0] != 0) {
		uint8* pNewStream = RemoveExistingCommentSegment(pTargetStream, nJPEGStreamLen);
		if (pNewStream != NULL) {
			if (tjFreeNeeded) TurboJpeg::Free(pTargetStream); else delete[] pTargetStream;
			pTargetStream = pNewStream;
			tjFreeNeeded = false;
		}
		pNewStream = InsertCommentBlock(pTargetStream, nJPEGStreamLen, sComment);
		if (pNewStream != NULL) {
			if (tjFreeNeeded) TurboJpeg::Free(pTargetStream); else delete[] pTargetStream;
			pTargetStream = pNewStream;
			tjFreeNeeded = false;
		}
	}

	bool bWriteOk = fwrite(pTargetStream, 1, nJPEGStreamLen, fptr) == nJPEGStreamLen;
	// A buffered write can still fail on close (flush to a full disk), so
	// a fclose failure is a save failure too, not a success. fclose must
	// run even when fwrite failed, otherwise the temp file stays open
	// and its deletion below fails.
	bool bCloseOk = (fclose(fptr) == 0);
	fptr = NULL; // closed: the R-11 catch below must not close it again
	bool bSuccess = bWriteOk && bCloseOk;

	// delete partial temp file if no success; the target is untouched
	if (!bSuccess) {
		if (tjFreeNeeded) TurboJpeg::Free(pTargetStream); else delete[] pTargetStream;
		DeleteAtomicTempFile(sTempFile);
		return NULL;
	}

	// Swap the temp file into place; on a swap failure the target stays
	// untouched and the temp file is removed.
	if (!CommitAtomicTempFile(sFileName, sTempFile)) {
		if (tjFreeNeeded) TurboJpeg::Free(pTargetStream); else delete[] pTargetStream;
		DeleteAtomicTempFile(sTempFile);
		return NULL;
	}

	} catch (...) {
		// R-11: a throwing new in the EXIF/comment helpers above (or in
		// GetThumbnailDIB) used to leak the TurboJPEG stream, leave the
		// temp FILE* open and orphan the temp file; the outer
		// CSaveImage::SaveImage catch only frees the 24bpp buffer. Free
		// the stream (whichever side owns it now), close the temp file
		// and delete it, so the save fails like any other write failure
		// and the target stays untouched.
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
		if (pEXIFStream != NULL) {
			delete[] pEXIFStream;
		}
		delete[] pStrippedSegments; // NULL-safe; NULled once freed above
		if (tjFreeNeeded) TurboJpeg::Free(pTargetStream); else delete[] pTargetStream;
		DeleteAtomicTempFile(sTempFile);
		return NULL;
	}
	// Success, return compressed JPEG stream
	return pTargetStream;
}

// pData must point to 24 bit BGR DIB
static bool SaveWebP(LPCTSTR sFileName, void* pData, int nWidth, int nHeight, bool bUseLosslessWEBP) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		uint8* pOutput;
		size_t nSize;
		int nQuality = CSettingsProvider::This().WEBPSaveQuality();
		pOutput = (uint8*)WebpReaderWriter::Compress((uint8*)pData, nWidth, nHeight, nSize, nQuality, bUseLosslessWEBP);
		// WebPEncode returns 0 bytes on failure; fwrite(NULL, 1, 0) returns 0, so
		// "0 == 0" made this look like a successful 0-byte save.
		bool bWriteOk = pOutput != NULL && nSize > 0 && fwrite(pOutput, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		WebpReaderWriter::FreeMemory(pOutput);
	} catch(...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB
static bool SaveJXL(LPCTSTR sFileName, void* pData, int nWidth, int nHeight, bool bUseLosslessJXL) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		uint8* pOutput;
		size_t nSize;
		int nQuality = CSettingsProvider::This().JXLSaveQuality();
		pOutput = (uint8*)JxlReader::Compress((uint8*)pData, nWidth, nHeight, nSize, nQuality, bUseLosslessJXL);
		if (pOutput == NULL) throw 1;
		// Like WebP/QOI below: a zero-byte "success" would leave an empty
		// file behind, so nSize > 0 is part of the success test.
		bool bWriteOk = pOutput != NULL && nSize > 0 && fwrite(pOutput, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		JxlReader::Free(pOutput);
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB
static bool SaveQOI(LPCTSTR sFileName, void* pData, int nWidth, int nHeight) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		uint8* pOutput;
		int nSize;
		pOutput = (uint8*)QoiReaderWriter::Compress((uint8*)pData, nWidth, nHeight, nSize);
		// Compress leaves nSize at 0 on failure, so a failed encode was reported as
		// a successful 0-byte save.
		bool bWriteOk = pOutput != NULL && nSize > 0 && fwrite(pOutput, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		QoiReaderWriter::FreeMemory(pOutput);
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB (top-down, rows padded to 4 bytes,
// same layout SaveWebP/SaveQOI receive).
// PNM is hand-written: stb_image_write has no PNM writer. Binary P6
// (RGB, maxval 255) for .ppm/.pnm, binary P5 (grey, maxval 255) for
// .pgm; the reader side (PPMWrapper) accepts P5/P6, so both round-trip
// through JPEGView. Rows are stored top-down, tightly packed.
static bool SavePNM(LPCTSTR sFileName, void* pData, int nWidth, int nHeight) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		bool bGray = HasFileExtension(sFileName, _T("PGM"));
		int nChannels = bGray ? 1 : 3;
		int nPaddedStride = Helpers::DoPadding(nWidth * 3, 4);
		unsigned char* pSourcePixels = (unsigned char*)pData;
		// Tight buffer, like QOIWrapper::Compress: strip the 4-byte row
		// padding and swap BGR to RGB (grey: single luma channel).
		// A3-2: format the header before allocating the pixel buffer:
		// CStringA::Format can throw on OOM, and the catch below only
		// closes fptr, so formatting after new would leak the buffer.
		CStringA sHeader;
		sHeader.Format(bGray ? "P5\n%d %d\n255\n" : "P6\n%d %d\n255\n", nWidth, nHeight);
		size_t nSize = (size_t)nWidth * (size_t)nHeight * (size_t)nChannels;
		unsigned char* pPixelData = new unsigned char[nSize];
		for (int y = 0; y < nHeight; y++) {
			for (int x = 0; x < nWidth; x++) {
				size_t nSrc = (size_t)y * (size_t)nPaddedStride + (size_t)x * 3;
				size_t nDst = ((size_t)y * (size_t)nWidth + (size_t)x) * (size_t)nChannels;
				if (bGray) {
					pPixelData[nDst] = LuminanceFromBGR(pSourcePixels[nSrc], pSourcePixels[nSrc + 1], pSourcePixels[nSrc + 2]);
				} else {
					pPixelData[nDst] = pSourcePixels[nSrc + 2];
					pPixelData[nDst + 1] = pSourcePixels[nSrc + 1];
					pPixelData[nDst + 2] = pSourcePixels[nSrc];
				}
			}
		}
		bool bWriteOk = fwrite(sHeader, 1, (size_t)sHeader.GetLength(), fptr) == (size_t)sHeader.GetLength() &&
			fwrite(pPixelData, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		delete[] pPixelData;
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB (top-down, rows padded to 4 bytes,
// same layout SaveWebP/SaveQOI receive).
// Radiance RGBE via stb_image_write. The float input is linear light
// (see LinearFromSRGB); stb stores it as-is, top-down, no gamma of
// its own. stb's HDR writer reports only its own success, so the
// callback's write flag is part of the result.
static bool SaveHDR(LPCTSTR sFileName, void* pData, int nWidth, int nHeight) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		int nPaddedStride = Helpers::DoPadding(nWidth * 3, 4);
		unsigned char* pSourcePixels = (unsigned char*)pData;
		// Tight top-down float RGB buffer: stb takes no stride.
		size_t nPixels = (size_t)nWidth * (size_t)nHeight;
		float* pFloatData = new float[nPixels * 3];
		for (size_t i = 0; i < nPixels; i++) {
			size_t nSrc = (i / (size_t)nWidth) * (size_t)nPaddedStride + (i % (size_t)nWidth) * 3;
			pFloatData[i * 3] = LinearFromSRGB(pSourcePixels[nSrc + 2]);
			pFloatData[i * 3 + 1] = LinearFromSRGB(pSourcePixels[nSrc + 1]);
			pFloatData[i * 3 + 2] = LinearFromSRGB(pSourcePixels[nSrc]);
		}
		StbiWriteFileContext ctx(fptr);
		bool bStbiOk = stbi_write_hdr_to_func(StbiWriteToFile, &ctx, nWidth, nHeight, 3, pFloatData) != 0;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bStbiOk && ctx.bOk && bCloseOk;
		fptr = NULL;
		delete[] pFloatData;
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB (top-down, rows padded to 4 bytes,
// same layout SaveWebP/SaveQOI receive).
// True-colour TGA via stb_image_write (RLE on, the header's default;
// ReaderTGA accepts RLE RGB). stb takes top-down RGB and stores it
// bottom-up with a bottom-left origin (descriptor 0); ReaderTGA flips
// back when bit 5 is clear, so the round-trip is upright.
static bool SaveTGA(LPCTSTR sFileName, void* pData, int nWidth, int nHeight) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		int nChannels = 3;
		int nPaddedStride = Helpers::DoPadding(nWidth * 3, 4);
		unsigned char* pSourcePixels = (unsigned char*)pData;
		// Tight top-down RGB buffer, like QOIWrapper::Compress.
		size_t nSize = (size_t)nWidth * (size_t)nHeight * (size_t)nChannels;
		unsigned char* pPixelData = new unsigned char[nSize];
		for (size_t i = 0; i < nSize; i += (size_t)nChannels) {
			size_t j = i / ((size_t)nWidth * (size_t)nChannels) * (size_t)nPaddedStride + i % ((size_t)nWidth * (size_t)nChannels);
			pPixelData[i] = pSourcePixels[j + 2];
			pPixelData[i + 1] = pSourcePixels[j + 1];
			pPixelData[i + 2] = pSourcePixels[j];
		}
		StbiWriteFileContext ctx(fptr);
		bool bStbiOk = stbi_write_tga_to_func(StbiWriteToFile, &ctx, nWidth, nHeight, 3, pPixelData) != 0;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bStbiOk && ctx.bOk && bCloseOk;
		fptr = NULL;
		delete[] pPixelData;
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// pData must point to 24 bit BGR DIB (top-down, rows padded to 4 bytes,
// same layout SaveWebP/SaveQOI receive).
// Binary PBM (P4), hand-written: stb_image_write has no PBM writer.
// Header "P4\n<w> <h>\n", then one bit per pixel, MSB first, rows
// padded to whole bytes with 0 bits. Per the PBM spec 1 = black,
// 0 = white; a pixel is black when its Rec.601 luminance is < 128
// (same luminance rule as SavePNM's PGM grey, threshold 128).
// P4 files reopen through JPEGView's own PBM reader (PBMWrapper,
// IF_PBM); the save/reopen round-trip is pixel-exact.
static bool SavePBM(LPCTSTR sFileName, void* pData, int nWidth, int nHeight) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	bool bSuccess = false;
	try {
		int nPaddedStride = Helpers::DoPadding(nWidth * 3, 4);
		unsigned char* pSourcePixels = (unsigned char*)pData;
		int nRowBytes = (nWidth + 7) / 8;
		// A3-2: format the header before allocating the bit buffer:
		// CStringA::Format can throw on OOM, and the catch below only
		// closes fptr, so formatting after new would leak the buffer.
		CStringA sHeader;
		sHeader.Format("P4\n%d %d\n", nWidth, nHeight);
		size_t nSize = (size_t)nRowBytes * (size_t)nHeight;
		unsigned char* pBits = new unsigned char[nSize];
		memset(pBits, 0, nSize);
		for (int y = 0; y < nHeight; y++) {
			for (int x = 0; x < nWidth; x++) {
				size_t nSrc = (size_t)y * (size_t)nPaddedStride + (size_t)x * 3;
				if (LuminanceFromBGR(pSourcePixels[nSrc], pSourcePixels[nSrc + 1], pSourcePixels[nSrc + 2]) < 128) {
					pBits[(size_t)y * (size_t)nRowBytes + (size_t)(x / 8)] |= (unsigned char)(0x80 >> (x % 8));
				}
			}
		}
		bool bWriteOk = fwrite(sHeader, 1, (size_t)sHeader.GetLength(), fptr) == (size_t)sHeader.GetLength() &&
			fwrite(pBits, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		delete[] pBits;
	}
	catch (...) {
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
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

// Copied from MS sample
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
   UINT  num = 0;          // number of image encoders
   UINT  size = 0;         // size of the image encoder array in bytes

   Gdiplus::GetImageEncodersSize(&num, &size);
   if(size == 0)
	  return -1;  // Failure

   Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
   if (pImageCodecInfo == NULL)
	  return -1;  // Failure

   Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

   for(UINT j = 0; j < num; ++j) {
	  if( wcscmp(pImageCodecInfo[j].MimeType, format) == 0 ) {
		 *pClsid = pImageCodecInfo[j].Clsid;
		 free(pImageCodecInfo);
		 return j;  // Success
	  }    
   }

   free(pImageCodecInfo);
   return -1;  // Failure
}

// Saves the given 24 bpp DIB data to the file name given using GDI+
static bool SaveGDIPlus(LPCTSTR sFileName, EImageFormat eFileFormat, void* pData, int nWidth, int nHeight) {
	Gdiplus::Bitmap* pBitmap = new Gdiplus::Bitmap(nWidth, nHeight, Helpers::DoPadding(nWidth*3, 4), PixelFormat24bppRGB, (BYTE*)pData);
	if (pBitmap->GetLastStatus() != Gdiplus::Ok) {
		delete pBitmap;
		return false;
	}

	const wchar_t* sMIMEType = NULL;
	switch (eFileFormat) {
		case IF_WindowsBMP:
			sMIMEType = L"image/bmp";
			break;
		case IF_PNG:
			sMIMEType = L"image/png";
			break;
		case IF_TIFF:
			sMIMEType = L"image/tiff";
			break;
		case IF_GIF:
			// GDI+ saves GIF quantized to a 256-colour palette: that is
			// the format, not a limitation of this code path.
			sMIMEType = L"image/gif";
			break;
	}

	CLSID encoderClsid;
	int result = (sMIMEType == NULL) ? -1 : GetEncoderClsid(sMIMEType, &encoderClsid);
	if (result < 0) {
		delete pBitmap;
		return false;
	}

	// F-H11, same class as F-B01: GDI+ used to save straight to the
	// target, leaving a partial file there on failure. Save to a temp
	// file in the same directory instead, then swap it in.
	// P3-1 residual: GDI+ Bitmap::Save takes a file name, not a handle,
	// so this path cannot use the handle overload above and keeps the
	// create-close-reopen sequence. The name is CREATE_NEW-reserved and
	// opened immediately, so the window is the microseconds between the
	// close and GDI+'s open, winnable only by a same-directory actor
	// that predicts the ~jv<pid>_<n>.tmp name; closing it would need an
	// in-memory stream copy of the whole encoded image. Accepted.
	CString sTempFile;
	if (!CreateAtomicTempFileName(sFileName, sTempFile)) {
		delete pBitmap;
		return false;
	}
	const wchar_t* sTempFileUnicode;
	sTempFileUnicode = (const wchar_t*)sTempFile;

	bool bOk = pBitmap->Save(sTempFileUnicode, &encoderClsid, NULL) == Gdiplus::Ok;

	delete pBitmap;
	if (!bOk) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	if (!CommitAtomicTempFile(sFileName, sTempFile)) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	return bOk;
}

//////////////////////////////////////////////////////////////////////////////////////////////
// CSaveImage
//////////////////////////////////////////////////////////////////////////////////////////////

bool CSaveImage::SaveImage(LPCTSTR sFileName, CJPEGImage * pImage, const CImageProcessingParams& procParams,
			 EProcessingFlags eFlags, bool bFullSize, bool bUseLosslessWEBP, bool bCreateParameterDBEntry) {
	pImage->EnableDimming(false);
	// Bad allocation anywhere on the save path (throwing new in the JPEG
	// comment helpers, CompressAndSave, or the 24bpp buffer below) must
	// show "Error saving file", not terminate the process (ST-02).
	char* pDIB24bpp = NULL;
	try {

	CSize imageSize;
	void* pDIB32bpp;

	if (bFullSize) {
		imageSize = pImage->OrigSize();
		pDIB32bpp = pImage->GetDIB(imageSize, imageSize, CPoint(0, 0), procParams, eFlags);
	} else {
		imageSize = CSize(pImage->DIBWidth(), pImage->DIBHeight());
		pDIB32bpp = pImage->DIBPixelsLastProcessed(true);
	}

	if (pDIB32bpp == NULL) {
		pImage->EnableDimming(true);
		return false;
	}

	// R-30: size_t + IsImageBufferSizeOk gate (was uint32: same wrap-then-
	// heap-overflow shape as Clipboard::DoCopy). Dimensions are already
	// bounded by MAX_IMAGE_DIMENSION, so imageSize.cx*3 cannot overflow
	// int before DoPadding; the gate still checks the real product.
	size_t nSizeLinePadded = (size_t)Helpers::DoPadding(imageSize.cx*3, 4);
	size_t nSizeBytes = 0;
	if (!IsImageBufferSizeOk(nSizeLinePadded, (size_t)imageSize.cy, &nSizeBytes)) {
		pImage->EnableDimming(true);
		return false;
	}
	pDIB24bpp = new char[nSizeBytes];
	CBasicProcessing::Convert32bppTo24bppDIB(imageSize.cx, imageSize.cy, pDIB24bpp, pDIB32bpp, false);

	EImageFormat eFileFormat = Helpers::GetImageFormat(sFileName);
	bool bSuccess = false;
	__int64 nPixelHash = 0;
	if (eFileFormat == IF_JPEG || eFileFormat == IF_JPEG_Embedded) {
		// Save JPEG not over GDI+ - we want to keep the meta-data if there is meta-data
		int nJPEGStreamLen;
		bool tjFreeNeeded;
		void* pCompressedJPEG = CompressAndSave(sFileName, pImage, pDIB24bpp, imageSize.cx, imageSize.cy, 
			CSettingsProvider::This().JPEGSaveQuality(), nJPEGStreamLen, tjFreeNeeded, true, !bFullSize);
		bSuccess = pCompressedJPEG != NULL;
		if (bSuccess) {
			nPixelHash = Helpers::CalculateJPEGFileHash(pCompressedJPEG, nJPEGStreamLen);
			if (tjFreeNeeded) {
				TurboJpeg::Free((unsigned char*)pCompressedJPEG);
			} else {
				delete[] pCompressedJPEG;
			}
		}
	} else {
		if (eFileFormat == IF_WEBP) {
			bSuccess = SaveWebP(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy, bUseLosslessWEBP);
		} else if (eFileFormat == IF_QOI) {
			bSuccess = SaveQOI(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else if (eFileFormat == IF_JXL) {
			bSuccess = SaveJXL(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy, bUseLosslessWEBP);
		} else if (eFileFormat == IF_JP2) {
			bSuccess = SaveJP2File(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy, bUseLosslessWEBP);
		} else if (eFileFormat == IF_EXR) {
			bSuccess = SaveEXRFile(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else if (eFileFormat == IF_PPM) {
			bSuccess = SavePNM(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else if (eFileFormat == IF_HDR) {
			bSuccess = SaveHDR(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else if (eFileFormat == IF_TGA) {
			bSuccess = SaveTGA(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else if (eFileFormat == IF_PBM) {
			bSuccess = SavePBM(sFileName, pDIB24bpp, imageSize.cx, imageSize.cy);
		} else {
			bSuccess = SaveGDIPlus(sFileName, eFileFormat, pDIB24bpp, imageSize.cx, imageSize.cy);
		}
		if (bSuccess) {
			CJPEGImage tempImage(imageSize.cx, imageSize.cy, pDIB32bpp, NULL, 4, 0, IF_Unknown, false, 0, 1, 0);
			nPixelHash = tempImage.GetUncompressedPixelHash();
			tempImage.DetachOriginalPixels();
		}
	}

	delete[] pDIB24bpp;
	pDIB24bpp = NULL;

	// Create database entry to avoid processing image again
	if (bSuccess && bCreateParameterDBEntry && CSettingsProvider::This().CreateParamDBEntryOnSave()) {
		if (nPixelHash != 0) {
			CParameterDBEntry newEntry;
			CImageProcessingParams ippNone(0.0, 1.0, 1.0, CSettingsProvider::This().Sharpen(), 0.0, 0.5, 0.5, 0.25, 0.5, 0.0, 0.0, 0.0);
			EProcessingFlags procFlagsNone = GetProcessingFlag(eFlags, PFLAG_HighQualityResampling) ? PFLAG_HighQualityResampling : PFLAG_None;
			newEntry.InitFromProcessParams(ippNone, procFlagsNone, CRotationParams(0));
			newEntry.SetHash(nPixelHash);
			CParameterDB::This().AddEntry(newEntry);
		}
	}
	pImage->EnableDimming(true);

	return bSuccess;
	} catch (...) {
		delete[] pDIB24bpp;
		pImage->EnableDimming(true);
		return false;
	}
}

bool CSaveImage::SaveImage(LPCTSTR sFileName, CJPEGImage * pImage, bool bUseLosslessWEBP)
{
	CImageProcessingParams paramsNotUsed;
	return SaveImage(sFileName, pImage, paramsNotUsed, PFLAG_None, false, bUseLosslessWEBP, false);
}

bool CSaveImage::ConvertToJXL(LPCTSTR sJxlFile, LPCTSTR sJpegFile) {
	CString sTempFile;
	HANDLE hTempFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sJxlFile, sTempFile, hTempFile)) {
		return false;
	}
	FILE* fptr = OpenTempFileForWriting(hTempFile);
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	bool bSuccess = false;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	unsigned char* pBuffer = NULL;

	try {
		size_t nLengthBytes = 0;
		hFile = ::CreateFile(sJpegFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			throw 1;
		}

		size_t nFileSize = Helpers::GetFileSize(sJpegFile);
		if (nFileSize > MAX_JPEG_FILE_SIZE) {
			throw 1;
		}

		pBuffer = new(std::nothrow) unsigned char[nFileSize];
		if (pBuffer == NULL) {
			throw 1;
		}

		unsigned int nNumBytesRead;
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			::CloseHandle(hFile);
			hFile = INVALID_HANDLE_VALUE;
			nLengthBytes = nFileSize;
		} else {
			throw 1;
		}

		size_t nSize;
		uint8* pOutput = (uint8*)JxlReader::CompressJPEG(pBuffer, nLengthBytes, nSize);
		delete[] pBuffer;
		pBuffer = NULL;
		if (pOutput == NULL) throw 1;
		// See SaveJXL above: nSize > 0, or an empty file counts as success.
		bool bWriteOk = pOutput != NULL && nSize > 0 && fwrite(pOutput, 1, nSize, fptr) == nSize;
		bool bCloseOk = (fclose(fptr) == 0);
		bSuccess = bWriteOk && bCloseOk;
		fptr = NULL;
		JxlReader::Free(pOutput);
	}
	catch (...) {
		if (hFile != INVALID_HANDLE_VALUE) {
			::CloseHandle(hFile);
		}
		delete[] pBuffer;
		if (fptr != NULL) {
			fclose(fptr);
			fptr = NULL;
		}
	}

	// delete partial temp file if no success; the target is untouched
	if (!bSuccess) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	if (!CommitAtomicTempFile(sJxlFile, sTempFile)) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	return true;
}
