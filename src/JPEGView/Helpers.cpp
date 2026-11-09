#include "StdAfx.h"
#include "Helpers.h"
#include "immintrin.h"
#include "NLS.h"
#include "MultiMonitorSupport.h"
#include "JPEGImage.h"
#include "FileList.h"
#include "SettingsProvider.h"
#include <math.h>
#include <new>

namespace Helpers {

float ScreenScaling = -1.0f;

TCHAR CReplacePipe::sm_buffer[MAX_SIZE_REPLACE_PIPE];

CReplacePipe::CReplacePipe(LPCTSTR sText) {
	_tcsncpy_s(sm_buffer, MAX_SIZE_REPLACE_PIPE, sText, MAX_SIZE_REPLACE_PIPE);
	sm_buffer[MAX_SIZE_REPLACE_PIPE-1] = 0;
	TCHAR* pPtr = sm_buffer;
	while (*pPtr != 0) {
		if (*pPtr == _T('|')) *pPtr = 0;
		pPtr++;
	}
}

static TCHAR buffAppPath[MAX_PATH + 32] = _T("");

LPCTSTR JPEGViewAppDataPath() {
	if (buffAppPath[0] == 0) {
		::SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffAppPath);
		_tcscat_s(buffAppPath, MAX_PATH + 32, _T("\\JPEGView\\"));
	}
	return buffAppPath;
}

void SetJPEGViewAppDataPath(LPCTSTR sPath) {
	_tcscpy_s(buffAppPath, MAX_PATH + 32, sPath);
}

CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, 
					bool bAllowZoomIn, bool bFillCrop, bool bLimitAR, double & dZoom) {
	double dAR1 = (double)nWidth/nScreenWidth;
	double dAR2 = (double)nHeight/nScreenHeight;
	double dARMin = min(dAR1, dAR2);
	double dARMax = max(dAR1, dAR2);
	double dAR = bFillCrop ? dARMin : dARMax;
	if (dAR <= 1.0 && !bAllowZoomIn) {
		if (bFillCrop) dAR = dARMax;
		if (dAR <= 1.0) {
			dZoom = 1.0;
			return CSize(nWidth, nHeight);
		}
	}
	if (bFillCrop && bLimitAR) {
		if (((nWidth/dAR * nHeight/dAR) / (nScreenWidth * nScreenHeight)) > 1.5) {
			dAR = dARMax; // use fit to screen
		}
	}
	dZoom = 1.0/dAR;
	dZoom = min(ZoomMax, dZoom);
	CSize size((int)(nWidth/dAR + 0.5), (int)(nHeight/dAR + 0.5));
	if (abs(size.cx - nScreenWidth) <= 1 && abs(size.cy - nScreenHeight) <= 1) {
		// allow 1 pixel tolerance to screen size - prefer screen size in this case to prevent another resize operation to fit image
		dZoom = (nScreenWidth > nScreenHeight) ? (double)nScreenWidth/nWidth : (double)nScreenHeight/nHeight;
		dZoom = min(ZoomMax, dZoom);
		return CSize(nScreenWidth, nScreenHeight);
	}
	return size;
}

CString SystemTimeToString(const SYSTEMTIME &time) {
	TCHAR sBufferDate[64];
	TCHAR sBufferDay[8];
	TCHAR sBufferTime[64];
	::GetDateFormat(LOCALE_USER_DEFAULT, 0, &time, _T("ddd"), sBufferDay, 8);
	::GetDateFormat(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &time, NULL, sBufferDate, 64);
	::GetTimeFormat(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &time, NULL, sBufferTime, 64);
	return CString(sBufferDay) + _T(" ") + sBufferDate + _T(" ") + sBufferTime;
}

CSize GetImageRect(int nWidth, int nHeight, int nScreenWidth, int nScreenHeight, EAutoZoomMode eAutoZoomMode, double & dZoom) {
	CSize newSize = GetImageRect(nWidth, nHeight, nScreenWidth, nScreenHeight,
		eAutoZoomMode == ZM_FitToScreen || eAutoZoomMode == ZM_FillScreen,
		eAutoZoomMode == ZM_FillScreenNoZoom || eAutoZoomMode == ZM_FillScreen,
		eAutoZoomMode == ZM_FillScreenNoZoom, dZoom);
	return CSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
}

CSize GetVirtualImageSize(CSize originalImageSize, CSize screenSize, EAutoZoomMode eAutoZoomMode, double & dZoom) {
	CSize newSize;
	if (dZoom < 0.0) {
		// zoom not set, interpret as 'fit to screen'
		// ---------------------------------------------
		newSize = Helpers::GetImageRect(originalImageSize.cx, originalImageSize.cy, screenSize.cx, screenSize.cy, eAutoZoomMode, dZoom);
	} else {
		// zoom set, use this value for the new size
		// ---------------------------------------------
		newSize = CSize((int)(originalImageSize.cx * dZoom + 0.5), (int)(originalImageSize.cy * dZoom + 0.5));
	}
	newSize.cx = max(1, min(65535, newSize.cx));
	newSize.cy = max(1, min(65535, newSize.cy));
	return newSize;
}

CPoint LimitOffsets(const CPoint& offsets, const CSize & rectSize, const CSize & outerRect) {
	int nMaxOffsetX = (outerRect.cx - rectSize.cx)/2;
	nMaxOffsetX = max(0, nMaxOffsetX);
	int nMaxOffsetY = (outerRect.cy - rectSize.cy)/2;
	nMaxOffsetY = max(0, nMaxOffsetY);
	return CPoint(max(-nMaxOffsetX, min(+nMaxOffsetX, offsets.x)), max(-nMaxOffsetY, min(+nMaxOffsetY, offsets.y)));
}

CRect InflateRect(const CRect& rect, float fAmount) {
	CRect r(rect);
	int nAmount = (int)(fAmount * r.Width());
	r.InflateRect(-nAmount, -nAmount);
	return ((r.Height() & 1) == 1) ? CRect(r.left, r.top, r.right, r.bottom - 1) : r;
}

CRect FitRectIntoRect(CSize size, CRect fitRect) {
	double ratioSource = (double)size.cx / size.cy;
	double ratioTarget = (double)fitRect.Width() / fitRect.Height();
	if (ratioSource > ratioTarget) {
		// fit x
		int targetHeight = Helpers::RoundToInt(fitRect.Width() / ratioSource);
		int startY = fitRect.top + (fitRect.Height() - targetHeight) / 2;
		return CRect(fitRect.left, startY, fitRect.right, startY + targetHeight);
	} else {
		// fit y
		int targetWidth = Helpers::RoundToInt(fitRect.Height() * ratioSource);
		int startX = fitRect.left + (fitRect.Width() - targetWidth) / 2;
		return CRect(startX, fitRect.top, startX + targetWidth, fitRect.bottom);
	}
}

CRect FillRectAroundRect(CSize size, CRect fillRect) {
	double ratioSource = (double)size.cx / size.cy;
	double ratioTarget = (double)fillRect.Width() / fillRect.Height();
	if (ratioSource > ratioTarget) {
		// fill height
		int targetWidth = Helpers::RoundToInt(fillRect.Height() * ratioSource);
		int startX = fillRect.left + (fillRect.Width() - targetWidth) / 2;
		return CRect(startX, fillRect.top, startX + targetWidth, fillRect.bottom);
	} else {
		// fill width
		int targetHeight = Helpers::RoundToInt(fillRect.Width() / ratioSource);
		int startY = fillRect.top + (fillRect.Height() - targetHeight) / 2;
		return CRect(fillRect.left, startY, fillRect.right, startY + targetHeight);
	}
}

