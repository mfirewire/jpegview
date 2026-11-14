#include "StdAfx.h"
#include "JPEGLosslessTransform.h"
#include "Helpers.h"
#include "SaveImage.h"
#include "EXIFReader.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "MaxImageDef.h" // A4: MAX_JPEG_FILE_SIZE

CJPEGLosslessTransform::EResult _DoTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, tjtransform &transform);
unsigned char* _ReadFile(LPCTSTR sFileName, unsigned int & nLengthBytes);
bool _WriteFile(LPCTSTR sFileName, unsigned char* pBuffer, unsigned int nLengthBytes);
int _TransformationEnumToOpCode(CJPEGLosslessTransform::ETransformation transformation);
void _UpdateEXIFOrientation(unsigned char* pJPEG, size_t nJPEGSize, int nTransformOp);

// Performs a lossless JPEG transformation, transforming the input file and writing the result to the output file.
// Input and output file can be identical, then the input file is overwritten by the resulting output file.
CJPEGLosslessTransform::EResult CJPEGLosslessTransform::PerformTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, 
	CJPEGLosslessTransform::ETransformation transformation, bool bAllowTrim) {
	tjtransform transform{ 0 };
	transform.op = _TransformationEnumToOpCode(transformation);
	transform.options = bAllowTrim ? TJXOPT_TRIM : TJXOPT_PERFECT;

	return _DoTransformation(sInputFile, sOutputFile, transform);
}

// Performs a lossless JPEG crop, using the input file and writing the result to the output file.
// Input and output file can be identical, then the input file is overwritten by the resulting output file.
CJPEGLosslessTransform::EResult CJPEGLosslessTransform::PerformCrop(LPCTSTR sInputFile, LPCTSTR sOutputFile, const CRect& cropRect) {
	tjtransform transform{ 0 };
	transform.op = TJXOP_NONE;
	transform.options = TJXOPT_PERFECT | TJXOPT_CROP;
	transform.r.x = cropRect.left;
	transform.r.y = cropRect.top;
	transform.r.w = cropRect.Width();
	transform.r.h = cropRect.Height();

	return _DoTransformation(sInputFile, sOutputFile, transform);
}


static CJPEGLosslessTransform::EResult _DoTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, tjtransform &transform) {
	CJPEGLosslessTransform::EResult eResult = CJPEGLosslessTransform::Success;

	tjhandle hTransform = tj3Init(TJINIT_TRANSFORM);

	unsigned int nNumBytesInput;
	unsigned char* pInputJPEGBytes = _ReadFile(sInputFile, nNumBytesInput);
	if (pInputJPEGBytes != NULL) {
		unsigned char* pOutputJPEGBytes = NULL;
		size_t nNumBytesOutput = 0;
		if (0 == tj3Transform(hTransform, pInputJPEGBytes, nNumBytesInput, 1, &pOutputJPEGBytes, &nNumBytesOutput, &transform) && pOutputJPEGBytes != NULL) {
			// R-A1: keep the EXIF orientation in step with the transformed
			// pixels (see _UpdateEXIFOrientation below); without it the
			// next load applies the old orientation on top of the
			// transformed pixels.
			_UpdateEXIFOrientation(pOutputJPEGBytes, nNumBytesOutput, transform.op);
			if (!_WriteFile(sOutputFile, pOutputJPEGBytes, nNumBytesOutput)) {
				eResult = CJPEGLosslessTransform::WriteFileFailed;
			}
		} else {
			eResult = CJPEGLosslessTransform::TransformationFailed;
		}
		if (pOutputJPEGBytes != NULL) {
			tj3Free(pOutputJPEGBytes);
		}
	} else {
		eResult = CJPEGLosslessTransform::ReadFileFailed;
	}

	delete[] pInputJPEGBytes;

	tj3Destroy(hTransform);

	return eResult;
}

static unsigned char* _ReadFile(LPCTSTR sFileName, unsigned int & nLengthBytes) {
	// A4: this used to be a private 50 MB cap, below the loader's own limit, so
	// a JPEG that JPEGView could open could not be rotated or cropped. Use the
	// same per-file limit the loader applies (MaxImageDef.h: 300 MB on x64,
	// 50 MB on Win32). The decoded frame still has to fit MAX_IMAGE_BYTES.

	nLengthBytes = 0;
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	long long nFileSize = Helpers::GetFileSize(hFile);
	if (nFileSize > MAX_JPEG_FILE_SIZE) {
		::CloseHandle(hFile);
		return NULL;
	}

	unsigned char *pBuffer = new(std::nothrow) unsigned char[nFileSize];
	if (pBuffer == NULL) {
		::CloseHandle(hFile);
		return NULL;
	}

	unsigned int nNumBytesRead;
	if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD) &nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
		::CloseHandle(hFile);
		nLengthBytes = nFileSize;
		return pBuffer;
	}
	delete[] pBuffer;
	::CloseHandle(hFile);
	return NULL;
}

