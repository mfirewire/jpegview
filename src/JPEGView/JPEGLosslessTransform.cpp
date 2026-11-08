#include "StdAfx.h"
#include "JPEGLosslessTransform.h"
#include "Helpers.h"
#include "SaveImage.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "MaxImageDef.h" // A4: MAX_JPEG_FILE_SIZE

CJPEGLosslessTransform::EResult _DoTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, tjtransform &transform);
unsigned char* _ReadFile(LPCTSTR sFileName, unsigned int & nLengthBytes);
bool _WriteFile(LPCTSTR sFileName, unsigned char* pBuffer, unsigned int nLengthBytes);
int _TransformationEnumToOpCode(CJPEGLosslessTransform::ETransformation transformation);

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