void GetZoomParameters(float & fZoom, CPoint & offsets, CSize imageSize, CSize windowSize, CRect zoomRect) {
	int nZoomWidth = zoomRect.Width();
	int nZoomHeight = zoomRect.Height();

	bool bTakeW = ((float)windowSize.cx/windowSize.cy < (float)nZoomWidth/nZoomHeight);
	if (bTakeW) {
		// Width is dominating
		fZoom = (float)windowSize.cx/nZoomWidth;
	} else {
		// Height is dominating
		fZoom = (float)windowSize.cy/nZoomHeight;
	}
	if (fZoom < 0.0001 || fZoom > Helpers::ZoomMax) {
		fZoom = -1.0f;
		return;
	}

	int nZoomRectMiddleX = (zoomRect.right + zoomRect.left)/2;
	int nOffsetX = Helpers::RoundToInt(fZoom*imageSize.cx*0.5 - fZoom*nZoomRectMiddleX);

	int nZoomRectMiddleY = (zoomRect.bottom + zoomRect.top)/2;
	int nOffsetY = Helpers::RoundToInt(fZoom*imageSize.cy*0.5 - fZoom*nZoomRectMiddleY);

	offsets = CPoint(nOffsetX, nOffsetY);
}

#ifdef _WIN64
static CPUType ProbeSSEorAVX2() {
	__try {
		// check if CPU supports AVX and the xgetbv instruction
		int abcd[4];
		__cpuid(abcd, 1);
		if ((abcd[2] & 0x18000000) != 0x18000000) // AVX and OSXSAVE bits
			return CPU_SSE;
		if ((abcd[2] & 0x04000000) == 0) // XSAVE bit, support for xgetbv
			return CPU_SSE;

		// check if operating system supports AVX(2)
		unsigned long long xcr0 = _xgetbv(0);
		if ((xcr0 & 6) != 6)
			return CPU_SSE; // nope, only use SSE

		// check if AVX2 instructions are supported
		const int AVX2BITMASK = 1 << 5;
		__cpuidex(abcd, 7, 0);
		return (abcd[1] & AVX2BITMASK) ? CPU_AVX2 : CPU_SSE;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return CPU_SSE;
	}
}
#endif

CPUType ProbeCPU(void) {
	static CPUType cpuType = CPU_Unknown;
	if (cpuType != CPU_Unknown) {
		return cpuType;
	}

#ifdef _WIN64
	return ProbeSSEorAVX2(); // 64 bit always supports at least SSE
#else
	// Structured exception handling is mandatory, try/catch(...) does not catch such severe stuff.
	cpuType = CPU_Generic;
	__try {
		uint32 FeatureMask;
		_asm {
			mov eax, 1
			cpuid
			mov FeatureMask, edx
		}
		if ((FeatureMask & (1 << 26)) != 0) {
			cpuType = CPU_SSE; // this means SSE2
		} else if ((FeatureMask & (1 << 25)) != 0) {
			cpuType = CPU_MMX; // yes, we need SSE as the pmax/pmin stuff was coming with the PIII and SSE
		} else {
			// last chance - check if AMD and if yes, test for AMD MMX extensions that also implement pmax/pmin
			_asm {
				mov eax, 0
				cpuid
				cmp ebx, 0x68747541 // is AMD processor?
				jne GiveUp
				mov eax, 0x80000001
				cpuid
				mov FeatureMask, edx
GiveUp:
				mov FeatureMask, 0
			}
			if ((FeatureMask & (1 << 22)) != 0) {
				cpuType = CPU_MMX; // extended AMD MMX instructions
			}
		}
		
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
		// even CPUID is not supported, use generic code
		return cpuType;
	}
	return cpuType;
#endif
}

// returns if the CPU supports some form of hardware multiprocessing, e.g. hyperthreading or multicore
static bool CPUSupportsHWMultiprocessing(void) {   
	if (ProbeCPU() >= CPU_SSE) {
		int output[4];
		__cpuid(output, 1);
		return (output[3] & 0x10000000);
	} else {
		return false;
	}  
}

int NumCoresPerPhysicalProc(void) {
	if (!CPUSupportsHWMultiprocessing()) {
		return 1;
	}

	int output[4];

	// check if cpuid supports leaf 4
	__cpuid(output, 0);
	if (output[0] < 4)
		return 1; // not support, single core

	// start with index = 0; Leaf 4 reports
	__cpuidex(output, 4, 0);

	return (int)((output[0] & 0xFC000000) >> 26) + 1;
}

bool PatternMatch(LPCTSTR & sMatchingPattern, LPCTSTR sString, LPCTSTR sPattern) {
	sMatchingPattern = NULL;
	if (sString == NULL || sPattern == NULL || *sPattern == 0) return false;
	LPCTSTR pInStr = sString;
	LPCTSTR pInPat = sPattern;
	while (*pInPat != 0) {
		bool bThisPatternFails = false;
		while (*pInPat == _T(';')) pInPat++; // skip sequences of separators
		sMatchingPattern = pInPat;
		while (*pInPat != 0 && *pInPat != _T(';')) {
			bool bAsterix = false;
			if (*pInPat == _T('*')) {
				pInPat++;
				bAsterix = true;
			}
			LPCTSTR pPatStart = pInPat;
			while (*pPatStart != 0 && *pPatStart != _T('*') && *pPatStart != _T(';')) pPatStart++;
			int nPatLen = (int)(pPatStart - pInPat);
			if (nPatLen > 0) {
				if (bAsterix) {
					while (*pInStr != 0 && _tcsnicmp(pInPat, pInStr, nPatLen) != 0) pInStr++;
					if (*pInStr == 0) {
						bThisPatternFails = true;
						while (*pInPat != 0 && *pInPat != _T(';')) pInPat++;
					} else {
						pInStr += nPatLen;
						pInPat += nPatLen;
					}
				} else {
					if (_tcsnicmp(pInPat, pInStr, nPatLen) != 0) {
						bThisPatternFails = true;
						while (*pInPat != 0 && *pInPat != _T(';')) pInPat++;
					} else {
						pInStr += nPatLen;
						pInPat += nPatLen;
					}
				}
			}
		}
		if (!bThisPatternFails) return true;
		pInStr = sString;
		if (*pInPat != 0) pInPat++;
	}
	return false;
}

int FindMoreSpecificPattern(LPCTSTR sPattern1, LPCTSTR sPattern2) {
	if (sPattern1 == NULL) {
		return (sPattern2 == NULL) ? 0 : -1;
	} else if (sPattern2 == NULL) {
		return 1;
	}
	while (*sPattern1 != 0 && *sPattern1 != _T(';') && *sPattern2 != 0 && *sPattern2 != _T(';') &&
		_totlower(*sPattern1) == _totlower(*sPattern2)) {
		sPattern1++;
		sPattern2++;
	}
	if (*sPattern1 == 0 || *sPattern1 == _T(';')) {
		if (*sPattern2 == 0 || *sPattern2 == _T(';')) {
			return 0; // no one is more specific
		} else {
			return -1; // sPattern2 is more specific
		}
	} else if (*sPattern2 == 0 || *sPattern2 == _T(';')) {
		return 1; // sPattern1 is more specific
	} else {
		if (*sPattern1 == _T('*')) {
			return -1; // pattern 1 accepts any char, thus sPattern2 is more specific
		} else if (*sPattern2 == _T('*')) {
			return 1; // sPattern1 is more specific
		}
		return 0;
	}
}

// calculate CRT table
void CalcCRCTable(unsigned int crc_table[256]) {
	 for (int n = 0; n < 256; n++) {
	   unsigned int c = (unsigned int) n;
	   for (int k = 0; k < 8; k++) {
		 if (c & 1)
		   c = 0xedb88320L ^ (c >> 1);
		 else
		   c = c >> 1;
	   }
	   crc_table[n] = c;
	 }
}

