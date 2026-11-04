#pragma once

#include "ProcessParams.h"

class CJPEGImage;

// Atomic-save helpers shared with the lossless JPEG transform
// (JPEGLosslessTransform.cpp). Defined in SaveImage.cpp: the temp file
// is created in the target's directory (same volume, so ReplaceFile
// works) with a unique name, and only on full success swapped into
// place; on any failure the caller deletes the temp file and the
// target is left untouched.
bool CreateAtomicTempFileName(LPCTSTR sTarget, CString& sTempOut);
void DeleteAtomicTempFile(LPCTSTR sTemp);
bool CommitAtomicTempFile(LPCTSTR sTarget, LPCTSTR sTemp);

// Class that enables to save a processed image to a one of the supported file formats
class CSaveImage
{
public:
	// Save processed image to file. Returns if saving was successful or not.
	// The file format is derived from the file ending of the specified file name.
	// If bFullSize is true, the image in its original size is processed and saved.
	// If bFullSize is false, the image section as shown in the window is saved.
	// Processing parameters and flags are ignored when bFullSize is false, the image is not reprocessed in this case.
	static bool SaveImage(LPCTSTR sFileName, CJPEGImage * pImage, const CImageProcessingParams& procParams,
		EProcessingFlags eFlags, bool bFullSize, bool bUseLosslessWEBP, bool bCreateParameterDBEntry = true);

	// Saves processed image in current window size as displayed on screen. Creates no parameter DB entry for the saved image.
	// The file format is derived from the file ending of the specified file name.
	static bool SaveImage(LPCTSTR sFileName, CJPEGImage * pImage, bool bUseLosslessWEBP);

	// Losslessy transcode a JPEG file into a JXL
	static bool ConvertToJXL(LPCTSTR sJxlFile, LPCTSTR sJpegFile);

private:
	CSaveImage(void);
};
