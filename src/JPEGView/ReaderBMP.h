#pragma once

class CJPEGImage;

// Simple reader for windows bitmap files (.bmp)
class CReaderBMP
{
public:
	// Returns NULL in case of errors
	static CJPEGImage* ReadBmpImage(LPCTSTR strFileName, bool& bOutOfMemory, bool& bSizeLimitExceeded); // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
private:
	CReaderBMP(void);
};
