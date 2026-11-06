#pragma once

class CJPEGImage;

// Simple reader for .tga files
class CReaderTGA
{
public:
	// Returns NULL in case of errors. backgroundColor is used for blending transparent parts of the image.
	static CJPEGImage* ReadTgaImage(LPCTSTR strFileName, COLORREF backgroundColor, bool& bOutOfMemory, bool& bSizeLimitExceeded); // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
private:
	CReaderTGA(void);
};