void* FindJPEGMarker(void* pJPEGStream, int nStreamLength, unsigned char nMarker) {
	uint8* pStream = (uint8*) pJPEGStream;
	if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
		return NULL; // not a JPEG
	}
	int nIndex = 2;
	while (nIndex < nStreamLength) {
		if (pStream[nIndex] != 0xFF) {
			break; // block with pixel data found, start hashing from here
		}
		// block header found, skip padding bytes. Test the bound before reading:
		// the old loop read pStream[nIndex] first, so a stream ending in 0xFF read
		// one byte past the end.
		while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
		if (nIndex >= nStreamLength) {
			break;
		}
		if (pStream[nIndex] == 0 || pStream[nIndex] == nMarker) {
			break; // 0xFF 0x00 is part of pixel block, break
		}
		// it's a block marker, read length of block and skip the block
		nIndex++;
		if (nIndex + 1 >= nStreamLength) {
			nIndex = nStreamLength;
			break;
		}
		int nBlockLength = pStream[nIndex] * 256 + pStream[nIndex + 1];
		if (nBlockLength < 2) {
			break; // malformed: a length below 2 would not advance the index
		}
		nIndex += nBlockLength;
	}

	if (nIndex < 1 || nIndex >= nStreamLength) {
		return NULL;
	}
	if (nMarker == 0 || (pStream[nIndex] == nMarker && pStream[nIndex - 1] == 0xFF)) {
		return &(pStream[nIndex - 1]); // place on marker start
	}
	return NULL;
}

void* FindEXIFBlock(void* pJPEGStream, int nStreamLength) {
	uint8* pEXIFBlock = (uint8*)Helpers::FindJPEGMarker(pJPEGStream, nStreamLength, 0xE1);
	if (pEXIFBlock == NULL) {
		return NULL;
	}
	// The marker starts at FF E1; bytes 2..3 hold the segment length, which counts
	// itself, and the "Exif" identifier follows at +4. A truncated file that ends
	// right after the length field used to reach past the buffer here.
	int nOffset = (int)(pEXIFBlock - (uint8*)pJPEGStream);
	if (nOffset + 4 > nStreamLength) {
		return NULL;
	}
	int nBlockLength = pEXIFBlock[2] * 256 + pEXIFBlock[3];
	if (nBlockLength < 6 || nOffset + 2 + nBlockLength > nStreamLength) {
		return NULL;
	}
	if (strncmp((const char*)(pEXIFBlock + 4), "Exif", 4) != 0) {
		return NULL;
	}
	return pEXIFBlock;
}

// F-H10: the largest preserved-segments blob we keep. 16 MB matches the
// ICC cap below (MAX_ICC_PROFILE_SIZE): a real file carries far less than
// that in its header, so anything beyond it is a malformed length field,
// not metadata worth keeping.
static const int MAX_PRESERVED_SEGMENTS_SIZE = 16 * 1024 * 1024;

// Walks the JPEG marker chain from the SOI, collecting the APP/COM segments
// worth preserving on save (see FindPreservedJPEGSegments). When pDest is
// NULL the walk only measures; otherwise each kept segment is copied to
// pDest at the running offset. Returns the total kept size, or -1 when the
// stream is malformed (a truncated segment, a length below 2, an absurd
// header) - the caller then keeps nothing rather than a partial header.
// nSegmentsBeforeEXIF is set to the kept size at the moment the EXIF APP1
// is passed (the total size when there is no EXIF block).
// The loop mirrors FindJPEGMarker above: it stops at the first non-APP/COM
// marker (DQT, SOF, SOS, ...) and treats 0xFF 0x00 as scan data.
static int WalkPreservedJPEGSegments(uint8* pStream, int nStreamLength, unsigned char* pDest, int& nSegmentsBeforeEXIF, bool bExcludeCOM) {
	nSegmentsBeforeEXIF = 0;
	bool bSeenEXIF = false;
	int nKeptSize = 0;
	int nIndex = 2;
	while (nIndex < nStreamLength) {
		if (pStream[nIndex] != 0xFF) {
			break; // scan data (or SOS): no more header segments
		}
		// Skip padding 0xFF bytes; the byte before the marker is its 0xFF.
		while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) nIndex++;
		if (nIndex >= nStreamLength) {
			break;
		}
		unsigned char nMarker = pStream[nIndex];
		if (nMarker == 0x00 || nMarker == 0x01 || (nMarker >= 0xD0 && nMarker <= 0xD9)) {
			break; // stuffed byte, TEM, RSTn, SOI/EOI: not a header segment
		}
		int nSegStart = nIndex - 1; // the 0xFF
		nIndex++;
		if (nIndex + 1 >= nStreamLength) {
			return -1;
		}
		int nBlockLength = pStream[nIndex] * 256 + pStream[nIndex + 1];
		if (nBlockLength < 2) {
			return -1; // malformed: a length below 2 would not advance the index
		}
		int nSegLen = 2 + nBlockLength; // marker (2) + length field (counts itself)
		if (nSegStart + nSegLen > nStreamLength) {
			return -1; // truncated segment
		}
		bool bKeep = false;
		if (nMarker == 0xE0) {
			// APP0: only JFIF (density info); JFXX and friends are skipped.
			bKeep = nBlockLength >= 7 && memcmp(pStream + nSegStart + 4, "JFIF\0", 5) == 0;
		} else if (nMarker == 0xE1) {
			// APP1: the EXIF block itself is handled separately (it is
			// rewritten on save); every other APP1 (XMP, extended XMP) is kept.
			// S-5: match FindEXIFBlock (length >= 6, first 4 bytes "Exif"),
			// not just a well-formed "Exif\0\0" block, so a malformed EXIF
			// APP1 is excluded here instead of being kept AND rewritten.
			if (nBlockLength >= 6 && memcmp(pStream + nSegStart + 4, "Exif", 4) == 0) {
				if (!bSeenEXIF) {
					nSegmentsBeforeEXIF = nKeptSize;
					bSeenEXIF = true;
				}
				bKeep = false;
			} else {
				bKeep = true;
			}
		} else if (nMarker == 0xE2) {
			// APP2: only ICC_PROFILE chunks (a profile may span several).
			bKeep = nBlockLength >= 14 && memcmp(pStream + nSegStart + 4, "ICC_PROFILE\0", 12) == 0;
		} else if (nMarker == 0xED) {
			// APP13: only Photoshop 3.0 (IPTC).
			bKeep = nBlockLength >= 16 && memcmp(pStream + nSegStart + 4, "Photoshop 3.0\0", 14) == 0;
		} else if (nMarker == 0xFE) {
			// COM: kept unless the caller already has the comment as a
			// string (it is re-inserted from the string on save).
			bKeep = !bExcludeCOM;
		} else if (nMarker >= 0xE0 && nMarker <= 0xEF) {
			// Other APPn: not in the preserved set (e.g. APP14 Adobe).
			bKeep = false;
		} else {
			// DQT, DHT, SOF, SOS, ...: the header ends here.
			break;
		}
		if (bKeep) {
			if (nKeptSize + nSegLen > MAX_PRESERVED_SEGMENTS_SIZE) {
				return -1; // absurd header, keep nothing
			}
			if (pDest != NULL) {
				memcpy(pDest + nKeptSize, pStream + nSegStart, nSegLen);
			}
			nKeptSize += nSegLen;
		}
		nIndex += nBlockLength;
	}
	if (!bSeenEXIF) {
		// No EXIF block: everything kept stood "before" it.
		nSegmentsBeforeEXIF = nKeptSize;
	}
	return nKeptSize;
}

