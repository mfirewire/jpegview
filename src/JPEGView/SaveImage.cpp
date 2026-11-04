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
#include "MaxImageDef.h"
#include <gdiplus.h>

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
			while (pStream[nIndex] == 0xFF && nIndex < nStreamLength) nIndex++;
			break;
		}
		else {
			break; // block with pixel data found, start hashing from here
		}
	} while (nIndex < nStreamLength);

	if (pStream[nIndex] != 0) {
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
	uint32 nSizeLinePadded = Helpers::DoPadding(sizeThumb.cx*3, 4);
	uint32 nSizeBytes = nSizeLinePadded*sizeThumb.cy;
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
bool CreateAtomicTempFileName(LPCTSTR sTarget, CString& sTempOut) {
	CString sTargetStr(sTarget);
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
			::CloseHandle(hFile);
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

// Returns the compressed JPEG stream that must be freed by the caller. NULL in case of error.
static void* CompressAndSave(LPCTSTR sFileName, CJPEGImage * pImage, 
							 void* pData, int nWidth, int nHeight, int nQuality, int& nJPEGStreamLen, 
							 bool& tjFreeNeeded, bool bCopyEXIF, bool bDeleteThumbnail) {
	nJPEGStreamLen = 0;
	tjFreeNeeded = true;
	bool bOutOfMemory;
	unsigned char* pTargetStream = (unsigned char*) TurboJpeg::Compress(pData, nWidth, nHeight, 
		nJPEGStreamLen, bOutOfMemory, nQuality);
	if (pTargetStream == NULL) {
		return NULL;
	}

	CString sTempFile;
	if (!CreateAtomicTempFileName(sFileName, sTempFile)) {
		TurboJpeg::Free(pTargetStream);
		return NULL;
	}
	FILE *fptr = _tfopen(sTempFile, _T("wb"));
	if (fptr == NULL) {
		DeleteAtomicTempFile(sTempFile);
		TurboJpeg::Free(pTargetStream);
		return NULL;
	}

	// If EXIF data is present, replace any JFIF block by this EXIF block to preserve the EXIF information
	if (pImage->GetEXIFData() != NULL && bCopyEXIF) {
		const int cnAdditionalThumbBytes = 32000;
		int nEXIFBlockLenCorrection = 0;
		unsigned char* pNewStream = new unsigned char[nJPEGStreamLen + pImage->GetEXIFDataLength() + cnAdditionalThumbBytes];
		memcpy(pNewStream, pTargetStream, 2); // copy SOI block
		memcpy(pNewStream + 2, pImage->GetEXIFData(), pImage->GetEXIFDataLength()); // copy EXIF block
		
		// Set image orientation back to normal orientation, we save the pixels as displayed
		CEXIFReader exifReader(pNewStream + 2, IF_JPEG);
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
					delete[] pJPEGThumb;
				}
				delete[] pDIBThumb;
			}
		}

		int nJFIFLength = GetJFIFBlockLength(pTargetStream);
		memcpy(pNewStream + 2 + pImage->GetEXIFDataLength() + nEXIFBlockLenCorrection, pTargetStream + 2 + nJFIFLength, nJPEGStreamLen - 2 - nJFIFLength);
		TurboJpeg::Free(pTargetStream);
		pTargetStream = pNewStream;
		tjFreeNeeded = false;
		nJPEGStreamLen = nJPEGStreamLen - nJFIFLength + pImage->GetEXIFDataLength() + nEXIFBlockLenCorrection;
	}

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

	// Success, return compressed JPEG stream
	return pTargetStream;
}

// pData must point to 24 bit BGR DIB
static bool SaveWebP(LPCTSTR sFileName, void* pData, int nWidth, int nHeight, bool bUseLosslessWEBP) {
	CString sTempFile;
	if (!CreateAtomicTempFileName(sFileName, sTempFile)) {
		return false;
	}
	FILE *fptr = _tfopen(sTempFile, _T("wb"));
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
	if (!CreateAtomicTempFileName(sFileName, sTempFile)) {
		return false;
	}
	FILE* fptr = _tfopen(sTempFile, _T("wb"));
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
	if (!CreateAtomicTempFileName(sFileName, sTempFile)) {
		return false;
	}
	FILE* fptr = _tfopen(sTempFile, _T("wb"));
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

// Copied from MS sample
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
   UINT  num = 0;          // number of image encoders
   UINT  size = 0;         // size of the image encoder array in bytes

   Gdiplus::GetImageEncodersSize(&num, &size);
   if(size == 0)
	  return -1;  // Failure

   Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));

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

	uint32 nSizeLinePadded = Helpers::DoPadding(imageSize.cx*3, 4);
	uint32 nSizeBytes = nSizeLinePadded*imageSize.cy;
	char* pDIB24bpp = new char[nSizeBytes];
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
}

bool CSaveImage::SaveImage(LPCTSTR sFileName, CJPEGImage * pImage, bool bUseLosslessWEBP)
{
	CImageProcessingParams paramsNotUsed;
	return SaveImage(sFileName, pImage, paramsNotUsed, PFLAG_None, false, bUseLosslessWEBP, false);
}

bool CSaveImage::ConvertToJXL(LPCTSTR sJxlFile, LPCTSTR sJpegFile) {
	CString sTempFile;
	if (!CreateAtomicTempFileName(sJxlFile, sTempFile)) {
		return false;
	}
	FILE* fptr = _tfopen(sTempFile, _T("wb"));
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