static bool _WriteFile(LPCTSTR sFileName, unsigned char* pBuffer, unsigned int nLengthBytes) {
	// Always write through a unique temp file in the target's directory
	// and swap it into place only on full success. The old code used the
	// fixed name "<target>.tmp" with CREATE_ALWAYS, so two JPEGView
	// instances transforming the same file truncated and interleaved into
	// one shared temp file, and the first one to finish ReplaceFile'd the
	// mixture over the original. The input file was already read into
	// memory by _DoTransformation, so input and output may be the same
	// file; CommitAtomicTempFile handles both cases (ReplaceFile for an
	// existing target, MoveFile for a brand-new one).
	// P3-1: take the CREATE_NEW handle straight from
	// CreateAtomicTempFileName instead of closing it and reopening the
	// temp file by name (the old create-close-reopen sequence had a
	// TOCTOU window). The handle has the same access/share as the
	// reopened one (GENERIC_WRITE, no sharing).
	CString sTempFile;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	if (!CreateAtomicTempFileName(sFileName, sTempFile, hFile)) {
		return false;
	}

	unsigned int nNumBytesWritten;
	bool bOk = ::WriteFile(hFile, pBuffer, nLengthBytes, (LPDWORD) &nNumBytesWritten, NULL) && nNumBytesWritten == nLengthBytes;

	// M-U2: the mtime is deliberately left at the write time, not restored
	// to the original file's mtime: restoring it made a second instance's
	// watcher and date-based sorting miss the change. (This is not the
	// UserCommand "keep modification date" option, which is an explicit
	// user opt-in and is untouched.)
	::CloseHandle(hFile);

	if (!bOk) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}
	// On commit failure the target is untouched (ReplaceFile leaves it
	// alone, MoveFile either succeeds or leaves both files); the temp
	// file is removed. This must not be delete-then-rename: if the delete
	// succeeds and the rename fails the original is gone.
	if (!CommitAtomicTempFile(sFileName, sTempFile)) {
		DeleteAtomicTempFile(sTempFile);
		return false;
	}

	return true;
}

// R-A1: EXIF orientation after a lossless transform. tj3Transform copies
// the EXIF APP1 verbatim (TJPARAM_SAVEMARKERS defaults to 2, "copy all
// extra markers"; TJXOPT_COPYNONE is not set), so the Orientation tag
// still describes the pre-transform stored pixels. If the tag is left
// stale, the next load applies the old orientation on top of the
// transformed pixels - a mirror on an orientation-6 phone photo comes
// back flipped twice. The tag must therefore become O' with
// O'(T(S)) == T(O(S)) (conjugation T . O . T^-1), where S are the stored
// pixels, O the old orientation and T the transform. The table below was
// derived by brute force over small pixel grids: rotations commute with
// pure-rotation orientations (1/3/6/8 are unchanged by ROT90/180/270),
// flips swap 6 <-> 8, and rotations swap 2 <-> 4 and 5 <-> 7. A crop
// (TJXOP_NONE) does not rotate or flip anything, so the tag is unchanged.
// The write is in place (2 bytes, same length), reusing CEXIFReader just
// like the save path (SaveImage.cpp). No EXIF block, no orientation tag,
// or an out-of-range value: leave the bytes untouched - there is nothing
// to compose, and failing the transform over metadata would be worse.
// Known limitation, shared with jpegtran: the embedded EXIF thumbnail
// pixels are not transformed, so a viewer showing the raw thumbnail
// still sees the pre-transform picture; the main image is correct.
static const int kComposedOrientation[9][5] = {
	//              ROT90 ROT180 ROT270 HFLIP VFLIP
	/* O=1 */ { 1, 1, 1, 1, 1 },
	/* O=2 */ { 4, 2, 4, 2, 2 },
	/* O=3 */ { 3, 3, 3, 3, 3 },
	/* O=4 */ { 2, 4, 2, 4, 4 },
	/* O=5 */ { 7, 5, 7, 7, 7 },
	/* O=6 */ { 6, 6, 6, 8, 8 },
	/* O=7 */ { 5, 7, 5, 5, 5 },
	/* O=8 */ { 8, 8, 8, 6, 6 },
};

static void _UpdateEXIFOrientation(unsigned char* pJPEG, size_t nJPEGSize, int nTransformOp) {
	if (pJPEG == NULL || nJPEGSize < 4 || nJPEGSize > INT_MAX) {
		return;
	}
	// A crop does not rotate or flip the pixels, so the orientation tag
	// still describes them.
	if (nTransformOp == TJXOP_NONE) {
		return;
	}
	int nColumn = -1;
	switch (nTransformOp) {
		case TJXOP_ROT90: nColumn = 0; break;
		case TJXOP_ROT180: nColumn = 1; break;
		case TJXOP_ROT270: nColumn = 2; break;
		case TJXOP_HFLIP: nColumn = 3; break;
		case TJXOP_VFLIP: nColumn = 4; break;
		default: return; // TJXOP_TRANSPOSE/TRANSVERSE are never requested
	}
	void* pEXIFBlock = Helpers::FindEXIFBlock(pJPEG, (int)nJPEGSize);
	if (pEXIFBlock == NULL) {
		return; // no EXIF block: nothing to update
	}
	CEXIFReader exifReader((unsigned char*)pEXIFBlock, IF_JPEG);
	int nOldOrientation = exifReader.GetImageOrientation();
	if (nOldOrientation < 1 || nOldOrientation > 8) {
		return; // no (valid) orientation tag: nothing to compose
	}
	int nNewOrientation = kComposedOrientation[nOldOrientation][nColumn];
	if (nNewOrientation != nOldOrientation) {
		exifReader.WriteImageOrientation(nNewOrientation);
	}
}

static int _TransformationEnumToOpCode(CJPEGLosslessTransform::ETransformation transformation) {
	switch (transformation) {
		case CJPEGLosslessTransform::Rotate90:
			return TJXOP_ROT90;
		case CJPEGLosslessTransform::Rotate270:
			return TJXOP_ROT270;
		case CJPEGLosslessTransform::Rotate180:
			return TJXOP_ROT180;
		case CJPEGLosslessTransform::MirrorH:
			return TJXOP_HFLIP;
		case CJPEGLosslessTransform::MirrorV:
			return TJXOP_VFLIP;
		default:
			return TJXOP_NONE;
	}
}