void* FindPreservedJPEGSegments(void* pJPEGStream, int nStreamLength, int& nSegmentsSize, int& nSegmentsBeforeEXIF, bool bExcludeCOM) {
	nSegmentsSize = 0;
	nSegmentsBeforeEXIF = 0;
	uint8* pStream = (uint8*)pJPEGStream;
	if (pStream == NULL || nStreamLength < 3 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
		return NULL; // not a JPEG
	}
	// Two passes over a header that is at most a few KB: measure, then copy.
	int nMeasured = WalkPreservedJPEGSegments(pStream, nStreamLength, NULL, nSegmentsBeforeEXIF, bExcludeCOM);
	if (nMeasured <= 0) {
		return NULL;
	}
	unsigned char* pSegments = new(std::nothrow) unsigned char[nMeasured];
	if (pSegments == NULL) {
		return NULL;
	}
	int nBeforeEXIF = 0;
	int nCopied = WalkPreservedJPEGSegments(pStream, nStreamLength, pSegments, nBeforeEXIF, bExcludeCOM);
	if (nCopied != nMeasured || nBeforeEXIF != nSegmentsBeforeEXIF) {
		// Cannot happen (the stream is unchanged between the two walks);
		// keep nothing rather than a half-copied header.
		delete[] pSegments;
		return NULL;
	}
	nSegmentsSize = nMeasured;
	return pSegments;
}


// A profile of this size is not something a camera or an editor produces; it is
// a broken length field that would otherwise make us allocate a lot of memory.
static const int MAX_ICC_PROFILE_SIZE = 16 * 1024 * 1024;

void* ExtractICCProfile(void* pJPEGStream, int nStreamLength, unsigned int* pProfileSize) {
	if (pProfileSize != NULL) {
		*pProfileSize = 0;
	}
	uint8* pStream = (uint8*) pJPEGStream;
	if (pStream == NULL || nStreamLength < 4 || pStream[0] != 0xFF || pStream[1] != 0xD8) {
		return NULL;
	}

	// Each part of the profile sits in an APP2 segment: "ICC_PROFILE\0", one byte
	// sequence number (1-based), one byte total count, then the profile bytes.
	struct Chunk {
		int nSequence;
		const uint8* pData;
		int nSize;
	};
	Chunk chunks[256];
	int nChunks = 0;
	int nExpectedCount = 0;
	int nTotalSize = 0;
	bool bCountMismatch = false;

	int nIndex = 2;
	while (nIndex + 4 <= nStreamLength) {
		if (pStream[nIndex] != 0xFF) {
			break; // not a segment any more
		}
		while (nIndex < nStreamLength && pStream[nIndex] == 0xFF) {
			nIndex++; // padding before the marker code
		}
		if (nIndex >= nStreamLength) {
			break;
		}
		uint8 nMarker = pStream[nIndex];
		if (nMarker == 0 || nMarker == 0xD9 || nMarker == 0xDA) {
			break; // stuffed byte, end of image or start of scan: segments are over
		}
		nIndex++;
		if (nIndex + 1 >= nStreamLength) {
			break;
		}
		int nSegmentLength = (pStream[nIndex] << 8) | pStream[nIndex + 1];
		if (nSegmentLength < 2 || nIndex + nSegmentLength > nStreamLength) {
			break; // truncated file
		}
		// 2 bytes of length, 12 bytes of identifier, 2 bytes of sequence and count
		if (nMarker == 0xE2 && nSegmentLength >= 16 && memcmp(pStream + nIndex + 2, "ICC_PROFILE\0", 12) == 0) {
			int nSequence = pStream[nIndex + 14];
			int nCount = pStream[nIndex + 15];
			if (nSequence >= 1 && nCount >= 1 && nChunks < 256) {
				if (nChunks == 0) {
					nExpectedCount = nCount;
				} else if (nCount != nExpectedCount) {
					// Every part must agree on the total. Taking the last segment's
					// count let a truncated profile (seq1/count=3 + seq2/count=2) pass
					// as if it were complete.
					bCountMismatch = true;
					break;
				}
				chunks[nChunks].nSequence = nSequence;
				chunks[nChunks].pData = pStream + nIndex + 16;
				chunks[nChunks].nSize = nSegmentLength - 16;
				nChunks++;
				nTotalSize += nSegmentLength - 16;
			}
		}
		nIndex += nSegmentLength;
	}

	// All parts have to be there, otherwise the profile would be corrupted.
	if (bCountMismatch || nChunks == 0 || nChunks != nExpectedCount || nTotalSize > MAX_ICC_PROFILE_SIZE) {
		return NULL;
	}

	// Parts need not arrive in order; sort them by sequence number (insertion sort,
	// at most a handful of entries).
	for (int i = 1; i < nChunks; i++) {
		Chunk key = chunks[i];
		int j = i - 1;
		while (j >= 0 && chunks[j].nSequence > key.nSequence) {
			chunks[j + 1] = chunks[j];
			j--;
		}
		chunks[j + 1] = key;
	}

	uint8* pProfile = new(std::nothrow) uint8[nTotalSize];
	if (pProfile == NULL) {
		return NULL;
	}
	int nOffset = 0;
	for (int i = 0; i < nChunks; i++) {
		if (chunks[i].nSequence != i + 1) {
			delete[] pProfile;
			return NULL; // a sequence number is missing
		}
		memcpy(pProfile + nOffset, chunks[i].pData, chunks[i].nSize);
		nOffset += chunks[i].nSize;
	}

	if (pProfileSize != NULL) {
		*pProfileSize = (unsigned int) nOffset;
	}
	return pProfile;
}

__int64 CalculateJPEGFileHash(void* pJPEGStream, int nStreamLength) {
	uint8* pStream = (uint8*) pJPEGStream;
	void* pPixelStart = FindJPEGMarker(pJPEGStream, nStreamLength, 0);
	if (pPixelStart == NULL) {
		return 0;
	}
	int nIndex = (int)((uint8*)pPixelStart - (uint8*)pJPEGStream + 1);
	
	// take whole stream in case of inconsistency or if remaining part is too small
	if (nStreamLength - nIndex < 4) {
		nIndex = 0;
		assert(false);
	}

	// now we can calculate the hash over the compressed pixel data
	// do not look at every byte due to performance reasons
	const int nTotalLookups = 10000;
	int nIncrement = (nStreamLength - nIndex)/nTotalLookups;
	nIncrement = max(1, nIncrement);

	unsigned int crc_table[256];
	CalcCRCTable(crc_table);
	uint32 crcValue = 0xffffffff;
	unsigned int sumValue = 0;
	while (nIndex < nStreamLength) {
		sumValue += pStream[nIndex];
		crcValue = crc_table[(crcValue ^ pStream[nIndex]) & 0xff] ^ (crcValue >> 8);
		nIndex += nIncrement;
	}

	return ((__int64)crcValue << 32) + sumValue;
}

CString TryConvertFromUTF8(uint8* pComment, int nLengthInBytes) {
	wchar_t* pCommentUnicode = new wchar_t[nLengthInBytes + 1];
	char* pCommentBack = new char[nLengthInBytes + 1];
	CString result;
	int nCharsConverted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)pComment, nLengthInBytes, pCommentUnicode, nLengthInBytes);
	if (nCharsConverted > 0) {
		pCommentUnicode[nCharsConverted] = 0;
		if (::WideCharToMultiByte(CP_UTF8, 0, pCommentUnicode, -1, pCommentBack, nLengthInBytes + 1, NULL , NULL) > 0) {
			if (memcmp(pComment, pCommentBack, nLengthInBytes) == 0) {
				result = CString((LPCWSTR)pCommentUnicode);
			}
		}
	}
	delete[] pCommentUnicode;
	delete[] pCommentBack;
	return result;
}

