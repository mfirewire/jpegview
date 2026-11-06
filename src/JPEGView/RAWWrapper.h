#pragma once

#include "JPEGImage.h"

class RawReader
{
public:
	static CJPEGImage* ReadImage(LPCTSTR strFileName, bool& bOutOfMemory, bool& bSizeLimitExceeded, bool bGetThumb); // R-04/P2-5: set to true when the image exceeds the pixel cap (MaxImagePixelsMP); only ever set, never cleared here - the caller initializes it to false once per request
};