CString GetJPEGComment(void* pJPEGStream, int nStreamLength) {
	uint8* pCommentSeg = (uint8*)FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE);
	if (pCommentSeg == NULL) {
		return CString("");
	}
	// The marker is FF FE; the length field at +2 counts itself and the payload.
	// Verify the declared payload stays inside the stream before reading it —
	// the old code trusted the length field and could read past the buffer.
	int nOffset = (int)(pCommentSeg - (uint8*)pJPEGStream);
	if (nOffset + 4 > nStreamLength) {
		return CString("");
	}
	int nBlockLength = pCommentSeg[2] * 256 + pCommentSeg[3];
	int nCommentLen = nBlockLength - 2;
	if (nCommentLen <= 0 || nOffset + 2 + nBlockLength > nStreamLength) {
		return CString("");
	}

	uint8* pComment = pCommentSeg + 4;
	if (nCommentLen > 2 && (nCommentLen & 1) == 0) {
		if (pComment[0] == 0xFF && pComment[1] == 0xFE) {
			// UTF16 little endian
			return CString((LPCWSTR)&(pComment[2]), (nCommentLen - 2) / 2);
		} else if (pComment[0] == 0xFE && pComment[1] == 0xFF) {
			// UTF16 big endian -> cannot read
			return CString("");
		}
	}

	CString sConvertedFromUTF8 = TryConvertFromUTF8(pComment, nCommentLen);
	if (!sConvertedFromUTF8.IsEmpty()) {
		return (sConvertedFromUTF8.Find(_T("Intel(R) JPEG Library")) != -1) ? CString("") : sConvertedFromUTF8;
	}

	// check if this is a reasonable string - it must contain enough characters between a and z and A and Z
	if (nCommentLen < 7) {
		return CString(""); // cannot check for such short strings, do not use
	}
	int nGoodChars = 0;
	for (int i = 0; i < nCommentLen; i++) {
		uint8 ch = pComment[i];
		if (ch >= 'a' && ch <= 'z' ||  ch >= 'A' && ch <= 'Z' || ch == ' ' || ch == ',' || ch == '.' || ch >= '0' && ch <= '9') {
			nGoodChars++;
		}
	}
	// The Intel lib puts this useless comment into each JPEG it writes - filter this out as nobody is interested in that...
	// The buffer is not NUL terminated, so search within nCommentLen instead of strstr.
	if (nCommentLen > 20) {
		static const char szIntel[] = "Intel(R) JPEG Library";
		const int nNeedleLen = (int)sizeof(szIntel) - 1;
		for (int i = 0; i + nNeedleLen <= nCommentLen; i++) {
			if (memcmp(pComment + i, szIntel, nNeedleLen) == 0) {
				return CString("");
			}
		}
	}

	return (nGoodChars > nCommentLen * 8 / 10) ? CString((LPCSTR)pComment, nCommentLen) : CString("");
}

void ClearJPEGComment(void* pJPEGStream, int nStreamLength) {
	uint8* pCommentSeg = (uint8*)FindJPEGMarker(pJPEGStream, nStreamLength, 0xFE);
	if (pCommentSeg == NULL) {
		return;
	}
	int nOffset = (int)(pCommentSeg - (uint8*)pJPEGStream);
	if (nOffset + 4 > nStreamLength) {
		return;
	}
	int nBlockLength = pCommentSeg[2] * 256 + pCommentSeg[3];
	int nCommentLen = nBlockLength - 2;
	if (nCommentLen <= 0) {
		return;
	}
	uint8* pComment = pCommentSeg + 4;
	if (pComment + nCommentLen <= (uint8*)pJPEGStream + nStreamLength) {
		memset(pComment, 0, nCommentLen);
	}
}

double GetExactTickCount() {
	static __int64 nFrequency = -1;
	if (nFrequency == -1) {
		if (!::QueryPerformanceFrequency((LARGE_INTEGER*)&nFrequency)) {
			nFrequency = 0;
		}
	}
	if (nFrequency == 0) {
		return ::GetTickCount();
	} else {
		__int64 nCounter;
		::QueryPerformanceCounter((LARGE_INTEGER*)&nCounter);
		return (1000*(double)nCounter)/nFrequency;
	}
}

// sRGB (8-bit, display-referred) -> linear light, gamma 2.2.
// Shared by the HDR writer (SaveImage.cpp) and the EXR writer
// (SaveEXR.cpp); see the declaration in Helpers.h.
float LinearFromSRGB(unsigned char v) {
	return (float)pow((double)v / 255.0, 2.2);
}

int GetWindowCaptionSize() {
	return ::GetSystemMetrics(SM_CYCAPTION);
}

CSize GetTotalBorderSize() {
	const int SM_CXP_ADDEDBORDER = 92;  // in MSDN this is SM_CXPADDEDBORDER, but for some reason it's not always available depending on configuration
	int nBorderWidth = (::GetSystemMetrics(SM_CXSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2;
	int nBorderHeight = (::GetSystemMetrics(SM_CYSIZEFRAME) + ::GetSystemMetrics(SM_CXP_ADDEDBORDER)) * 2 + ::GetSystemMetrics(SM_CYCAPTION);
	return CSize(nBorderWidth, nBorderHeight);
}

CRect GetWindowRectMatchingImageSize(HWND hWnd, CSize minSize, CSize maxSize, double& dZoom, CJPEGImage* pImage, bool bForceCenterWindow, bool bKeepAspectRatio, bool bWindowBorderless) {
	int nOrigWidth = (pImage == NULL) ? ::GetSystemMetrics(SM_CXSCREEN) / 2 : pImage->OrigWidth();
	int nOrigWidthUnzoomed = nOrigWidth;
	int nOrigHeight = (pImage == NULL) ? ::GetSystemMetrics(SM_CYSCREEN) / 2 : pImage->OrigHeight();
	if (dZoom == ZoomMax) {
		// NOTE: somewhat hacky, but the original code did not account for ZoomMax == DBL_MAX, causing overflows
		//       This workaround artificially calculates an image that is == the maximum allowed dimensions,
		//       so as to not overflow int or double
		//
		// when dZoom was unbounded to DBL_MAX in 1.1.44, doing any arithmetic would cause the nOrig* to overflow
		// So just set to scaled maximum allowed image dimension on one side
		// the math below will not overflow, and it stays within the bounds of an integer
		if (nOrigWidth == nOrigHeight) {
			// ratio is 1:1
			nOrigWidth = nOrigHeight = MAX_IMAGE_DIMENSION;
		} else if (nOrigWidth > nOrigHeight) {
			// wider than high
			nOrigHeight = (int)((double)nOrigHeight / nOrigWidth * MAX_IMAGE_DIMENSION);  // max * height/width ratio
			nOrigWidth = MAX_IMAGE_DIMENSION;
		} else {
			// higher than wide
			nOrigWidth = (int)((double)nOrigWidth / nOrigHeight * MAX_IMAGE_DIMENSION);  // max * width/height ratio
			nOrigHeight = MAX_IMAGE_DIMENSION;
		}
	} else if (dZoom > 0) {
		nOrigWidth = (int)(nOrigWidth * dZoom + 0.5);
		nOrigHeight = (int)(nOrigHeight * dZoom + 0.5);
	}

	CSize borderSize;
	if (!bWindowBorderless) {
		borderSize = GetTotalBorderSize();
	} else {
		borderSize = { 0, 0 };
	}

	int nRequiredWidth = borderSize.cx + nOrigWidth;
	int nRequiredHeight = borderSize.cy + nOrigHeight;
	CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
	if (bKeepAspectRatio && (nRequiredWidth > workingArea.Width() || nRequiredHeight > workingArea.Height())) {
		double dZoom;
		CSize imageRect = Helpers::GetImageRect(nOrigWidth, nOrigHeight, 
			min(maxSize.cx, workingArea.Width() - borderSize.cx),
			min(maxSize.cy, workingArea.Height() - borderSize.cy),
			false, false, false, dZoom);
		nRequiredWidth = imageRect.cx + borderSize.cx;
		nRequiredHeight = imageRect.cy + borderSize.cy;
	} else {
		nRequiredWidth = min(maxSize.cx, min(workingArea.Width(), nRequiredWidth));
		nRequiredHeight = min(maxSize.cy, min(workingArea.Height(), nRequiredHeight));
	}
	nRequiredWidth = max(minSize.cx, nRequiredWidth);
	nRequiredHeight = max(minSize.cy, nRequiredHeight);
	dZoom = ((double)(nRequiredWidth - borderSize.cx)) / nOrigWidthUnzoomed;
	CRect wndRect;
	::GetWindowRect(hWnd, &wndRect);
	int nNewLeft = (wndRect.left + wndRect.right - nRequiredWidth) / 2;
	int nNewTop = (wndRect.top + wndRect.bottom - nRequiredHeight) / 2;
	nNewLeft = max(workingArea.left, min(workingArea.right - nRequiredWidth, nNewLeft));
	nNewTop = max(workingArea.top, min(workingArea.bottom - nRequiredHeight, nNewTop));

	if (!bForceCenterWindow) {
		return CRect(CPoint(nNewLeft, nNewTop), CSize(nRequiredWidth, nRequiredHeight));
	} else {
		return CRect(CPoint(workingArea.left + (workingArea.Width() - nRequiredWidth) / 2, workingArea.top + (workingArea.Height() - nRequiredHeight) / 2), CSize(nRequiredWidth, nRequiredHeight));
	}
}

bool CanDisplayImageWithoutResize(HWND hWnd, CJPEGImage* pImage) {
	if (pImage == NULL) {
		return true;
	}
	CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
	CSize borderSize = GetTotalBorderSize();
	return pImage->OrigWidth() + borderSize.cx <= workingArea.Width() && pImage->OrigHeight() + borderSize.cy <= workingArea.Height();
}

CRect CalculateMaxIncludedRectKeepAR(const CTrapezoid& trapezoid, double dAspectRatio) {
	int w1 = trapezoid.x1e - trapezoid.x1s;
	int w2 = trapezoid.x2e - trapezoid.x2s;
	if ((trapezoid.x1s >= trapezoid.x2s && trapezoid.x1e <= trapezoid.x2e) || (trapezoid.x1s <= trapezoid.x2s && trapezoid.x1e >= trapezoid.x2e)) {
		int h = trapezoid.y2 - trapezoid.y1;
		double dTerm = (double)(w2 - w1)/h;
		bool bPyramid = w1 < w2;
		double dY =  bPyramid ? (dAspectRatio * h - w1)/(dAspectRatio + dTerm) : w1/(dAspectRatio - dTerm);
		double dAlpha = dY/h;
		double dX = (trapezoid.x2s - trapezoid.x1s)*dAlpha;
		dX += trapezoid.x1s;
		dY += trapezoid.y1;
		return CRect((int)(dX + 0.5), bPyramid ? (int)(dY + 0.5) : trapezoid.y1, 
			(int)(trapezoid.x1e + (trapezoid.x2e - trapezoid.x1e)*dAlpha + 0.5), bPyramid ? trapezoid.y2 : (int)(dY + 0.5));
	}
	if (trapezoid.x1s < trapezoid.x2s) {
		if (w1 < w2) 
			return CalculateMaxIncludedRectKeepAR(CTrapezoid(trapezoid.x2s, trapezoid.x1e, trapezoid.y1, trapezoid.x2s, trapezoid.x2e, trapezoid.y2), dAspectRatio);
		else
			return CalculateMaxIncludedRectKeepAR(CTrapezoid(trapezoid.x1s, trapezoid.x1e, trapezoid.y1, trapezoid.x2s, trapezoid.x1e, trapezoid.y2), dAspectRatio);
	} else {
		if (w1 < w2) 
			return CalculateMaxIncludedRectKeepAR(CTrapezoid(trapezoid.x1s, trapezoid.x2e, trapezoid.y1, trapezoid.x2s, trapezoid.x2e, trapezoid.y2), dAspectRatio);
		else
			return CalculateMaxIncludedRectKeepAR(CTrapezoid(trapezoid.x1s, trapezoid.x1e, trapezoid.y1, trapezoid.x1s, trapezoid.x2e, trapezoid.y2), dAspectRatio);
	}
}

CSize GetMaxClientSize(HWND hWnd) {
	CRect workingArea = CMultiMonitorSupport::GetWorkingRect(hWnd);
	CSize borderSize = GetTotalBorderSize();
	return CSize(workingArea.Width() - borderSize.cx, workingArea.Height() - borderSize.cy);
}

ETransitionEffect ConvertTransitionEffectFromString(LPCTSTR str) {
	if (_tcsicmp(str, _T("Blend")) == 0) {
		return TE_Blend;
	} else if (_tcsicmp(str, _T("SlideRL")) == 0) {
		return TE_SlideRL;
	} else if (_tcsicmp(str, _T("SlideLR")) == 0) {
		return TE_SlideLR;
	} else if (_tcsicmp(str, _T("SlideTB")) == 0) {
		return TE_SlideTB;
	} else if (_tcsicmp(str, _T("SlideBT")) == 0) {
		return TE_SlideBT;
	} else if (_tcsicmp(str, _T("RollRL")) == 0) {
		return TE_RollRL;
	} else if (_tcsicmp(str, _T("RollLR")) == 0) {
		return TE_RollLR;
	} else if (_tcsicmp(str, _T("RollTB")) == 0) {
		return TE_RollTB;
	} else if (_tcsicmp(str, _T("RollBT")) == 0) {
		return TE_RollBT;
	} else if (_tcsicmp(str, _T("ScrollRL")) == 0) {
		return TE_ScrollRL;
	} else if (_tcsicmp(str, _T("ScrollLR")) == 0) {
		return TE_ScrollLR;
	} else if (_tcsicmp(str, _T("ScrollTB")) == 0) {
		return TE_ScrollTB;
	} else if (_tcsicmp(str, _T("ScrollBT")) == 0) {
		return TE_ScrollBT;
	}
	return TE_None;
}

LPCTSTR ConvertTransitionEffectToString(ETransitionEffect effect) {
	switch (effect)
	{
	case Helpers::TE_None:
		return _T("None");
	case Helpers::TE_Blend:
		return _T("Blend");
	case Helpers::TE_SlideRL:
		return _T("SlideRL");
	case Helpers::TE_SlideLR:
		return _T("SlideLR");
	case Helpers::TE_SlideTB:
		return _T("SlideTB");
	case Helpers::TE_SlideBT:
		return _T("SlideBT");
	case Helpers::TE_RollRL:
		return _T("RollRL");
	case Helpers::TE_RollLR:
		return _T("RollLR");
	case Helpers::TE_RollTB:
		return _T("RollTB");
	case Helpers::TE_RollBT:
		return _T("RollBT");
	case Helpers::TE_ScrollRL:
		return _T("ScrollRL");
	case Helpers::TE_ScrollLR:
		return _T("ScrollLR");
	case Helpers::TE_ScrollTB:
		return _T("ScrollTB");
	case Helpers::TE_ScrollBT:
		return _T("ScrollBT");
	default:
		return _T("None");
	}
}

bool IsInFileEndingList(LPCTSTR sFileEndings, LPCTSTR sEnding) {
	// F-12: parse the ';'-separated list in place instead of copying
	// it into a fixed 256-char buffer, which silently truncated
	// longer lists (user-extended FileEndingsRAW / FilesProcessedByWIC
	// or repeated AddTemporaryRAWFileEnding) and dropped the extensions
	// past the cut. The comparison is unchanged: list tokens are
	// "*.ext" and sEnding points just past the '.' in the file name,
	// so sStart+1 (".ext") is compared against sEnding-1 (".ext").
	LPCTSTR sStart = sFileEndings;
	while (*sStart != 0) {
		LPCTSTR sCurrent = sStart;
		while (*sCurrent != 0 && *sCurrent != _T(';')) {
			sCurrent++;
		}
		// Token is [sStart, sCurrent); its ".ext" form has length
		// (sCurrent - sStart) - 1 and must match sEnding-1 exactly.
		size_t nTokenLen = (size_t)(sCurrent - sStart);
		if (nTokenLen >= 1 && _tcslen(sEnding - 1) == nTokenLen - 1 &&
			_tcsnicmp(sStart + 1, sEnding - 1, nTokenLen - 1) == 0) {
			return true;
		}
		if (*sCurrent == _T(';')) {
			sCurrent++;
		}
		sStart = sCurrent;
	}
	return false;
}

EImageFormat GetImageFormat(LPCTSTR sFileName) {
	// Look for the dot only in the file name, not the whole path. _tcsrchr over
	// the full path took the last dot anywhere, so a file without an extension in
	// a dotted directory (C:\my.dir\photo) was matched against "dir\photo".
	LPCTSTR sBase = _tcsrchr(sFileName, _T('\\'));
	LPCTSTR sBaseAlt = _tcsrchr(sFileName, _T('/'));
	if (sBaseAlt != NULL && (sBase == NULL || sBaseAlt > sBase)) sBase = sBaseAlt;
	sBase = (sBase == NULL) ? sFileName : (sBase + 1);
	LPCTSTR sEnding = _tcsrchr(sBase, _T('.'));
	if (sEnding != NULL) {
		sEnding += 1;
		if (_tcsicmp(sEnding, _T("JPG")) == 0 || _tcsicmp(sEnding, _T("JPEG")) == 0 || _tcsicmp(sEnding, _T("JPE")) == 0 || _tcsicmp(sEnding, _T("JFIF")) == 0 || _tcsicmp(sEnding, _T("JPS")) == 0 || _tcsicmp(sEnding, _T("JFI")) == 0 || _tcsicmp(sEnding, _T("JIF")) == 0 || _tcsicmp(sEnding, _T("MPO")) == 0) {
			return IF_JPEG;
		} else if (_tcsicmp(sEnding, _T("BMP")) == 0) {
			return IF_WindowsBMP;
		} else if (_tcsicmp(sEnding, _T("PNG")) == 0 || _tcsicmp(sEnding, _T("APNG")) == 0) {
			return IF_PNG;
		} else if (_tcsicmp(sEnding, _T("ICO")) == 0) {
			// No dedicated decoder case in ImageLoadThread (falls through
			// to GDI+ like IF_Unknown); the branch exists so a .ico file
			// routes by extension even when its magic cannot be read,
			// matching the .cur branch below.
			return IF_ICO;
		} else if (_tcsicmp(sEnding, _T("GIF")) == 0) {
			// No dedicated decoder case in ImageLoadThread (falls through
			// to GDI+ like IF_Unknown); the branch exists so SaveImage
			// can route .gif to the GDI+ GIF encoder.
			return IF_GIF;
		} else if (_tcsicmp(sEnding, _T("TIF")) == 0 || _tcsicmp(sEnding, _T("TIFF")) == 0) {
			return IF_TIFF;
		} else if (_tcsicmp(sEnding, _T("WEBP")) == 0) {
			return IF_WEBP;
		} else if (_tcsicmp(sEnding, _T("JXL")) == 0) {
			return IF_JXL;
		} else if (_tcsicmp(sEnding, _T("AVIF")) == 0 || _tcsicmp(sEnding, _T("AVIFS")) == 0 || _tcsicmp(sEnding, _T("AVIS")) == 0) {
			return IF_AVIF;
		} else if (_tcsicmp(sEnding, _T("HEIF")) == 0 || _tcsicmp(sEnding, _T("HEIC")) == 0 || _tcsicmp(sEnding, _T("HIF")) == 0) {
			return IF_HEIF;
		} else if (_tcsicmp(sEnding, _T("TGA")) == 0 || _tcsicmp(sEnding, _T("ICB")) == 0 || _tcsicmp(sEnding, _T("VDA")) == 0 || _tcsicmp(sEnding, _T("VST")) == 0) {
			return IF_TGA;
		} else if (_tcsicmp(sEnding, _T("QOI")) == 0) {
			return IF_QOI;
		} else if (_tcsicmp(sEnding, _T("PPM")) == 0 || _tcsicmp(sEnding, _T("PGM")) == 0 || _tcsicmp(sEnding, _T("PNM")) == 0) {
			return IF_PPM;
		} else if (_tcsicmp(sEnding, _T("HDR")) == 0 || _tcsicmp(sEnding, _T("RGBE")) == 0) {
			return IF_HDR;
		} else if (_tcsicmp(sEnding, _T("PBM")) == 0) {
			return IF_PBM;
		} else if (_tcsicmp(sEnding, _T("PAM")) == 0) {
			return IF_PAM;
		} else if (_tcsicmp(sEnding, _T("CUR")) == 0) {
			// F-10: a cursor file decodes through GDI+ like an icon: there
			// is no IF_ICO case in the load dispatch, it falls through to
			// the GDI+ default. (.ico itself reaches IF_ICO through the
			// 00 00 01 00 sniffer branch, not through this function; the
			// 00 00 02 00 sniffer branch routes real .cur files the same
			// way, this extension branch covers files whose header could
			// not be read.)
			return IF_ICO;
		} else if (_tcsicmp(sEnding, _T("JP2")) == 0 || _tcsicmp(sEnding, _T("J2K")) == 0 || _tcsicmp(sEnding, _T("J2C")) == 0) {
			return IF_JP2;
		} else if (_tcsicmp(sEnding, _T("EXR")) == 0) {
			return IF_EXR;
		} else if (_tcsicmp(sEnding, _T("SVG")) == 0) {
			return IF_SVG;
		} else if (_tcsicmp(sEnding, _T("PSD")) == 0 || _tcsicmp(sEnding, _T("PSB")) == 0) {
			return IF_PSD;
		} else if (IsInFileEndingList(CSettingsProvider::This().FilesProcessedByWIC(), sEnding)) {
			return IF_WIC;
		} else if (IsInFileEndingList(CSettingsProvider::This().FileEndingsRAW(), sEnding)) {
			return IF_CameraRAW;
		} else if (IsInFileEndingList(CSettingsProvider::This().FileEndingsExtra(), sEnding)) {
			// F-11: an Extra-listed extension no decoder claims by extension.
			// Formats with a magic signature already open through the magic
			// sniff in ImageLoadThread (which runs before this fallback), so
			// this only covers the no-magic remainder: give WIC one attempt
			// with the machine's installed codecs, and fail cleanly as before
			// when there is none. The shipped ini already warns that Extra is
			// browsing-only for anything no decoder understands (fa00eb3).
			return IF_WIC;
		}
	}
	return IF_Unknown;
}

// Returns the short form of the path (including the file name)
CString GetShortFilePath(LPCTSTR sPath) {
	TCHAR shortPath[MAX_PATH];
	if (::GetShortPathName(sPath, (LPTSTR)(&shortPath), MAX_PATH) != 0) {
		return CString(shortPath);
	} else {
		int nError = ::GetLastError();
		return CString(sPath);
	}
}

// Returns the short form of the path but the file name remains untouched
CString ReplacePathByShortForm(LPCTSTR sPath) {
	LPCTSTR sLast = _tcsrchr(sPath, _T('\\'));
	if (sLast != NULL) {
		CString sShortPath = GetShortFilePath(CString(sPath).Left((int)(sLast - sPath)));
		return sShortPath + sLast;
	}
	return CString(sPath);
}

// strstr() ignoring case
LPTSTR stristr(LPCTSTR szStringToBeSearched, LPCTSTR szSubstringToSearchFor) {
	LPCTSTR pPos = NULL;
	LPTSTR szCopy1 = NULL;
	LPTSTR szCopy2 = NULL;
	 
	// verify parameters
	if ( szStringToBeSearched == NULL || szSubstringToSearchFor == NULL ) {
		return (LPTSTR)szStringToBeSearched;
	}
	 
	// empty substring - return input (consistent with strstr)
	if ( _tcslen(szSubstringToSearchFor) == 0 ) {
		return (LPTSTR)szStringToBeSearched;
	}
	 
	szCopy1 = _tcsdup(szStringToBeSearched);
	szCopy2 = _tcsdup(szSubstringToSearchFor);
	 
	if ( szCopy1 == NULL || szCopy2 == NULL ) {
		// another option is to raise an exception here
		free((void*)szCopy1);
		free((void*)szCopy2);
		return NULL;
	}
	// Lower-case only after the NULL check: _tcslwr dereferences the block.
	_tcslwr(szCopy1);
	_tcslwr(szCopy2);
	 
	pPos = _tcsstr(szCopy1, szCopy2);
	 
	if ( pPos != NULL ) {
		// map to the original string
		pPos = szStringToBeSearched + (pPos - szCopy1);
	}
	 
	free((void*)szCopy1);
	free((void*)szCopy2);
	 
	return (LPTSTR)pPos;
 } // stristr(...)

__int64 GetFileSize(LPCTSTR sPath) {
	HANDLE hFile = ::CreateFile(sPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return 0;
	}
	__int64 fileSize = 0;
	::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
	::CloseHandle(hFile);
	return fileSize;
}

__int64 GetFileSize(HANDLE hFile) {
	__int64 fileSize = 0;
	::GetFileSizeEx(hFile, (PLARGE_INTEGER)&fileSize);
	return fileSize;
}

// Gets the frame index of the next frame, depending on the index of the last image (relevant if the image is a multiframe image)
int GetFrameIndex(CJPEGImage* pImage, bool bNext, bool bPlayAnimation, bool & switchImage) {
	if (pImage == NULL) {
		switchImage = !bPlayAnimation;
		return 0;
	}
	bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
	bool isAnimation = pImage != NULL && pImage->IsAnimation();
	int nFrameIndex = 0;
	switchImage = true;
	if (isMultiFrame) {
		switchImage = false;
		nFrameIndex = pImage->FrameIndex() + (bNext ? 1 : -1);
		if (isAnimation) {
			if (bPlayAnimation) {
				if (nFrameIndex < 0) {
					nFrameIndex = pImage->NumberOfFrames() - 1;
				} else if (nFrameIndex > pImage->NumberOfFrames() - 1) {
					nFrameIndex = 0;
				}
			} else {
				switchImage = true;
				nFrameIndex = 0;
			}
		} else {
			if (nFrameIndex < 0 || nFrameIndex >= pImage->NumberOfFrames()) {
				nFrameIndex = 0;
				switchImage = true;
			}
		}
	}
	if (bPlayAnimation && pImage == NULL) {
		switchImage = false; // never switch image when error during animation playing
	}

	return nFrameIndex;
}

// Gets an index string of the form [a/b] for multiframe images, empty string for single frame images
CString GetMultiframeIndex(CJPEGImage* pImage) {
	bool isMultiFrame = pImage != NULL && pImage->NumberOfFrames() > 1;
	if (isMultiFrame && !pImage->IsAnimation()) {
		CString s;
		s.Format(_T(" [%d/%d]"), pImage->FrameIndex() + 1, pImage->NumberOfFrames());
		return s;
	}
	return CString(_T(""));
}

CString GetFileInfoString(LPCTSTR sFormat, CJPEGImage* pImage, CFileList* pFilelist, double dZoom) {
	if (pImage == NULL) {
		return CString(_T(""));
	}
	bool isClipboardImage = pImage->IsClipboardImage();
	if (_tcscmp(sFormat, _T("<i>  <p>")) == 0) {
		if (isClipboardImage) return CString(CNLS::GetString(_T("Clipboard Image")));
		CString sFileInfo;
		sFileInfo.Format(_T("[%d/%d]  %s"), pFilelist->CurrentIndex() + 1, pFilelist->Size(), pFilelist->Current() + GetMultiframeIndex(pImage));
		return sFileInfo;
	}
	
	CString sFileInfo(sFormat);
	sFileInfo.Replace(_T("\\t"), _T("        "));
	if (_tcsstr(sFormat, _T("<f>")) != NULL) {
		CString sFileName = isClipboardImage ? CNLS::GetString(_T("Clipboard Image")) : pFilelist->CurrentFileTitle() + GetMultiframeIndex(pImage);
		sFileInfo.Replace(_T("<f>"), sFileName);
	}
	if (_tcsstr(sFormat, _T("<p>")) != NULL) {
		CString sFilePath = isClipboardImage ? CNLS::GetString(_T("Clipboard Image")) : pFilelist->Current() + GetMultiframeIndex(pImage);
		sFileInfo.Replace(_T("<p>"), sFilePath);
	}
	if (_tcsstr(sFormat, _T("<i>")) != NULL) {
		CString sIndex;
		if (!isClipboardImage) sIndex.Format(_T("[%d/%d]"), pFilelist->CurrentIndex() + 1, pFilelist->Size());
		sFileInfo.Replace(_T("<i>"), sIndex);
	}
	if (_tcsstr(sFormat, _T("<z>")) != NULL) {
		TCHAR buff[32];
		_stprintf_s(buff, 32, _T("%d %%"), int(dZoom*100 + 0.5));
		sFileInfo.Replace(_T("<z>"), buff);
	}
	if (_tcsstr(sFormat, _T("<s>")) != NULL) {
		TCHAR buff[48];
		_stprintf_s(buff, 48, _T("%d x %d"), pImage->OrigWidth(), pImage->OrigHeight());
		sFileInfo.Replace(_T("<s>"), buff);
	}
	if (_tcsstr(sFormat, _T("<l>")) != NULL) {
		__int64 fileSize = isClipboardImage ? 0 : GetFileSize(pFilelist->Current());
		CString sFileSize;
		if (fileSize >= 1024 * 1024) {
			sFileSize.Format(_T("%d MB"), (int)(fileSize >> 20));
		} else if (fileSize >= 1024) {
			sFileSize.Format(_T("%d KB"), (int)(fileSize >> 10));
		} else if (fileSize > 0) {
			sFileSize.Format(_T("%d b"), (int)fileSize);
		}
		sFileInfo.Replace(_T("<l>"), sFileSize);
	}
	sFileInfo.TrimLeft();
	sFileInfo.TrimRight();
	return sFileInfo;
}

int GetWindowsVersion() {
#pragma warning(push)
#pragma warning(disable:4996)
	OSVERSIONINFO osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFO));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&osvi);
#pragma warning(pop)
	return osvi.dwMajorVersion * 100 + osvi.dwMinorVersion;
}


// D2/B4 (L-U03, F-M09): see Helpers.h. Empty, too long, control characters,
// the reserved characters, a trailing dot/space and the reserved device names
// (CON, PRN, AUX, NUL, COM1-9, LPT1-9) are rejected; everything else is left
// to the file system.
bool IsValidFileNameComponent(LPCTSTR sName) {
	if (sName == NULL) return false;
	size_t nLen = _tcslen(sName);
	if (nLen == 0 || nLen > 255) return false;
	if (sName[nLen - 1] == _T('.') || sName[nLen - 1] == _T(' ')) return false;
	for (size_t i = 0; i < nLen; i++) {
		TCHAR c = sName[i];
		if (c < 32) return false;
		if (_tcschr(_T("\\/:*?\"<>|"), c) != NULL) return false;
	}
	TCHAR sBase[16];
	size_t nBase = 0;
	while (nBase < nLen && nBase < 15 && sName[nBase] != _T('.')) {
		sBase[nBase] = sName[nBase];
		nBase++;
	}
	sBase[nBase] = 0;
	CString sUpper(sBase);
	sUpper.MakeUpper();
	if (sUpper == _T("CON") || sUpper == _T("PRN") || sUpper == _T("AUX") || sUpper == _T("NUL")) {
		return false;
	}
	if (sUpper.GetLength() == 4 && sUpper[3] >= _T('1') && sUpper[3] <= _T('9') &&
		(sUpper.Left(3) == _T("COM") || sUpper.Left(3) == _T("LPT"))) {
		return false;
	}
	return true;
}
}