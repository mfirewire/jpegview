
#include "StdAfx.h"
#include "ImageLoadThread.h"
#include <gdiplus.h>
#include <vector>
#include <set>
#include "JPEGImage.h"
#include "MessageDef.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "BasicProcessing.h"
#include "dcraw_mod.h"
#include "TJPEGWrapper.h"
#include "libjpeg-turbo\include\turbojpeg.h"
#include "ICCProfileTransform.h"
#include "PNGWrapper.h"
#ifndef WINXP
#include "JXLWrapper.h"
#include "HEIFWrapper.h"
#include "AVIFWrapper.h"
#include "RAWWrapper.h"
#endif
#include "WEBPWrapper.h"
#include "QOIWrapper.h"
#include "PPMWrapper.h"
#include "HDRWrapper.h"
#include "PBMWrapper.h"
#include "PAMWrapper.h"
#include "JP2Wrapper.h"
#include "EXRWrapper.h"
#include "SVGWrapper.h"
#include "PSDWrapper.h"
#include "MaxImageDef.h"


using namespace Gdiplus;

namespace {
// SetErrorMode is process-global. The decode paths that call it have early
// returns for oversized files and failed allocations; with a bare
// SetErrorMode/restore pair those returns skipped the restore and left
// SEM_FAILCRITICALERRORS in effect for the rest of the process.
class CErrorModeGuard {
public:
	CErrorModeGuard() : m_nPrev(::SetErrorMode(SEM_FAILCRITICALERRORS)) {}
	~CErrorModeGuard() { ::SetErrorMode(m_nPrev); }
private:
	UINT m_nPrev;
	CErrorModeGuard(const CErrorModeGuard&);
	CErrorModeGuard& operator=(const CErrorModeGuard&);
};
}

/////////////////////////////////////////////////////////////////////////////////////////////
// E6/E12: one memo for bad-file and TIFF-probe conclusions, shared by both
// load threads. A RAW/TIFF that fails decoding is decoded 3-5 times before it
// finally fails (LibRaw -> dcraw_mod -> thumbnail; TIFF probe + cleanup +
// decode), and again on every revisit. The memo records the conclusion once
// and later loads skip straight to the failure. It is keyed by path + file
// size + last-write time, so an externally edited file (same name, different
// content) is never served a stale verdict, and releasing a file for reload
// (CImageLoadThread::ReleaseFile) drops its entries. OOM is never memoized:
// it is transient, and a retry after freeing memory must re-decode.
// R-16/E19: bBad is recorded only for a genuine decode failure. Four
// failures are NOT decode failures and are never recorded, so they can
// never poison the memo: (1) the image decoded fine but ProcessImageAfterLoad
// failed (VerifyRotation failure, DIB size-limit trip - both depend on the
// processing parameters / MaxImagePixelsMP, not on the file alone), (2) a
// size-limit rejection anywhere (it depends on the MaxImagePixelsMP setting,
// so raising the cap must retry the file), (3) a failure to even open the
// file at record time (locked by another program, antivirus scan, network
// hiccup - the file is re-checked with GetFileIdentity before recording),
// (4) a TIFF gate rejection (P0: the gate runs before any decoder, so a
// retry re-runs only the cheap header parse, while memoizing a false
// rejection would lock the file out for the whole session).
// No settings-generation stamp is kept: with (1) and (2) excluded at the
// source, no settings-dependent verdict ever enters the memo, so there is
// nothing for a stamp to invalidate; the probe conclusions are file-content
// only and are unaffected by settings. Residual: FAT/exFAT stores mtime at
// 2 s granularity, so a same-size rewrite within 2 s of a genuine decode
// failure can still hit the stale verdict (NTFS mtime is 100 ns and is
// unaffected); ReleaseFile still clears it.
/////////////////////////////////////////////////////////////////////////////////////////////
struct LoadMemoEntry {
	CString sPath;
	__int64 nSize;
	FILETIME ftWrite;
	bool bBad; // a previous load failed with a decode error (not OOM)
	bool bProbeKnown; // TIFF 318/319 probe conclusion is known
	bool bProbeNeedsSanitize; // probe said the TIFF needs 318/319 sanitizing
};
static const int kLoadMemoMaxEntries = 32;
static std::vector<LoadMemoEntry> s_loadMemo;
struct CLoadMemoLockHolder {
	CRITICAL_SECTION cs;
	CLoadMemoLockHolder() { ::InitializeCriticalSection(&cs); }
	~CLoadMemoLockHolder() { ::DeleteCriticalSection(&cs); }
};
static CLoadMemoLockHolder& LoadMemoLock() { static CLoadMemoLockHolder lock; return lock; }

// File identity for the memo: size + last-write time from one open.
static bool GetFileIdentity(LPCTSTR sFileName, __int64& nSize, FILETIME& ftWrite) {
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER liSize;
	bool bOk = ::GetFileSizeEx(hFile, &liSize) && ::GetFileTime(hFile, NULL, NULL, &ftWrite);
	if (bOk) {
		nSize = liSize.QuadPart;
	}
	::CloseHandle(hFile);
	return bOk;
}

static bool LoadMemoLookup(LPCTSTR sFileName, __int64 nSize, const FILETIME& ftWrite, LoadMemoEntry& entryOut) {
	Helpers::CAutoCriticalSection lock(LoadMemoLock().cs);
	for (size_t i = 0; i < s_loadMemo.size(); i++) {
		const LoadMemoEntry& e = s_loadMemo[i];
		if (e.nSize == nSize && e.ftWrite.dwLowDateTime == ftWrite.dwLowDateTime &&
			e.ftWrite.dwHighDateTime == ftWrite.dwHighDateTime && e.sPath.CompareNoCase(sFileName) == 0) {
			entryOut = e;
			return true;
		}
	}
	return false;
}

static void LoadMemoRecord(LPCTSTR sFileName, __int64 nSize, const FILETIME& ftWrite, bool bBad, bool bProbeKnown, bool bProbeNeedsSanitize) {
	Helpers::CAutoCriticalSection lock(LoadMemoLock().cs);
	for (size_t i = 0; i < s_loadMemo.size(); i++) {
		LoadMemoEntry& e = s_loadMemo[i];
		if (e.nSize == nSize && e.ftWrite.dwLowDateTime == ftWrite.dwLowDateTime &&
			e.ftWrite.dwHighDateTime == ftWrite.dwHighDateTime && e.sPath.CompareNoCase(sFileName) == 0) {
			if (bBad) e.bBad = true;
			if (bProbeKnown) { e.bProbeKnown = true; e.bProbeNeedsSanitize = bProbeNeedsSanitize; }
			return;
		}
	}
	LoadMemoEntry e;
	e.sPath = sFileName;
	e.nSize = nSize;
	e.ftWrite = ftWrite;
	e.bBad = bBad;
	e.bProbeKnown = bProbeKnown;
	e.bProbeNeedsSanitize = bProbeNeedsSanitize;
	s_loadMemo.push_back(e);
	while (s_loadMemo.size() > (size_t)kLoadMemoMaxEntries) {
		s_loadMemo.erase(s_loadMemo.begin());
	}
}

void CImageLoadThread::InvalidateLoadMemo(LPCTSTR sFileName) {
	Helpers::CAutoCriticalSection lock(LoadMemoLock().cs);
	for (size_t i = 0; i < s_loadMemo.size();) {
		if (s_loadMemo[i].sPath.CompareNoCase(sFileName) == 0) {
			s_loadMemo.erase(s_loadMemo.begin() + (std::vector<LoadMemoEntry>::difference_type)i);
		} else {
			i++;
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// F-14/E13: blend an image's alpha channel onto the background colour, in place.
// Replaces the six per-format loops that used an int counter over nWidth*nHeight
// (F-14: the int product overflows for very large images; the counter is size_t
// here). E13: two optimizations, both pixel-exact. First, if every pixel is
// opaque the blend is the identity (AlphaBlendBackground returns the pixel
// unchanged for alpha 0xFF), so the blend pass leaves those pixels untouched.
// Second, the blend itself uses lookup tables instead of the double math in
// Helpers::AlphaBlendBackground: out = (v*a + bg*(255-a) + 127) / 255, which a
// brute-force check over all 256^3 (value, alpha, background) combinations
// proved identical to the (x / 255.0) + 0.5 formula. Fully transparent pixels
// (alpha 0) become the background colour with alpha 0, exactly as the original did.
// R-17/E20: the tables are no longer allocated per call (192 KB + 196,608
// table writes on every call, even for a 2x2 frame with a single transparent
// pixel). They are a thread_local static - one per load thread, rebuilt only
// when the background colour changes - and they are built lazily, on the first
// transparent pixel, so a fully opaque image never builds them at all. Small
// images (< 4096 pixels) skip the tables entirely and use the scalar
// Helpers::AlphaBlendBackground loop: building 196,608 table entries to blend
// a few thousand pixels is pure overhead. E20: the old separate "is anything
// transparent?" pre-scan is gone - the single pass below blends each pixel as
// it meets it, so a transparent frame is scanned once instead of twice. All
// callers run on the load threads, so thread_local is safe here (this function
// is only called from the ProcessRead* functions); the tables are plain bytes
// with no destructor, so thread exit needs no cleanup.
/////////////////////////////////////////////////////////////////////////////////////////////
static void AlphaBlendImageToBackground(uint32* pPixels, size_t nPixels, COLORREF bkColor) {
	if (pPixels == NULL || nPixels == 0) {
		return;
	}
	const uint8 bgR = GetRValue(bkColor);
	const uint8 bgG = GetGValue(bkColor);
	const uint8 bgB = GetBValue(bkColor);
	const uint32 bgRGB = ((uint32)bgR << 16) | ((uint32)bgG << 8) | (uint32)bgB;
	// R-17: one LUT set per load thread, rebuilt only when the background
	// colour changes. thread_local, not a process static: the two load threads
	// must never share (see R-01).
	thread_local static uint8 s_tables[3 * 256 * 256];
	thread_local static COLORREF s_tablesBkColor = 0;
	thread_local static bool s_tablesValid = false;
	bool bTablesReady = false;
	if (nPixels >= 4096) {
		// Large image: the tables pay for themselves, but only build them
		// when the first transparent pixel actually needs them.
		if (s_tablesValid && s_tablesBkColor == bkColor) {
			bTablesReady = true;
		}
	}
	uint8* pTableR = s_tables;
	uint8* pTableG = s_tables + 256 * 256;
	uint8* pTableB = s_tables + 2 * 256 * 256;
	for (size_t i = 0; i < nPixels; i++) {
		uint32 pixel = pPixels[i];
		uint32 alpha = pixel & 0xFF000000;
		if (alpha == 0xFF000000) {
			continue; // opaque: blending would not change this pixel
		}
		if (alpha == 0) {
			pPixels[i] = bgRGB; // fully transparent: background, alpha 0
			continue;
		}
		if (nPixels < 4096) {
			// Small image: the scalar helper is cheaper than building the
			// tables, and pixel-identical to them (see the comment above).
			pPixels[i] = Helpers::AlphaBlendBackground(pixel, bkColor);
			continue;
		}
		if (!bTablesReady) {
			for (unsigned int a = 0; a < 256; a++) {
				for (unsigned int v = 0; v < 256; v++) {
					pTableR[a * 256 + v] = (uint8)((v * a + bgR * (255 - a) + 127) / 255);
					pTableG[a * 256 + v] = (uint8)((v * a + bgG * (255 - a) + 127) / 255);
					pTableB[a * 256 + v] = (uint8)((v * a + bgB * (255 - a) + 127) / 255);
				}
			}
			s_tablesBkColor = bkColor;
			s_tablesValid = true;
			bTablesReady = true;
		}
		unsigned int a = alpha >> 24;
		pPixels[i] = 0xFF000000 |
			((uint32)pTableR[a * 256 + ((pixel >> 16) & 0xFF)] << 16) |
			((uint32)pTableG[a * 256 + ((pixel >> 8) & 0xFF)] << 8) |
			(uint32)pTableB[a * 256 + (pixel & 0xFF)];
	}
}

// static initializers
volatile int CImageLoadThread::m_curHandle = 0;

/////////////////////////////////////////////////////////////////////////////////////////////
// static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

// find image format of this image by reading some header bytes
static EImageFormat GetImageFormat(LPCTSTR sFileName) {
	FILE *fptr;
	if ((fptr = _tfopen(sFileName, _T("rb"))) == NULL) {
		return IF_Unknown;
	}
	// Zero-initialised: a file shorter than 48 bytes leaves the tail as zero
	// instead of uninitialised stack, and the format checks below then compare
	// against fixed bytes rather than garbage. 48 bytes so the EMF dSignature
	// at offset 40 can be checked (F-10).
	unsigned char header[48] = { 0 };
	int nSize = (int)fread((void*)header, 1, 48, fptr);
	fclose(fptr);
	if (nSize < 2) {
		return IF_Unknown;
	}

	if (header[0] == 0x42 && header[1] == 0x4d) {
		return IF_WindowsBMP;
	} else if (header[0] == 0xff && header[1] == 0xd8) {
		return IF_JPEG;
	} else if (header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' &&
		header[4] == 0x0d && header[5] == 0x0a && header[6] == 0x1a && header[7] == 0x0a) {
		return IF_PNG;
	} else if (header[0] == 'G' && header[1] == 'I' && header[2] == 'F' && header[3] == '8' &&
		(header[4] == '7' || header[4] == '9') && header[5] == 'a') {
		return IF_GIF;
	} else if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
		header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P') {
		return IF_WEBP;
	} else if ((header[0] == 0xff && header[1] == 0x0a) ||
		memcmp(header, "\x00\x00\x00\x0cJXL\x20\x0d\x0a\x87\x0a", 12) == 0) {
		return IF_JXL;
	} else if (!memcmp(header+4, "ftyp", 4)) {
		// https://github.com/strukturag/libheif/issues/83
		// https://github.com/strukturag/libheif/blob/ce1e4586b6222588c5afcd60c7ba9caa86bcc58c/libheif/heif.h#L602-L805

		// AV1: avif, avis
		if (!memcmp(header+8, "avi", 3))
			return IF_AVIF;
		// H265: heic, heix, hevc, hevx, heim, heis, hevm, hevs
		if (!memcmp(header+8, "hei", 3) || !memcmp(header+8, "hev", 3))
			return IF_HEIF;
		// Canon CR3
		if (!memcmp(header+8, "crx ", 4))
			return IF_CameraRAW;
	} else if (header[0] == 'q' && header[1] == 'o' && header[2] == 'i' && header[3] == 'f') {
		return IF_QOI;
	} else if (header[0] == '8' && header[1] == 'B' && header[2] == 'P' && header[3] == 'S') {
		return IF_PSD;
	} else if (header[0] == 0xd7 && header[1] == 0xcd && header[2] == 0xc6 && header[3] == 0x9a) {
		return IF_WMF;
	} else if (nSize >= 12 && header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x00 && header[3] == 0x0c &&
		header[4] == 0x6a && header[5] == 0x50 && header[6] == 0x20 && header[7] == 0x20) {
		return IF_JP2;
	} else if (header[0] == 0xff && header[1] == 0x4f && header[2] == 0xff && header[3] == 0x51) {
		return IF_JP2;
	} else if (header[0] == 0x76 && header[1] == 0x2f && header[2] == 0x31 && header[3] == 0x01) {
		return IF_EXR;
	} else if (header[0] == 'P' && (header[1] == '5' || header[1] == '6')) {
		return IF_PPM;
	} else if (header[0] == 'P' && header[1] == '4') {
		return IF_PBM;
	} else if (header[0] == 'P' && header[1] == '7') {
		return IF_PAM;
	} else if (!memcmp(header, "DDS ", 4)) {
		return IF_WIC;
	}

	// default fallback if no matches based on magic bytes
	EImageFormat eImageFormat = Helpers::GetImageFormat(sFileName);

	if (eImageFormat != IF_Unknown) {
		return eImageFormat;
	} else if (!memcmp(header+4, "ftyp", 4)) {
		// Unspecified encoding (possibly AVIF or HEIF): mif1, mif2, msf1, miaf, 1pic
		return IF_AVIF;
	} else if (!memcmp(header, "II*\0", 4) || !memcmp(header, "MM\0*", 4)) {
		// Must be checked after file extension to avoid classifying RAW as TIFF
		// A few RAW image formats use TIFF as the container
		// ex: CR2 - http://lclevy.free.fr/cr2/#key_info
		// ex: DNG - https://www.adobe.com/creativecloud/file-types/image/raw/dng-file.html#dng
		return IF_TIFF;
	} else if (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x01 && header[3] == 0x00) {
		return IF_ICO;
	} else if (header[0] == 0x00 && header[1] == 0x00 && header[2] == 0x02 && header[3] == 0x00) {
		// F-10: a real cursor file (type 2). Like .ico it routes to IF_ICO
		// and falls through to the GDI+ default path; the two .cur samples
		// on hand are ICO-cored (type 1), so a true CUR file still needs a
		// device sample to verify.
		return IF_ICO;
	} else if (header[0] == 0x01 && header[1] == 0x00 && header[2] == 0x00 && header[3] == 0x00 &&
		// F-10: the bare 01 00 00 00 matches any file starting with those bytes;
		// also require the EMF dSignature 0x464D4520 (" EMF") at offset 40.
		nSize >= 44 && header[40] == 0x20 && header[41] == 'E' && header[42] == 'M' && header[43] == 'F') {
		return IF_EMF;
	}
	return IF_Unknown;
}

static EImageFormat GetBitmapFormat(Gdiplus::Bitmap * pBitmap) {
	GUID guid{ 0 };
	pBitmap->GetRawFormat(&guid);
	if (guid == Gdiplus::ImageFormatBMP) {
		return IF_WindowsBMP;
	} else if (guid == Gdiplus::ImageFormatPNG) {
		return IF_PNG;
	} else if (guid == Gdiplus::ImageFormatGIF) {
		return IF_GIF;
	} else if (guid == Gdiplus::ImageFormatTIFF) {
		return IF_TIFF;
	} else if (guid == Gdiplus::ImageFormatJPEG || guid == Gdiplus::ImageFormatEXIF) {
		return IF_JPEG;
	} else {
		return IF_Unknown;
	}
}

static CJPEGImage* ConvertGDIPlusBitmapToJPEGImage(Gdiplus::Bitmap* pBitmap, int nFrameIndex, void* pEXIFData, 
	__int64 nJPEGHash, bool &isOutOfMemory, bool &isAnimatedGIF, bool &isSizeLimitExceeded) {

	isOutOfMemory = false;
	isAnimatedGIF = false;
	isSizeLimitExceeded = false;
	Gdiplus::Status lastStatus = pBitmap->GetLastStatus();
	if (lastStatus != Gdiplus::Ok) {
		isOutOfMemory = lastStatus == Gdiplus::OutOfMemory;
		return NULL;
	}

	// P2-5: an image larger than the viewer can display is a size-limit
	// rejection, not out-of-memory. The old code set isOutOfMemory here, so
	// the provider freed memory and re-decoded the whole image, then reported
	// "not enough memory" for an image that simply exceeds the pixel cap.
	if (pBitmap->GetWidth() > MAX_IMAGE_DIMENSION || pBitmap->GetHeight() > MAX_IMAGE_DIMENSION)
	{
		isSizeLimitExceeded = true;
		return NULL;
	}
	if ((double)pBitmap->GetWidth() * pBitmap->GetHeight() > (double)CSettingsProvider::This().MaxImagePixels())
	{
		isSizeLimitExceeded = true;
		return NULL;
	}

	EImageFormat eImageFormat = GetBitmapFormat(pBitmap);

	// Handle multiframe images.
	// Note that only the first frame dimension is looked at, it is unclear if GDI+ anyway supports image formats with more dimensions
	UINT nDimensions = pBitmap->GetFrameDimensionsCount();
	GUID* pDimensionIDs = new GUID[nDimensions];
	pBitmap->GetFrameDimensionsList(pDimensionIDs, nDimensions);
	int nFrameCount = (nDimensions == 0) ? 1 : pBitmap->GetFrameCount(&pDimensionIDs[0]);
	nFrameIndex = max(0, min(nFrameCount - 1, nFrameIndex));
	int nFrameTimeMs = 100;
	if (nFrameCount > 1) {
		isAnimatedGIF = eImageFormat == IF_GIF;
		int nTagFrameDelaySize = pBitmap->GetPropertyItemSize(PropertyTagFrameDelay);
		if (nTagFrameDelaySize > 0) {
			PropertyItem* pPropertyItem = (PropertyItem*)new char[nTagFrameDelaySize];
			if (pBitmap->GetPropertyItem(PropertyTagFrameDelay, nTagFrameDelaySize, pPropertyItem) == Gdiplus::Ok) {
				// GDI+ does not guarantee one delay entry per frame. A forged GIF/TIFF
				// with fewer entries than frames made [nFrameIndex] read past the
				// property buffer.
				const size_t needed = ((size_t)nFrameIndex + 1) * sizeof(long);
				if (pPropertyItem->length >= needed && (size_t)nTagFrameDelaySize >= needed) {
					nFrameTimeMs = ((long*)pPropertyItem->value)[nFrameIndex] * 10;
				}
			}
			delete[] pPropertyItem;
		}
		GUID pageGuid = (eImageFormat == IF_TIFF) ? FrameDimensionPage : FrameDimensionTime;
		pBitmap->SelectActiveFrame(&pageGuid, nFrameIndex);
	}
	delete[] pDimensionIDs;

	// If there is an alpha channel in the original file we must blit the image onto a background color offscreen
	// bitmap first to achieve proper rendering.
	CJPEGImage* pJPEGImage = NULL;
	Gdiplus::PixelFormat pixelFormat = pBitmap->GetPixelFormat();
	bool bHasAlphaChannel = (pixelFormat & (PixelFormatAlpha | PixelFormatPAlpha));
	Gdiplus::Bitmap* pBmTarget = NULL;
	Gdiplus::Graphics* pBmGraphics = NULL;
	Gdiplus::Bitmap* pBitmapToUse;
	if (bHasAlphaChannel) {
		pBmTarget = new Gdiplus::Bitmap(pBitmap->GetWidth(), pBitmap->GetHeight(), PixelFormat32bppRGB);
		pBmGraphics = new Gdiplus::Graphics(pBmTarget);
		COLORREF bkColor = CSettingsProvider::This().ColorTransparency();
		Gdiplus::SolidBrush bkBrush(Gdiplus::Color(GetRValue(bkColor), GetGValue(bkColor), GetBValue(bkColor)));
		pBmGraphics->FillRectangle(&bkBrush, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		pBmGraphics->DrawImage(pBitmap, 0, 0, pBmTarget->GetWidth(), pBmTarget->GetHeight());
		pBitmapToUse = pBmTarget;
		if (pBmGraphics->GetLastStatus() == Gdiplus::OutOfMemory) {
			isOutOfMemory = true;
			delete pBmGraphics; delete pBmTarget;
			return NULL;
		}
	} else {
		pBitmapToUse = pBitmap;
	}

	Gdiplus::Rect bmRect(0, 0, pBitmap->GetWidth(), pBitmap->GetHeight());
	Gdiplus::BitmapData bmData;
	lastStatus = pBitmapToUse->LockBits(&bmRect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &bmData);
	if (lastStatus == Gdiplus::Ok) {
		assert(bmData.PixelFormat == PixelFormat32bppRGB);
		void* pDIB = CBasicProcessing::ConvertGdiplus32bppRGB(bmRect.Width, bmRect.Height, bmData.Stride, bmData.Scan0);
		if (pDIB != NULL) {
			pJPEGImage = new CJPEGImage(bmRect.Width, bmRect.Height, pDIB, pEXIFData, 4, nJPEGHash, eImageFormat,
				eImageFormat == IF_GIF && nFrameCount > 1, nFrameIndex, nFrameCount, nFrameTimeMs);
		}
		pBitmapToUse->UnlockBits(&bmData);
	} else if (lastStatus == Gdiplus::ValueOverflow) {
		isOutOfMemory = true;
	}

	if (pBmGraphics != NULL && pBmTarget != NULL) {
		delete pBmGraphics;
		delete pBmTarget;
	}

	pBitmap->GetLastStatus(); // reset status

	return pJPEGImage;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CImageLoadThread::CImageLoadThread(void) : CWorkThread(true) {
	m_pLastBitmap = NULL;
	m_pReadBuffer = NULL;
	m_nReadBufferCapacity = 0;
}

CImageLoadThread::~CImageLoadThread(void) {
	// Stop the worker before releasing the caches it may still be reading. The
	// base destructor runs after this one, so without this call the caches were
	// freed first and a WebP/PNG/JXL/AVIF frame decode in flight used freed memory.
	// Terminate() returns false when the thread outlives its 10 s window; on that
	// path the caches are leaked rather than freed out from under a running decode.
	if (Terminate()) {
		DeleteCachedGDIBitmap();
		DeleteCachedWebpDecoder();
		DeleteCachedPngDecoder();
		DeleteCachedJxlDecoder();
		DeleteCachedAvifDecoder();
		// E8: free the per-thread reuse buffer (same rule as the caches
		// above: skip the release when Terminate() times out, so a decode
		// still in flight cannot lose its buffer).
		delete[] m_pReadBuffer;
		m_pReadBuffer = NULL;
		m_nReadBufferCapacity = 0;
	}
}

int CImageLoadThread::AsyncLoad(LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams, HWND targetWnd, HANDLE eventFinished, bool forceFullSizeRAW) {
	// A2-9: 'new CRequest' can throw on allocation failure; AsyncLoad is
	// reached from the UI thread via StartNewRequest with no try/catch
	// above. Use nothrow and report -1 (never a valid handle: handles come
	// from InterlockedIncrement on m_curHandle, starting at 0, so the first
	// real handle is 1); StartNewRequest drops the request on -1.
	CRequest* pRequest = new(std::nothrow) CRequest(strFileName, nFrameIndex, targetWnd, processParams, eventFinished, forceFullSizeRAW);
	if (pRequest == NULL) {
		return -1;
	}

	ProcessAsync(pRequest);

	return pRequest->RequestHandle;
}

CImageData CImageLoadThread::GetLoadedImage(int nHandle) {
	Helpers::CAutoCriticalSection criticalSection(m_csList);
	CJPEGImage* imageFound = NULL;
	bool bFailedMemory = false;
	bool bFailedException = false;
	bool bFailedSizeLimit = false;
	std::list<CRequestBase*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		// P3-3: the list holds CRequestBase*; a CReleaseFileRequest has no
		// RequestHandle/Image fields, so only load requests (Type == 0, the
		// CRequestBase default; CRequest never changes it) may be read as
		// CRequest. Processed/Deleted/Type themselves live in the base class.
		if ((*iter)->Type != 0) {
			continue;
		}
		CRequest* pRequest = (CRequest*)(*iter);
		if (pRequest->Processed && pRequest->Deleted == false && pRequest->RequestHandle == nHandle) {
			imageFound = pRequest->Image;
			bFailedMemory = pRequest->OutOfMemory;
			bFailedException = pRequest->ExceptionError;
			bFailedSizeLimit = pRequest->SizeLimitExceeded;
			// only mark as deleted
			pRequest->Deleted = true;
			break;
		}
	}
	return CImageData(imageFound, bFailedMemory, bFailedException, bFailedSizeLimit);
}

void CImageLoadThread::ReleaseFile(LPCTSTR strFileName) {
	// A2-9: 'new CReleaseFileRequest' can throw on allocation failure;
	// ReleaseFile runs on the UI thread via ClearRequest with no
	// try/catch above. Use nothrow and skip the release on failure: the
	// release is best-effort (a failed wait is already tolerated the same
	// way in ProcessAndWait), the worst case is a stale cached decoder
	// surviving until the next load of that file.
	CReleaseFileRequest* pRequest = new(std::nothrow) CReleaseFileRequest(strFileName);
	if (pRequest == NULL) {
		return;
	}
	ProcessAndWait(pRequest);
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Protected
/////////////////////////////////////////////////////////////////////////////////////////////

// Called on the processing thread
void CImageLoadThread::ProcessRequest(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		CReleaseFileRequest& rq = (CReleaseFileRequest&)request;
		if (rq.FileName == m_sLastFileName) {
			DeleteCachedGDIBitmap();
		}
		if (rq.FileName == m_sLastWebpFileName) {
			DeleteCachedWebpDecoder();
		}
		if (rq.FileName == m_sLastPngFileName) {
			DeleteCachedPngDecoder();
		}
		if (rq.FileName == m_sLastJxlFileName) {
			DeleteCachedJxlDecoder();
		}
		if (rq.FileName == m_sLastAvifFileName) {
			DeleteCachedAvifDecoder();
		}
		// E6/E12: the file is being released for reload (e.g. after an external
		// edit), so its bad-file / probe conclusions must not survive.
		InvalidateLoadMemo(rq.FileName);
		return;
	}

	CRequest& rq = (CRequest&)request;
	double dStartTime = Helpers::GetExactTickCount();
	// Get image format and read the image. The format is sniffed once here:
	// the memo check below and the switch both need it.
	EImageFormat eFormat = GetImageFormat(rq.FileName);
	// E6: a RAW/TIFF that already failed decoding (bad-file memo) is not
	// decoded again - LibRaw, dcraw_mod and the thumbnail fallback would each
	// chew on it again. The memo is keyed by path + size + mtime, so an
	// edited file is re-tried; OOM is never memoized.
	__int64 nMemoSize = 0;
	FILETIME ftMemoWrite = { 0, 0 };
	bool bMemoIdentityKnown = false;
	if (eFormat == IF_CameraRAW || eFormat == IF_TIFF) {
		bMemoIdentityKnown = GetFileIdentity(rq.FileName, nMemoSize, ftMemoWrite);
		if (bMemoIdentityKnown) {
			LoadMemoEntry memo;
			if (LoadMemoLookup(rq.FileName, nMemoSize, ftMemoWrite, memo) && memo.bBad) {
				return;
			}
		}
	}
	switch (eFormat) {
		case IF_JPEG :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadJPEGRequest(&rq);
			break;
		case IF_WindowsBMP :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadBMPRequest(&rq);
			break;
		case IF_TGA :
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadTGARequest(&rq);
			break;
		case IF_WEBP:
			DeleteCachedGDIBitmap();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadWEBPRequest(&rq);
			break;
		case IF_PNG:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPNGRequest(&rq);
			break;
#ifndef WINXP
		case IF_JXL:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadJXLRequest(&rq);
			break;
		case IF_AVIF:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			ProcessReadAVIFRequest(&rq);
			break;
		case IF_HEIF:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadHEIFRequest(&rq);
			break;
		case IF_PSD:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPSDRequest(&rq);
			break;
		case IF_CameraRAW:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadRAWRequest(&rq);
			break;
#endif
		case IF_QOI:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadQOIRequest(&rq);
			break;
		case IF_PPM:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPPMRequest(&rq);
			break;
		case IF_HDR:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadHDRRequest(&rq);
			break;
		case IF_PBM:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPBMRequest(&rq);
			break;
		case IF_PAM:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadPAMRequest(&rq);
			break;
		case IF_JP2:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadJP2Request(&rq);
			break;
		case IF_EXR:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadEXRRequest(&rq);
			break;
		case IF_SVG:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadSVGRequest(&rq);
			break;
		case IF_WIC:
			DeleteCachedGDIBitmap();
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadWICRequest(&rq);
			break;
		default:
			// try with GDI+
			DeleteCachedWebpDecoder();
			DeleteCachedPngDecoder();
			DeleteCachedJxlDecoder();
			DeleteCachedAvifDecoder();
			ProcessReadGDIPlusRequest(&rq);
			break;
	}
	// then process the image if read was successful
	// R-16/E19: a post-decode failure is not a decode failure. Remember
	// whether AfterLoad failed so the memo below can tell the two apart.
	bool bAfterLoadFailed = false;
	if (rq.Image != NULL) {
		rq.Image->SetLoadTickCount(Helpers::GetExactTickCount() - dStartTime);
		// P2-6: tri-state. Only a genuine allocation failure sets OutOfMemory
		// (and may trigger the provider's free-and-retry); any other failure
		// just drops the image as a plain load error.
		EAfterLoadResult eAfterLoad = ProcessImageAfterLoad(&rq);
		if (eAfterLoad != AfterLoad_OK) {
			delete rq.Image;
			rq.Image = NULL;
			rq.OutOfMemory = (eAfterLoad == AfterLoad_OOM);
			bAfterLoadFailed = true;
		}
	}
	if (rq.Image == NULL && bMemoIdentityKnown && (eFormat == IF_CameraRAW || eFormat == IF_TIFF)) {
		// E6: remember the failure so the next visit skips the decoders.
		// OutOfMemory is transient and is never memoized.
		// R-16/E19: only a genuine decode failure is memoized. Skip when the
		// image decoded but AfterLoad failed (rotation / DIB size-limit: the
		// verdict depends on the processing parameters, not the file), when
		// the pixel cap rejected it (depends on MaxImagePixelsMP: raising the
		// cap must retry the file), and when the file cannot even be opened
		// right now (locked / antivirus / network hiccup: the failure may
		// be the open, not the decode). The re-check is one extra open on
		// the failure path only; a lock that clears between the decode and
		// this check can still be recorded - that race is unavoidable here,
		// the flags alone cannot tell an open failure from a decode failure.
		// P0: a TIFF gate rejection is also never memoized. The gate runs
		// before any decoder, so rejecting costs nothing to retry (a
		// re-attempt re-runs only the cheap header parse), while memoizing
		// it would lock the file out for the whole session on a single
		// false rejection.
		if (!rq.OutOfMemory && !bAfterLoadFailed && !rq.SizeLimitExceeded && !rq.GateRejected) {
			__int64 nRecheckSize = 0;
			FILETIME ftRecheck = { 0, 0 };
			if (GetFileIdentity(rq.FileName, nRecheckSize, ftRecheck)) {
				LoadMemoRecord(rq.FileName, nMemoSize, ftMemoWrite, true, false, false);
			}
		}
	}
}

// Called on the processing thread
void CImageLoadThread::AfterFinishProcess(CRequestBase& request) {
	if (request.Type == CReleaseFileRequest::ReleaseFileRequest) {
		return;
	}

	CRequest& rq = (CRequest&)request;
	if (rq.TargetWnd != NULL) {
		// post message to window that request has been processed
		::PostMessage(rq.TargetWnd, WM_IMAGE_LOAD_COMPLETED, 0, rq.RequestHandle);
	}

	// P3-1: release the per-thread reuse buffer once it grew past 64 MB.
	// The decode is finished and no decoder aliases the buffer (each cache
	// keeps its own copy), so freeing here is safe; it also covers the
	// open-once case, which a free-on-next-small-file would leave resident.
	if (m_nReadBufferCapacity > (size_t)64 * 1024 * 1024) {
		delete[] m_pReadBuffer;
		m_pReadBuffer = NULL;
		m_nReadBufferCapacity = 0;
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

static void LimitOffsets(CPoint& offsets, CSize clippingSize, const CSize & imageSize) {
	int nMaxOffsetX = (imageSize.cx - clippingSize.cx)/2;
	nMaxOffsetX = max(0, nMaxOffsetX);
	int nMaxOffsetY = (imageSize.cy - clippingSize.cy)/2;
	nMaxOffsetY = max(0, nMaxOffsetY);
	offsets.x = max(-nMaxOffsetX, min(+nMaxOffsetX, offsets.x));
	offsets.y = max(-nMaxOffsetY, min(+nMaxOffsetY, offsets.y));
}

void CImageLoadThread::DeleteCachedGDIBitmap() {
	if (m_pLastBitmap != NULL) {
		delete m_pLastBitmap;
	}
	m_pLastBitmap = NULL;
	m_sLastFileName.Empty();
}

void CImageLoadThread::DeleteCachedWebpDecoder() {
	m_webpReader.DeleteCache();
	m_sLastWebpFileName.Empty();
}

void CImageLoadThread::DeleteCachedPngDecoder() {
#ifndef WINXP
	m_pngReader.DeleteCache();
	m_sLastPngFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedJxlDecoder() {
#ifndef WINXP
	m_jxlReader.DeleteCache();
	m_sLastJxlFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedAvifDecoder() {
#ifndef WINXP
	// prevent crashing when libavif/dav1d fail or missing
	CErrorModeGuard errorModeGuard;
	try {
		m_avifReader.DeleteCache();
	} catch (...) {}
	m_sLastAvifFileName.Empty();
#endif
}

// Applies the color profile embedded in a JPEG to the decoded pixels, in place.
// TurboJPEG hands back BGR rows padded to four bytes, which is the layout lcms2
// writes, so the conversion can happen in the same buffer. Nothing happens when
// the file carries no profile, when the profile already is sRGB (what cameras
// mostly write), or when it is not an RGB profile at all.
static void ApplyEmbeddedICCProfile(void* pPixelData, int nWidth, int nHeight, int nChannels,
                                    void* pJPEGStream, int nStreamLength) {
	if (pPixelData == NULL || nChannels != 3 || !CSettingsProvider::This().UseEmbeddedColorProfiles()) {
		return;
	}
	unsigned int nProfileSize = 0;
	uint8* pProfile = (uint8*) Helpers::ExtractICCProfile(pJPEGStream, nStreamLength, &nProfileSize);
	if (pProfile == NULL) {
		return;
	}
	if (!ICCProfileTransform::IsNoTransformNeeded(pProfile, nProfileSize)) {
		void* transform = ICCProfileTransform::CreateTransform(pProfile, nProfileSize, ICCProfileTransform::FORMAT_BGR);
		if (transform != NULL) {
			ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, nWidth, nHeight,
				Helpers::DoPadding(nWidth * 3, 4));
			ICCProfileTransform::DeleteTransform(transform);
		}
	}
	delete[] pProfile;
}

// Convert a TJPF_CMYK plane to BGR. libjpeg-turbo hands back inverted CMYK
// (255-C, 255-M, 255-Y, 255-K): ycck_cmyk_convert writes 255-R/G/B into the
// first three components, and for a YCCK source those R/G/B are already the ink
// amounts, so the output is the additive inverse of the ink. Flip all four
// channels back to ink and, in the same pass, swap C and Y because lcms2's
// FORMAT_YMCK expects Y-M-C-K. The embedded profile is used when present;
// R-07 (measured 2026-11-04, closed): the invert-all-four step above was
// validated only for Adobe-polarity output until a genuinely non-Adobe CMYK
// file without APP14 was measured: a 1600x860 test chart with 16 known-ink
// patches (no APP14, no embedded ICC, ASCII C/M/Y/K component IDs) displayed
// normally on an aa26097 build, so the device decoder's output for no-APP14
// files is the same Adobe polarity (255-ink) as for APP14 files. No separate
// branch is needed for the no-APP14 case.
// otherwise CreateTransform falls back to the shipped cmyk.icm. Returns NULL
// when no transform could be built (lcms2 or cmyk.icm missing, e.g. the XP
// build). On every NULL return the plane has already been converted in
// place to YMCK ink by the loop above, so the caller can still run the
// naive ink-formula conversion on it (R-09 fallback step 3) instead of
// falling straight back to GDI+.
static void* ConvertCmykToRgb(void* pCmyk, int nWidth, int nHeight, void* pJPEGStream, int nStreamLength) {
	// Invert all four channels and swap C<->Y in one pass. TJPAD(width*4) ==
	// width*4 (already 4-aligned), so there is no row padding. Walk by pixel
	// count as size_t: nWidth*nHeight can exceed INT_MAX for large images.
	unsigned char* p = (unsigned char*)pCmyk;
	const size_t nPixels = (size_t)nWidth * (size_t)nHeight;
	for (size_t i = 0; i < nPixels; i++, p += 4) {
		const unsigned char c = p[0];
		p[0] = (unsigned char)(255 - p[2]); // Y -> C slot
		p[2] = (unsigned char)(255 - c);    // C -> Y slot
		p[1] = (unsigned char)(255 - p[1]); // M
		p[3] = (unsigned char)(255 - p[3]); // K
	}

	unsigned int nProfileSize = 0;
	uint8* pProfile = (uint8*) Helpers::ExtractICCProfile(pJPEGStream, nStreamLength, &nProfileSize);
	void* transform = ICCProfileTransform::CreateTransform(pProfile, nProfileSize, ICCProfileTransform::FORMAT_YMCK);
	delete[] pProfile;
	if (transform == NULL) {
		return NULL;
	}

	const size_t outStride = (size_t)Helpers::DoPadding(nWidth * 3, 4);
	size_t outBytes = 0;
	if (!IsImageBufferSizeOk(outStride, (size_t)nHeight, &outBytes)) {
		ICCProfileTransform::DeleteTransform(transform);
		return NULL;
	}
	void* pRgb = new(std::nothrow) unsigned char[outBytes];
	if (pRgb == NULL) {
		ICCProfileTransform::DeleteTransform(transform);
		return NULL;
	}

	if (!ICCProfileTransform::DoTransform(transform, pCmyk, pRgb, nWidth, nHeight, (unsigned int)(nWidth * 4))) {
		delete[] pRgb;
		ICCProfileTransform::DeleteTransform(transform);
		return NULL;
	}
	ICCProfileTransform::DeleteTransform(transform);
	return pRgb;
}

// R-09 fallback step 3: naive YMCK-ink to BGR conversion, used when no lcms2
// transform could be built (embedded profile unreadable and cmyk.icm
// missing). Input is the plane ConvertCmykToRgb already converted in place
// to YMCK ink: p[0] = Y, p[1] = M, p[2] = C, p[3] = K, each 0..255.
// The ink formula gives approximate color with no profile and no GDI+:
//   R = (255 - C) * (255 - K) / 255, and likewise G from M, B from Y.
// Output is BGR with the same padded stride as the lcms2 path, so the
// caller can use it interchangeably. Returns NULL only when the output
// buffer cannot be allocated.
static void* ConvertYmckInkToRgbNaive(void* pYmck, int nWidth, int nHeight) {
	const size_t outStride = (size_t)Helpers::DoPadding(nWidth * 3, 4);
	size_t outBytes = 0;
	if (!IsImageBufferSizeOk(outStride, (size_t)nHeight, &outBytes)) {
		return NULL;
	}
	unsigned char* pRgb = new(std::nothrow) unsigned char[outBytes];
	if (pRgb == NULL) {
		return NULL;
	}
	const unsigned char* p = (const unsigned char*)pYmck;
	unsigned char* q = pRgb;
	for (int y = 0; y < nHeight; y++) {
		unsigned char* row = q;
		for (int x = 0; x < nWidth; x++, p += 4) {
			const unsigned int inkK = 255 - p[3];
			row[0] = (unsigned char)((255 - p[0]) * inkK / 255); // B from Y
			row[1] = (unsigned char)((255 - p[1]) * inkK / 255); // G from M
			row[2] = (unsigned char)((255 - p[2]) * inkK / 255); // R from C
			row += 3;
		}
		q += outStride;
	}
	return pRgb;
}

// E8: read a whole file into the thread's reusable buffer. The buffer grows
// on demand and is released after a request once it passed 64 MB (P3-1);
// it is owned by this CImageLoadThread instance, so the
// two load threads never share it (R-01). Only "borrow" readers use it: the
// decoder reads synchronously from the buffer and keeps no pointer to it
// (QOI/PPM/HDR/PBM/JP2/EXR/SVG, HEIF, and WebP/PNG/AVIF, whose animation
// caches keep their own malloc'ed copies). Two readers must never use it:
// JPEG keeps its HGLOBAL buffer because the GDI+ fallback needs a stream
// built on a moveable global (CreateStreamOnHGlobal), and JXL takes over
// the caller's buffer (JxlReader::DeleteCache delete[]s cache.data, see
// R-20) - handing it the shared buffer would free it out from under the
// next load. Nested use cannot happen either: no function that reads through
// this buffer calls another one that does (AVIF's HEIF fallback takes the
// buffer as a parameter and decodes from it without touching this member;
// GDI+/WIC/RAW fallbacks read by filename).
// Returns false only when the buffer cannot be grown (the caller reports
// out-of-memory, exactly as for a failed new[] today). A short read is NOT
// a failure here: nBytesReadOut reports what was actually read and the
// caller treats a short read as a plain decode failure, exactly as before.
bool CImageLoadThread::ReadWholeFileIntoReuseBuffer(HANDLE hFile, long long nFileSize, char*& pBufferOut, unsigned int& nBytesReadOut) {
	pBufferOut = NULL;
	nBytesReadOut = 0;
	if (nFileSize < 0) {
		return false;
	}
	size_t nNeeded = (size_t)nFileSize;
	if (nNeeded == 0) {
		nNeeded = 1; // new char[0] returns a non-NULL pointer; keep that
	}
	if (nNeeded > m_nReadBufferCapacity) {
		char* pNew = new(std::nothrow) char[nNeeded];
		if (pNew == NULL) {
			return false; // old buffer (if any) stays intact
		}
		delete[] m_pReadBuffer;
		m_pReadBuffer = pNew;
		m_nReadBufferCapacity = nNeeded;
	}
	pBufferOut = m_pReadBuffer;
	DWORD nRead = 0;
	::ReadFile(hFile, m_pReadBuffer, (DWORD)nFileSize, &nRead, NULL);
	nBytesReadOut = nRead;
	return true;
}

void CImageLoadThread::ProcessReadJPEGRequest(CRequest * request) {
	HANDLE hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}

	void* pBuffer = NULL;
	try {
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_JPEG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The GDI+ branch below still needs
		// a moveable global for its stream, so it makes a temporary copy.
		unsigned int nNumBytesRead = 0;
		char* pReadBuffer = NULL;
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pReadBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		pBuffer = pReadBuffer;
		if (nNumBytesRead == nFileSize) {
			bool bUseGDIPlus = CSettingsProvider::This().ForceGDIPlus();
			if (bUseGDIPlus) {
				// E8: the stream needs a moveable global, so the GDI+ path
				// works on a temporary copy of the reuse buffer. The copy is
				// freed below, after the stream is released and the bitmap is
				// deleted - the same lifetime order as the old HGLOBAL.
				HGLOBAL hTempBuffer = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
				void* pTempBuffer = (hTempBuffer == NULL) ? NULL : ::GlobalLock(hTempBuffer);
				if (pTempBuffer == NULL) {
					if (hTempBuffer) ::GlobalFree(hTempBuffer);
					request->OutOfMemory = true;
				} else {
					memcpy(pTempBuffer, pBuffer, (size_t)nFileSize);
					IStream* pStream = NULL;
					if (::CreateStreamOnHGlobal(hTempBuffer, FALSE, &pStream) == S_OK) {
						Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
						bool isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded;
						request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, Helpers::FindEXIFBlock(pBuffer, nFileSize),
							Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded);
						request->OutOfMemory = request->Image == NULL && isOutOfMemory;
						request->SizeLimitExceeded = request->Image == NULL && isSizeLimitExceeded;
						if (request->Image != NULL) {
							// F-H10: preserve the source JPEG's APP/COM segments for saving.
							CString sJPEGComment = Helpers::GetJPEGComment(pBuffer, nFileSize);
							request->Image->SetJPEGComment(sJPEGComment);
							int nSegSize = 0, nSegBeforeEXIF = 0;
							void* pSegments = Helpers::FindPreservedJPEGSegments(pBuffer, nFileSize, nSegSize, nSegBeforeEXIF, !sJPEGComment.IsEmpty());
							request->Image->SetJPEGSegments(pSegments, nSegSize, nSegBeforeEXIF);
						}
						pStream->Release();
						delete pBitmap;
					} else {
						request->OutOfMemory = true;
					}
					::GlobalUnlock(hTempBuffer);
					::GlobalFree(hTempBuffer);
				}
			}
			if (!bUseGDIPlus || request->OutOfMemory) {
				int nWidth, nHeight, nBPP;
				TJSAMP eChromoSubSampling;
				bool bOutOfMemory;
				bool bSizeLimitExceeded = false;
				// int nTicks = ::GetTickCount();

				void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, bSizeLimitExceeded, pBuffer, nFileSize);
				
				/*
				TCHAR buffer[20];
				_stprintf_s(buffer, 20, _T("%d"), ::GetTickCount() - nTicks);
				::MessageBox(NULL, CString(_T("Elapsed ticks: ")) + buffer, _T("Time"), MB_OK);
				*/

				// CMYK/YCCK JPEGs decode to a 4-component C-M-Y-K plane; convert to BGR here.
				if (pPixelData != NULL && nBPP == 4) {
					void* pRGB = ConvertCmykToRgb(pPixelData, nWidth, nHeight, pBuffer, (int) nFileSize);
					if (pRGB == NULL) {
						// R-09 fallback step 3: the plane was already
						// converted in place to YMCK ink before the
						// transform failed, so the naive ink-formula
						// conversion can reuse it. Approximate color,
						// but no GDI+ and no crash risk.
						pRGB = ConvertYmckInkToRgbNaive(pPixelData, nWidth, nHeight);
					}
					delete[] pPixelData;
					if (pRGB != NULL) {
						request->Image = new CJPEGImage(nWidth, nHeight, pRGB, 
							Helpers::FindEXIFBlock(pBuffer, nFileSize), 3, 
							Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
						// F-H10: preserve the source JPEG's APP/COM segments for saving.
						CString sJPEGComment = Helpers::GetJPEGComment(pBuffer, nFileSize);
						request->Image->SetJPEGComment(sJPEGComment);
						int nSegSize = 0, nSegBeforeEXIF = 0;
						void* pSegments = Helpers::FindPreservedJPEGSegments(pBuffer, nFileSize, nSegSize, nSegBeforeEXIF, !sJPEGComment.IsEmpty());
						request->Image->SetJPEGSegments(pSegments, nSegSize, nSegBeforeEXIF);
						// R-06: TurboJpeg lets TJSAMP_UNKNOWN (-1) through for CMYK,
						// but CanUseLosslessJPEGTransformations /
						// TrimRectToMCUBlockSize index tjMCUWidth/tjMCUHeight
						// with it: an out-of-bounds read, and a divide by zero
						// if the slot happens to read 0. Normalize to 444 here.
						// This only keeps the MCU indexing safe; it does NOT
						// claim the source image actually is 4:4:4.
						if (eChromoSubSampling == TJSAMP_UNKNOWN) {
							eChromoSubSampling = TJSAMP_444;
						}
						request->Image->SetJPEGChromoSampling(eChromoSubSampling);
					} else {
						// R-09 fallback step 4: both the profile
						// transform and the naive conversion failed.
#ifdef WINXP
						// GDI+ only on XP, where lcms2 is unavailable.
						// Logging the fallback matters: on non-XP builds
						// this path is never taken, so a silent GDI+
						// fallback cannot re-expose the large-profile
						// heap corruption 91d1d61 was meant to avoid.
						OutputDebugString(_T("JPEGView: CMYK transform unavailable, falling back to GDI+ (XP only).\n"));
						// Pass the EXIF block and hash from the original
						// JPEG buffer, like the success path does; the
						// plain GDI+ path passes NULL/0 and loses both.
						ProcessReadGDIPlusRequest(request,
							Helpers::FindEXIFBlock(pBuffer, nFileSize),
							Helpers::CalculateJPEGFileHash(pBuffer, nFileSize));
#else
						// On builds with lcms2 the naive conversion
						// only fails when its output buffer cannot be
						// allocated, so report out of memory instead
						// of falling back to GDI+.
						request->OutOfMemory = true;
#endif
					}
				} else if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
					ApplyEmbeddedICCProfile(pPixelData, nWidth, nHeight, nBPP, pBuffer, (int) nFileSize);
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, 
						Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP, 
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
					// F-H10: preserve the source JPEG's APP/COM segments for saving.
					CString sJPEGComment = Helpers::GetJPEGComment(pBuffer, nFileSize);
					request->Image->SetJPEGComment(sJPEGComment);
					int nSegSize = 0, nSegBeforeEXIF = 0;
					void* pSegments = Helpers::FindPreservedJPEGSegments(pBuffer, nFileSize, nSegSize, nSegBeforeEXIF, !sJPEGComment.IsEmpty());
					request->Image->SetJPEGSegments(pSegments, nSegSize, nSegBeforeEXIF);
					request->Image->SetJPEGChromoSampling(eChromoSubSampling);
				} else if (bOutOfMemory) {
					request->OutOfMemory = true;
				} else if (bSizeLimitExceeded) {
					// R-04/P2-5: TurboJpeg proved the file exceeds the pixel cap; the GDI+ fallback would reject it the same way after decoding the whole bitmap, so skip it. request->OutOfMemory may already be set from the earlier GDI+ attempt: clear it, a size-limit verdict must never trigger the provider's free-and-retry.
					request->OutOfMemory = false;
					request->SizeLimitExceeded = true;
				} else {
					// failed, try GDI+
					delete[] pPixelData;
					ProcessReadGDIPlusRequest(request);
				}
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}


void CImageLoadThread::ProcessReadBMPRequest(CRequest * request) {
	bool bOutOfMemory;
	bool bSizeLimitExceeded = false;
	request->Image = CReaderBMP::ReadBmpImage(request->FileName, bOutOfMemory, bSizeLimitExceeded);
	if (bSizeLimitExceeded) {
		// R-04/P2-5: pixel-cap rejection, not OOM; skip the GDI+ fallback, which would reject the same file the same way after decoding it.
		request->SizeLimitExceeded = true;
	} else if (bOutOfMemory) {
		request->OutOfMemory = true;
	} else if (request->Image == NULL) {
		// probably one of the bitmap formats that can not be read directly, try with GDI+
		ProcessReadGDIPlusRequest(request);
	}
}

void CImageLoadThread::ProcessReadTGARequest(CRequest * request) {
	bool bOutOfMemory;
	bool bSizeLimitExceeded = false;
	request->Image = CReaderTGA::ReadTgaImage(request->FileName, CSettingsProvider::This().ColorTransparency(), bOutOfMemory, bSizeLimitExceeded);
	if (bSizeLimitExceeded) {
		// R-04/P2-5: pixel-cap rejection, not OOM.
		request->SizeLimitExceeded = true;
	} else if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
}

void CImageLoadThread::ProcessReadWEBPRequest(CRequest * request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastWebpFileName) {
		DeleteCachedWebpDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_WEBP_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			// E8: read into the per-thread reuse buffer (see
			// ReadWholeFileIntoReuseBuffer); the animation cache keeps its
			// own malloc'ed copy, so it never holds this buffer.
			if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (nNumBytesRead == nFileSize)) {
			int nWidth, nHeight;
			bool bHasAnimation = bUseCachedDecoder;
			int nFrameCount = 1;
			int nFrameTimeMs = 0;
			int nBPP;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)m_webpReader.ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, nFileSize);
			if (pPixelData && nBPP == 4) {
				// F-14/E13: alpha blend with a size_t counter and LUTs, skipping
				// the loop entirely when no pixel is transparent (see
				// AlphaBlendImageToBackground; pixel results are identical).
				AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());

				if (bHasAnimation) {
					m_sLastWebpFileName = sFileName;
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_WEBP, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
			else {
				delete[] pPixelData;
				DeleteCachedWebpDecoder();
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
	}
}

#ifndef WINXP
void CImageLoadThread::ProcessReadPNGRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastPngFileName) {
		DeleteCachedPngDecoder();
	}
	else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	void* pBuffer = NULL;
	try {
		long long nFileSize;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_PNG_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
			// E8: read into the per-thread reuse buffer (see
			// ReadWholeFileIntoReuseBuffer). The PNG cache keeps its own
			// malloc'ed copy, so it never holds this buffer; the GDI+
			// fallback below still needs a moveable global, so it makes a
			// temporary copy there.
			char* pReadBuffer = NULL;
			if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pReadBuffer, nNumBytesRead)) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
			pBuffer = pReadBuffer;
		} else {
			nFileSize = 0; // to avoid compiler warnings, not used
		}
		if (bUseCachedDecoder || (nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			uint8* pPixelData = NULL;
			void* pEXIFData = NULL;

#ifndef WINXP
			// The iCCP profile is applied by PngReader::ReadNextFrame through lcms2,
			// not by libpng itself — libpng only inflates the chunk. Do not delete
			// that conversion; the only reason to take GDI+ would be ForceGDIPlus.
			bool bUseGDIPlus = CSettingsProvider::This().ForceGDIPlus();
			if (bUseCachedDecoder || !bUseGDIPlus || PngReader::MustUseLibpng(pBuffer, nFileSize))
				pPixelData = (uint8*)m_pngReader.ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, nFileSize);
#endif

			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastPngFileName = sFileName;
				// F-14/E13: alpha blend with a size_t counter and LUTs, skipping
				// the loop entirely when no pixel is transparent (see
				// AlphaBlendImageToBackground; pixel results are identical).
				AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
			} else {
				DeleteCachedPngDecoder();
				
				// R-04/P2-5: libpng already proved the file exceeds the pixel cap; GDI+ would reject it the same way after decoding the whole bitmap, so skip the fallback and keep the verdict.
				if (!request->SizeLimitExceeded) {
					// E8: the stream needs a moveable global, so the GDI+
					// fallback works on a temporary copy of the reuse buffer,
					// freed below after the stream is released and the bitmap
					// is deleted (the same lifetime order as before).
					HGLOBAL hTempBuffer = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
					void* pTempBuffer = (hTempBuffer == NULL) ? NULL : ::GlobalLock(hTempBuffer);
					if (pTempBuffer == NULL) {
						if (hTempBuffer) ::GlobalFree(hTempBuffer);
						request->OutOfMemory = true;
					} else {
						memcpy(pTempBuffer, pBuffer, (size_t)nFileSize);
						IStream* pStream = NULL;
						if (::CreateStreamOnHGlobal(hTempBuffer, FALSE, &pStream) == S_OK) {
							Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
							bool isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded;
							pEXIFData = PngReader::GetEXIFBlock(pBuffer, nFileSize);
							request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, pEXIFData, 0, isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded);
							request->OutOfMemory = request->Image == NULL && isOutOfMemory;
							request->SizeLimitExceeded = request->Image == NULL && isSizeLimitExceeded;
							pStream->Release();
							delete pBitmap;
						} else {
							request->OutOfMemory = true;
						}
						::GlobalUnlock(hTempBuffer);
						::GlobalFree(hTempBuffer);
					}
				}
			}
			free(pEXIFData);
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
	}
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadJXLRequest(CRequest* request) {
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastJxlFileName) {
		DeleteCachedJxlDecoder();
	} else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	uint8_t* pBuffer = NULL; // JxlReader takes ownership and releases with delete[]
	// R-20: set once ReadImage has been called (ownership moved to the
	// reader cache); the catch below frees pBuffer only while this is false.
	bool bJxlReaderOwnsBuffer = false;
	// R-20: hoisted from the decode block below so the catch can free the
	// EXIF chunk when the CJPEGImage constructor throws after ReadImage
	// handed it out (free(NULL) is safe when ReadImage never ran).
	void* pEXIFData = NULL;
	CErrorModeGuard errorModeGuard;
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_JXL_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) uint8_t[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			// R-20: ownership of pBuffer passes to the JXL reader inside
			// ReadImage (JXLWrapper.cpp:48-64); set the flag before the
			// call so the catch below never frees a buffer the reader owns.
			bJxlReaderOwnsBuffer = true;
			uint8* pPixelData = (uint8*)m_jxlReader.ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastJxlFileName = sFileName;
				// F-14/E13: alpha blend with a size_t counter and LUTs, skipping
				// the loop entirely when no pixel is transparent (see
				// AlphaBlendImageToBackground; pixel results are identical).
				AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_JXL, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			} else {
				DeleteCachedJxlDecoder();
			}
		} else {
			// L-D09: the read failed before JxlReader ever took ownership
			// of pBuffer (it only releases buffers it received), so the
			// old code fell through and leaked it. Release it here.
			// pBuffer is NULL on the cached path, where this branch is
			// unreachable, so delete[] is safe in all cases.
			delete[] pBuffer;
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
		// R-20 residual: free the file buffer only when the JXL reader
		// never took ownership of it (an exception before ReadImage).
		// After ReadImage the buffer belongs to the reader cache
		// (JXLWrapper.cpp:48-64): it is freed by DeleteCachedJxlDecoder
		// on the failure path, by DeleteCache inside ReadImage for still
		// images, or kept for the next animation frame. Freeing it here
		// would double-free, and clearing the cache here would destroy a
		// valid animation decoder on the cached path. The EXIF chunk is
		// likewise freed here only when ReadImage handed it out and the
		// CJPEGImage constructor never took it (free(NULL) is safe).
		if (!bJxlReaderOwnsBuffer) {
			delete[] pBuffer;
		}
		free(pEXIFData);
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		// R-20: pBuffer is NOT freed here: ownership passed to the JXL
		// reader inside ReadImage (see the catch above), and the reader
		// cache releases it. (The old commented-out delete[] here would
		// have double-freed on the success path.)
	}
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadAVIFRequest(CRequest* request) {
	bool bSuccess = false;
	bool bUseCachedDecoder = false;
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;
	if (sFileName != m_sLastAvifFileName) {
		DeleteCachedAvifDecoder();
	} else {
		bUseCachedDecoder = true;
	}

	HANDLE hFile;
	if (!bUseCachedDecoder) {
		hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}
	}
	char* pBuffer = NULL;
	CErrorModeGuard errorModeGuard;
	// E8: file size at function scope so the AVIF->HEIF fallback below can
	// reuse the already-read buffer (nFileSize is needed after the try).
	long long nFileSize = 0;
	try {
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_HEIF_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			// E8: read into the per-thread reuse buffer (see
			// ReadWholeFileIntoReuseBuffer); the animation cache keeps its
			// own malloc'ed copy, so it never holds this buffer.
			if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)m_avifReader.ReadImage(nWidth, nHeight, nBPP, bHasAnimation, request->FrameIndex, 
				nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastAvifFileName = sFileName;
				// F-14/E13: alpha blend with a size_t counter and LUTs, skipping
				// the loop entirely when no pixel is transparent (see
				// AlphaBlendImageToBackground; pixel results are identical).
				AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_AVIF, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
				bSuccess = true;
			} else {
				DeleteCachedAvifDecoder();
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	// E8: fall back to HEIF with the buffer this function already read, instead
	// of letting the HEIF path open and read the file a second time. The buffer
	// is still owned here, so the fallback runs before it is freed below. On
	// the cached-decoder path there is no buffer; fall back the old way.
	// P3-2: a size-limit rejection must not fall back to HEIF: HEIF has its
	// own pixel cap and would reject the same file after a second full parse.
	// Guarded here, not an early return, so the CloseHandle below still runs.
	if (!bSuccess && !request->SizeLimitExceeded) {
		if (!bUseCachedDecoder && pBuffer != NULL) {
			ProcessReadHEIFRequest(request, pBuffer, nFileSize);
		} else {
			ProcessReadHEIFRequest(request);
		}
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
	}
}
#endif

#ifndef WINXP
void CImageLoadThread::ProcessReadHEIFRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	CErrorModeGuard errorModeGuard;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_HEIF_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			ProcessReadHEIFRequest(request, pBuffer, nFileSize);
		}
	} catch(heif::Error he) {
		// invalid image
		delete request->Image;
		request->Image = NULL;
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

// E8: HEIF decode from a buffer the caller already read (the AVIF fallback).
// No file open here; the caller owns the buffer and frees it. The size cap
// was already applied by the caller (same MAX_HEIF_FILE_SIZE), so it is not
// re-checked - the buffer is at most that large by construction.
void CImageLoadThread::ProcessReadHEIFRequest(CRequest* request, const char* pBuffer, long long nFileSize) {
	CErrorModeGuard errorModeGuard;
	try {
		int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
		nFrameCount = 1;
		nFrameTimeMs = 0;
		void* pEXIFData;
		uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->SizeLimitExceeded, request->FrameIndex, pBuffer, nFileSize);
		if (pPixelData != NULL) {
			// F-14/E13: alpha blend with a size_t counter and LUTs, skipping
			// the loop entirely when no pixel is transparent (see
			// AlphaBlendImageToBackground; pixel results are identical).
			AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());

			request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_HEIF, false, request->FrameIndex, nFrameCount, nFrameTimeMs);
			free(pEXIFData);
		}
	} catch(heif::Error he) {
		// invalid image
		delete request->Image;
		request->Image = NULL;
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
}

void CImageLoadThread::ProcessReadPSDRequest(CRequest* request) {
	request->Image = PsdReader::ReadImage(request->FileName, request->OutOfMemory, request->SizeLimitExceeded);
	// R-04/P2-5: a size-limit rejection must not fall through to the embedded-thumbnail reader the way a plain decode failure does.
	if (request->Image == NULL && !request->OutOfMemory && !request->SizeLimitExceeded) {
		request->Image = PsdReader::ReadThumb(request->FileName, request->OutOfMemory, request->SizeLimitExceeded);
	}
}

#endif

void CImageLoadThread::ProcessReadQOIRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}

		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP;
			void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (nBPP == 4) {
					// F-14/E13: see AlphaBlendImageToBackground (size_t counter, LUTs,
					// opaque skip; pixel results are identical).
					AlphaBlendImageToBackground((uint32*)pPixelData, (size_t)nWidth * (size_t)nHeight, CSettingsProvider::This().ColorTransparency());
				}
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, nBPP, 0, IF_QOI, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadPPMRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI: 300 MB Win64, 50 MB Win32,
		// so CFV.PPM at 50,331,665 bytes still fits on Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = PPMReader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_PPM, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}
void CImageLoadThread::ProcessReadHDRRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = HDRReader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_HDR, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadPBMRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = PBMReader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_PBM, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadPAMRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = PAMReader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_PAM, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadJP2Request(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = JP2Reader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_JP2, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadEXRRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = EXRReader::ReadImage(nWidth, nHeight, request->OutOfMemory, request->SizeLimitExceeded, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_EXR, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

void CImageLoadThread::ProcessReadSVGRequest(CRequest* request) {
	HANDLE hFile;
	hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}
	char* pBuffer = NULL;
	try {
		unsigned int nNumBytesRead;
		// Don't read too huge files (same cap as QOI/PPM: 300 MB Win64, 50 MB Win32).
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_PNG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		// E8: read into the per-thread reuse buffer (see
		// ReadWholeFileIntoReuseBuffer). The reader decodes synchronously
		// from the buffer and keeps no pointer to it.
		if (!ReadWholeFileIntoReuseBuffer(hFile, nFileSize, pBuffer, nNumBytesRead)) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (nNumBytesRead == nFileSize) {
			int nWidth, nHeight;
			void* pPixelData = SVGReader::ReadImage(nWidth, nHeight, request->OutOfMemory, pBuffer, (int)nFileSize);
			if (pPixelData != NULL) {
				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, NULL, 3, 0, IF_SVG, false, 0, 1, 0);
			}
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	::CloseHandle(hFile);
}

// TIFF gate rejection reasons (P0-2). The gate returns only a bool, so
// without a reason a gate rejection and a genuinely corrupt file look
// identical upstream. Each rejection is logged once per file (see
// LogTiffGateRejectionOnce) with the reason, the IFD offset and the
// offending values, so the next false rejection can be diagnosed from
// the debug log instead of from a user report that "a RAW won't open".
// A non-TIFF file passing through the gate is NOT a rejection and is
// never logged.
enum TiffGateRejectReason {
	GateReject_None = 0,
	GateReject_OpenFailed,          // CreateFile failed (locked / missing)
	GateReject_SizeInvalid,         // GetFileSizeEx failed or size <= 0
	GateReject_HeaderReadFailed,    // first 8 bytes unreadable
	GateReject_IfdOffsetZero,       // TIFF header declares IFD0 offset 0
	GateReject_IfdUnreadable,       // ReadIFD: entry count unreadable / entry block outside file
	GateReject_IfdEmpty,            // ReadIFD: entry count == 0
	GateReject_SubIfdBad,           // tag 330: count > 1024, bad type, or element unreadable
	GateReject_NodeCapExceeded,     // > 4096 nodes collected (fail-closed cap, never skipped)
	GateReject_DimUnpaired,         // width declared without height or vice versa
	GateReject_DimZero,             // declared width or height == 0
	GateReject_DimTooBig,           // declared width or height > 65535
	GateReject_CheckDimInvalid,     // CheckIFD: largest IFD dimensions invalid
	GateReject_CheckSamplesTooMany, // CheckIFD: samples > 8
	GateReject_CheckBitsTooMany,    // CheckIFD: bits > 64
	GateReject_CheckBitsMissing,    // CheckIFD uncompressed: BitsPerSample missing
	GateReject_CheckSamplesMissing, // CheckIFD uncompressed: SamplesPerPixel missing, photometric not mono/CFA
	GateReject_CheckDeclaredBytes,  // CheckIFD uncompressed: declaredBytes > fileSize
	GateReject_CheckPixelsFloor,    // CheckIFD: fileSize < pixels/8
	GateReject_CheckStripPair,      // CheckIFD compressed: strip offset/count pair invalid
	GateReject_CheckTilePair,       // CheckIFD compressed: tile offset/count pair invalid
	GateReject_DimAllSkipped       // every dimension-declaring IFD was skipped as a thumbnail
};

// How CollectIFDTree failed. Fix 2 treats the two failures differently:
// an unreadable IFD is skippable outside IFD0 (dcraw breaks the chain /
// skips the SubIFD and keeps decoding), while the 4096-node cap is a
// fail-closed resource bound and is NEVER skipped (Fix 8 territory,
// explicitly not done). The cap is therefore a separate result, never
// confused with an unreadable IFD.
enum TiffGateCollectResult {
	TiffGateCollect_Ok,         // tree collected (possibly with skipped IFDs)
	TiffGateCollect_BadIfd,     // an IFD that must be read was unreadable
	TiffGateCollect_CapExceeded // > 4096 nodes: fail closed, never skipped
};

static LPCTSTR TiffGateRejectReasonName(TiffGateRejectReason reason) {
	switch (reason) {
		case GateReject_OpenFailed: return _T("open-failed");
		case GateReject_SizeInvalid: return _T("size-invalid");
		case GateReject_HeaderReadFailed: return _T("header-read-failed");
		case GateReject_IfdOffsetZero: return _T("ifd-offset-zero");
		case GateReject_IfdUnreadable: return _T("ifd-unreadable");
		case GateReject_IfdEmpty: return _T("ifd-empty");
		case GateReject_SubIfdBad: return _T("subifd-bad");
		case GateReject_NodeCapExceeded: return _T("node-cap-exceeded");
		case GateReject_DimUnpaired: return _T("dim-unpaired");
		case GateReject_DimZero: return _T("dim-zero");
		case GateReject_DimTooBig: return _T("dim-too-big");
		case GateReject_CheckDimInvalid: return _T("check-dim-invalid");
		case GateReject_CheckSamplesTooMany: return _T("check-samples-too-many");
		case GateReject_CheckBitsTooMany: return _T("check-bits-too-many");
		case GateReject_CheckBitsMissing: return _T("check-bits-missing");
		case GateReject_CheckSamplesMissing: return _T("check-samples-missing");
		case GateReject_CheckDeclaredBytes: return _T("check-declared-bytes");
		case GateReject_CheckPixelsFloor: return _T("check-pixels-floor");
		case GateReject_CheckStripPair: return _T("check-strip-pair");
		case GateReject_CheckTilePair: return _T("check-tile-pair");
		case GateReject_DimAllSkipped: return _T("dim-all-skipped");
		default: return _T("unknown");
	}
}

// One log line per rejected file identity. P0-1 stopped memoizing gate
// rejections, so a rejected file is re-attempted on every visit and would
// spam the log without dedup. Keyed by path + size + mtime, like the
// bad-file memo, so an edited file logs again. The set only ever holds
// rejected files, so it stays tiny and is never pruned. Guarded by its
// own CRITICAL_SECTION (same pattern as the memo lock): the two load
// threads can reject concurrently.
struct TiffGateLoggedFile {
	CString sPath;
	__int64 nSize;
	FILETIME ftWrite;
	bool operator<(const TiffGateLoggedFile& other) const {
		int c = sPath.CompareNoCase(other.sPath);
		if (c != 0) return c < 0;
		if (nSize != other.nSize) return nSize < other.nSize;
		if (ftWrite.dwHighDateTime != other.ftWrite.dwHighDateTime)
			return ftWrite.dwHighDateTime < other.ftWrite.dwHighDateTime;
		return ftWrite.dwLowDateTime < other.ftWrite.dwLowDateTime;
	}
};
static std::set<TiffGateLoggedFile> s_tiffGateLogged;
struct CTiffGateLogLockHolder {
	CRITICAL_SECTION cs;
	CTiffGateLogLockHolder() { ::InitializeCriticalSection(&cs); }
	~CTiffGateLogLockHolder() { ::DeleteCriticalSection(&cs); }
};
static CTiffGateLogLockHolder& TiffGateLogLock() { static CTiffGateLogLockHolder lock; return lock; }

static void LogTiffGateRejectionOnce(LPCTSTR sFileName, __int64 nSize, const FILETIME& ftWrite,
	TiffGateRejectReason reason, unsigned long long ifdOff,
	unsigned long long aux1, unsigned long long aux2) {
	TiffGateLoggedFile key;
	key.sPath = sFileName;
	key.nSize = nSize;
	key.ftWrite = ftWrite;
	{
		Helpers::CAutoCriticalSection lock(TiffGateLogLock().cs);
		if (s_tiffGateLogged.find(key) != s_tiffGateLogged.end()) {
			return;
		}
		// Bound the set like the bad-file memo (32 entries). A folder full
		// of rejected files must not grow it without limit; clearing at the
		// cap only costs one extra log line per file afterwards.
		if (s_tiffGateLogged.size() >= 32) {
			s_tiffGateLogged.clear();
		}
		s_tiffGateLogged.insert(key);
	}
	CString sMsg;
	sMsg.Format(_T("JPEGView: TIFF gate rejected \"%s\" reason=%s ifdOff=%I64u aux1=%I64u aux2=%I64u size=%I64d\n"),
		sFileName, TiffGateRejectReasonName(reason), ifdOff, aux1, aux2, nSize);
	::OutputDebugString(sMsg);
}

// Decoder-independent RAW gate. Both libraw and dcraw_mod allocate and read the
// image according to the dimensions the file declares, and neither checks those
// dimensions against the file's real size. A DNG that declares 65535x21846 but
// carries only a few KB reads past its buffer inside them and crashes
// intermittently (observed: 2 crashes in 4 opens). Parse the TIFF IFDs here
// (IFD0, its SubIFDs and the next-IFD chain), before either decoder runs,
// and compare the declared data size with the file size using 64-bit
// arithmetic. Files that are not TIFF-shaped are let through to the
// decoders rather than being mis-classified.
// R-03/R-04/R-05: this gate is a 32-bit TIFF gate, not a general RAW gate.
// It deliberately does NOT cover BigTIFF (magic 43, passed through below)
// or non-TIFF RAW formats (CRW/RAF/X3F and friends, passed through below).
// Those still rely on the decoders' own protection. What it does cover:
// IFD0 plus SubIFDs (tag 330, depth <= 2) AND the "next IFD" chain after
// each IFD's entries (F-01): CollectIFDTree walks the chain iteratively
// with no depth limit - dcraw_mod follows the chain too, so an absurd
// chained IFD must not slip past - bounded instead by the 4096-node cap
// and the visited-offset set (F-H05). Strip (273/279) and tile (324/325)
// arrays are read by seeking to their file offsets instead of from a
// 1 MB prefix, so an IFD past 1 MB is no longer falsely rejected and
// external values past 1 MB are no longer silently skipped. A width/height
// above JPEGView's
// MAX_IMAGE_DIMENSION (65535) is rejected for every IFD visited: the
// viewer cannot display such an image anyway, and the gate only returns a
// bool, so "over the viewer limit" and "corrupt" are intentionally not
// distinguished here.
struct TiffGateArray {
	bool present;
	unsigned long long type;
	unsigned long long count;
	unsigned long long dataOff; // file offset of the first element
	TiffGateArray() : present(false), type(0), count(0), dataOff(0) {}
};

struct TiffGateIFD {
	unsigned long long width;
	unsigned long long height;
	unsigned long long compression;
	unsigned long long samples;
	unsigned long long bits;
	unsigned long long photometric;
	unsigned long long offset; // file offset of this IFD (set by CollectIFDTree; only for the rejection log)
	unsigned long long newSubfileType; // tag 254 (NewSubfileType; bit 0 = reduced-resolution image)
	bool samplesPresent;
	bool bitsPresent;
	bool photometricPresent;
	bool widthPresent;
	bool heightPresent;
	TiffGateArray stripOff;
	TiffGateArray stripCnt;
	TiffGateArray tileOff;
	TiffGateArray tileCnt;
	std::vector<unsigned long long> subIFDs;
	TiffGateIFD() : width(0), height(0), compression(1), samples(0), bits(0), photometric(0), offset(0), newSubfileType(0),
		samplesPresent(false), bitsPresent(false), photometricPresent(false),
		widthPresent(false), heightPresent(false) {}
};

class TiffGateReader {
public:
	TiffGateReader(HANDLE hFile, unsigned long long fileSize, bool little, bool isKdc = false, bool isSrf = false)
		: m_hFile(hFile), m_fileSize(fileSize), m_little(little), m_isKdc(isKdc), m_isSrf(isSrf),
		m_rejectReason(GateReject_None), m_rejectIfdOff(0), m_rejectAux1(0), m_rejectAux2(0) {}

	// Last rejection, for the one-line reason log (P0-2). Only meaningful
	// when CheckIFDTree returned false. A skipped (non-fatal) failure may
	// leave a stale reason behind; every fatal failure site sets it first,
	// so a stale reason is never reported.
	TiffGateRejectReason GetRejectReason() const { return m_rejectReason; }
	unsigned long long GetRejectIfdOff() const { return m_rejectIfdOff; }
	unsigned long long GetRejectAux1() const { return m_rejectAux1; }
	unsigned long long GetRejectAux2() const { return m_rejectAux2; }

	// Seek-read exactly n bytes. Fails closed on any short read, so a
	// truncated file can never yield a half-read value that looks valid.
	bool ReadAt(unsigned long long off, unsigned char* dst, unsigned long long n) const {
		if (off > m_fileSize || n > m_fileSize - off) {
			return false;
		}
		LARGE_INTEGER li;
		li.QuadPart = (LONGLONG)off;
		if (!::SetFilePointerEx(m_hFile, li, NULL, FILE_BEGIN)) {
			return false;
		}
		DWORD got = 0;
		// All callers bound n (IFD <= 65535 * 12 bytes, array chunks <= 4 KB).
		return ::ReadFile(m_hFile, dst, (DWORD)n, &got, NULL) && got == n;
	}

	unsigned long long Rd16(const unsigned char* p) const {
		return m_little ? (p[0] | ((unsigned long long)p[1] << 8))
			: (((unsigned long long)p[0] << 8) | p[1]);
	}

	unsigned long long Rd32(const unsigned char* p) const {
		if (m_little) {
			return p[0] | ((unsigned long long)p[1] << 8) |
				((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24);
		}
		return ((unsigned long long)p[0] << 24) | ((unsigned long long)p[1] << 16) |
			((unsigned long long)p[2] << 8) | p[3];
	}

	static unsigned long long TypeSize(unsigned long long type) {
		switch (type) {
			case 1: case 2: case 6: case 7: return 1;
			case 3: return 2;
			case 4: case 9: case 11: return 4;
			case 5: case 10: return 8;
			default: return 0;
		}
	}

	static const unsigned long long kInvalid = (unsigned long long)-1;

	// First value of a scalar tag at a file offset (inline value fields
	// are addressed by their own file offset too, so this is uniform).
	// Returns 0 when the value cannot be read; callers that care about
	// presence track it separately.
	unsigned long long ReadScalar(unsigned long long dataOff, unsigned long long type) const {
		unsigned long long size = TypeSize(type);
		if (size == 0 || size > 8) {
			return 0;
		}
		unsigned char buf[8];
		if (!ReadAt(dataOff, buf, size)) {
			return 0;
		}
		if (size == 1) {
			return buf[0];
		}
		if (size == 2) {
			return Rd16(buf);
		}
		return Rd32(buf); // LONG/SLONG/FLOAT, and the numerator of RATIONAL
	}

	// One element of an array tag. kInvalid when it cannot be read.
	unsigned long long ReadElement(unsigned long long dataOff, unsigned long long type, unsigned long long j) const {
		unsigned long long size = TypeSize(type);
		if (size == 0) {
			return kInvalid;
		}
		unsigned char buf[8];
		if (!ReadAt(dataOff + j * size, buf, size)) {
			return kInvalid;
		}
		if (size == 1) {
			return buf[0];
		}
		if (size == 2) {
			return Rd16(buf);
		}
		return Rd32(buf);
	}

	// R-16: bits is per-channel; the maximum over all channels, not the
	// first value. Only SHORT/LONG element types are meaningful here.
	bool ReadBitsArray(unsigned long long dataOff, unsigned long long type, unsigned long long count, unsigned long long& maxOut) const {
		if (type != 3 && type != 4) {
			return false;
		}
		unsigned long long size = TypeSize(type);
		std::vector<unsigned char> buf((size_t)(count * size));
		if (!ReadAt(dataOff, &buf[0], count * size)) {
			return false;
		}
		maxOut = 0;
		for (unsigned long long j = 0; j < count; ++j) {
			unsigned long long v = (type == 3) ? Rd16(&buf[(size_t)(j * 2)]) : Rd32(&buf[(size_t)(j * 4)]);
			if (v > maxOut) {
				maxOut = v;
			}
		}
		return true;
	}

	static void FillArray(TiffGateArray& a, unsigned long long type, unsigned long long count, unsigned long long dataOff) {
		// Only SHORT/LONG are valid strip/tile arrays; anything else is
		// left absent so the completeness rule below rejects it when it
		// matters (compressed branch).
		if (type != 3 && type != 4) {
			return;
		}
		a.present = true;
		a.type = type;
		a.count = count;
		a.dataOff = dataOff;
	}

	// Reads one IFD's entries into info. Returns false when the IFD
	// itself is malformed (truncated, unreadable, absurd SubIFD count).
	// Tag values that cannot be read are left missing; CheckIFD decides
	// what a missing value means for each branch.
	bool ReadIFD(unsigned long long ifdOff, TiffGateIFD& info, unsigned long long& nextIFD) const {
		unsigned char countBuf[2];
		if (!ReadAt(ifdOff, countBuf, 2)) {
			SetRejection(GateReject_IfdUnreadable, ifdOff);
			return false;
		}
		unsigned long long n = Rd16(countBuf);
		if (n == 0) {
			SetRejection(GateReject_IfdEmpty, ifdOff);
			return false;
		}
		// n is a uint16, so the entry block is at most 65535 * 12 bytes.
		unsigned long long entriesSize = n * 12;
		if (ifdOff + 2 > m_fileSize || entriesSize > m_fileSize - (ifdOff + 2)) {
			SetRejection(GateReject_IfdUnreadable, ifdOff, n);
			return false;
		}
		std::vector<unsigned char> entries((size_t)entriesSize);
		if (!ReadAt(ifdOff + 2, &entries[0], entriesSize)) {
			SetRejection(GateReject_IfdUnreadable, ifdOff, n);
			return false;
		}
		for (unsigned long long k = 0; k < n; ++k) {
			const unsigned char* e = &entries[(size_t)(k * 12)];
			unsigned long long tag = Rd16(e);
			unsigned long long type = Rd16(e + 2);
			unsigned long long count = Rd32(e + 4);
			// Dimension presence is tracked separately from the value:
			// a dimension tag whose value cannot be read still counts
			// as declared, so CheckIFDTree can tell a pointer block
			// (no dimension tag at all) from a malformed image IFD.
			if (tag == 256) {
				info.widthPresent = true;
			} else if (tag == 257) {
				info.heightPresent = true;
			}
			unsigned long long typeSize = TypeSize(type);
			if (typeSize == 0 || count == 0) {
				continue;
			}
			// Guard against a hostile count overflowing the byte total.
			if (count > (0xFFFFFFFFULL / typeSize)) {
				continue;
			}
			unsigned long long byteCount = typeSize * count;
			unsigned long long dataOff;
			if (byteCount <= 4) {
				// Inline: the value field's own file offset.
				dataOff = ifdOff + 2 + k * 12 + 8;
			} else {
				dataOff = Rd32(e + 8);
				if (dataOff > m_fileSize || byteCount > m_fileSize - dataOff) {
					continue; // external value outside the file: tag missing
				}
			}
			switch (tag) {
				case 254: info.newSubfileType = ReadScalar(dataOff, type); break;
				case 256: info.width = ReadScalar(dataOff, type); break;
				case 257: info.height = ReadScalar(dataOff, type); break;
				case 259: info.compression = ReadScalar(dataOff, type); break;
				case 262:
					info.photometric = ReadScalar(dataOff, type);
					info.photometricPresent = true;
					break;
				case 277:
					info.samples = ReadScalar(dataOff, type);
					info.samplesPresent = true;
					break;
				case 258:
					// R-16: an oversized bits array cannot be a real
					// per-channel list; leave bits missing so the
					// uncompressed verdict below rejects the file.
					if (count <= 1024) {
						unsigned long long m = 0;
						if (ReadBitsArray(dataOff, type, count, m)) {
							info.bits = m;
							info.bitsPresent = true;
						}
					}
					break;
				case 273: FillArray(info.stripOff, type, count, dataOff); break;
				case 279: FillArray(info.stripCnt, type, count, dataOff); break;
				case 324: FillArray(info.tileOff, type, count, dataOff); break;
				case 325: FillArray(info.tileCnt, type, count, dataOff); break;
				case 330:
					// SubIFD offsets. No real camera file lists more
					// than a handful; an absurd count is malformed.
					if (count > 1024 || (type != 3 && type != 4)) {
						SetRejection(GateReject_SubIfdBad, ifdOff, count);
						return false;
					}
					for (unsigned long long j = 0; j < count; ++j) {
						unsigned long long v = ReadElement(dataOff, type, j);
						if (v == kInvalid) {
							SetRejection(GateReject_SubIfdBad, ifdOff, count, j);
							return false;
						}
						info.subIFDs.push_back(v);
					}
					break;
				default: break;
			}
		}
		// F-01: read the next-IFD pointer (4 bytes after the entries).
		// dcraw_mod walks this chain, so the gate must too, or an IFD0
		// that is small and legal can hide an absurd chained IFD1.
		// A truncated file that ends before the pointer has no chain
		// to hide (dcraw stops there as well), so treat it as 0.
		// The pointer VALUE is not trusted: manufacturers do not
		// maintain the chain pointer after a SubIFD, so those 4 bytes
		// are often unrelated data (a D5000 NEF carries 34799619, an
		// A100 ARW 957017457, both past EOF). Only follow a pointer
		// that lands on a structurally plausible IFD - readable entry
		// count, non-zero, whole entry block inside the file (the same
		// checks ReadIFD applies at its entry); anything else ends the
		// chain. A real chained IFD is still visited, so the F-01
		// protection is unchanged.
		nextIFD = 0;
		unsigned char nextBuf[4];
		if (ReadAt(ifdOff + 2 + entriesSize, nextBuf, 4)) {
			unsigned long long candidate = Rd32(nextBuf);
			// Validate the candidate the same way ReadIFD validates
			// its own entry point (see above): a readable, non-zero
			// entry count whose whole block fits in the file.
			unsigned char candCount[2];
			if (candidate != 0 && ReadAt(candidate, candCount, 2)) {
				unsigned long long candN = Rd16(candCount);
				if (candN != 0 && candidate + 2 <= m_fileSize &&
					candN * 12 <= m_fileSize - (candidate + 2)) {
					nextIFD = candidate;
				}
			}
		}
		return true;
	}

	// Plausibility verdict for one parsed IFD. Strip/tile ranges stream
	// both partner arrays in lockstep (R-05: no 512 cap, nothing stored).
	bool CheckIFD(const TiffGateIFD& info) const {
		if (info.width == 0 || info.height == 0 || info.width > 65535 || info.height > 65535) {
			SetRejection(GateReject_CheckDimInvalid, info.offset, info.width, info.height);
			return false;
		}
		// R-02: with these caps the product below cannot overflow 64 bit
		// (65535^2 * 8 * 64 / 8 is far below 2^64).
		if (info.samplesPresent && info.samples > 8) {
			SetRejection(GateReject_CheckSamplesTooMany, info.offset, info.samples);
			return false;
		}
		if (info.bitsPresent && info.bits > 64) {
			SetRejection(GateReject_CheckBitsTooMany, info.offset, info.bits);
			return false;
		}
		unsigned long long declaredPixels = info.width * info.height;
		if (info.compression == 1 || info.compression == 0) {
			// R-16: a missing declaration normally makes the bound
			// meaningless, so the file is rejected instead of passing a
			// weakened check. One exception: single-channel TIFFs
			// legitimately omit SamplesPerPixel (the TIFF spec default
			// is 1), and Kodak DCS monochrome files do exactly that
			// (photometric 1, single strip of exactly pixels*bits/8).
			// The default is trusted only for single-channel
			// photometrics (0 WhiteIsZero, 1 BlackIsZero, 32803 CFA);
			// a colour file that omits samples contradicts its own
			// photometric and still fails, as does any file with bits
			// missing.
			if (!info.bitsPresent) {
				SetRejection(GateReject_CheckBitsMissing, info.offset);
				return false;
			}
			unsigned long long samples = info.samples;
			if (!info.samplesPresent) {
				if (!info.photometricPresent ||
					(info.photometric != 0 && info.photometric != 1 && info.photometric != 32803)) {
					SetRejection(GateReject_CheckSamplesMissing, info.offset, info.photometric);
					return false;
				}
				samples = 1;
			}
			// KDC exemption: Kodak DCS wrapper files declare full-frame
			// dimensions but store a smaller wrapped payload (DC120: 1280x960
			// declares 1,228,800 bytes in an 843,328-byte file, while its
			// actual strip fits exactly to EOF), so the declared-bytes bound
			// false-rejects them. Only the declared-bytes bound is skipped for .kdc; the
			// dimension/sample/bits caps still apply, and LibRaw's own
			// pixels/8 check in RAWWrapper remains as a backstop, so a
			// truncated KDC fails at decode rather than at the gate.
			// SRF exemption: Sony SRF tags are nominal, not the data
			// layout (DSC-F828: 3360x2460 declares 43,394,400 bytes at
			// 3 samples x 14 bits, but the actual strip is 16,531,200
			// bytes = pixels x 2, single-channel 16-bit-aligned sensor
			// data; LibRaw's Sony parser owns the layout), so the
			// declared-bytes bound false-rejects every SRF. Only the
			// declared-bytes bound is skipped for .srf; the
			// dimension/sample/bits caps and the pixels/8 floor below
			// still apply.
			unsigned long long declaredBytes = declaredPixels * samples * info.bits / 8;
			if (!m_isKdc && !m_isSrf && declaredBytes > m_fileSize) {
				SetRejection(GateReject_CheckDeclaredBytes, info.offset, declaredBytes, m_fileSize);
				return false;
			}
			// F-H06: the pixels/8 floor applies even for .kdc. It is very
			// loose for genuine uncompressed data (declaredBytes is
			// pixels*samples*bits/8, far above pixels/8), so it cannot
			// false-reject a real KDC, but it stops a renamed TIFF from
			// using the .kdc exemption to smuggle an absurdly small file
			// past the gate.
			if (m_fileSize < declaredPixels / 8) {
				SetRejection(GateReject_CheckPixelsFloor, info.offset, declaredPixels, m_fileSize);
				return false;
			}
		} else {
			// JPEG floor exemption: JPEG routinely compresses below 1 bit
			// per pixel (DC120_WITH_JPEG: 135,688 bytes for 1280x960 =
			// 0.88 bpp), so the pixels/8 floor false-rejects real JPEG
			// RAWs. A truncated JPEG fails in the decoder with an error,
			// unlike raw unpack over-reads, so skipping the floor for
			// compression 7 loses no crash protection. Non-JPEG
			// compressed data keeps the floor.
			if (info.compression != 7 && m_fileSize < declaredPixels / 8) {
				SetRejection(GateReject_CheckPixelsFloor, info.offset, declaredPixels, m_fileSize);
				return false;
			}
			// JPEG-7 exemption: Hasselblad 3FR JPEG strips carry the nominal
			// uncompressed byte count (CFV: 33,554,432), not the actual JPEG
			// stream size, so the strip-range check false-rejects every
			// JPEG-compressed RAW. The pixels/8 floor above still rejects
			// absurdly small files.
			if (info.compression != 7) {
				if (!CheckArrayPair(info.stripOff, info.stripCnt)) {
					SetRejection(GateReject_CheckStripPair, info.offset, info.stripOff.count, info.stripCnt.count);
					return false;
				}
				if (!CheckArrayPair(info.tileOff, info.tileCnt)) {
					SetRejection(GateReject_CheckTilePair, info.offset, info.tileOff.count, info.tileCnt.count);
					return false;
				}
			}
		}
		return true;
	}

	bool CheckArrayPair(const TiffGateArray& offs, const TiffGateArray& cnts) const {
		// R-10, applied to strips and tiles alike: both absent is fine
		// (then only the pixels/8 bound applies); anything else must be
		// complete and matched, or the file is rejected.
		if (!offs.present && !cnts.present) {
			return true;
		}
		if (offs.present != cnts.present || offs.count != cnts.count) {
			return false;
		}
		const unsigned long long kChunk = 1024;
		std::vector<unsigned char> bufOff;
		std::vector<unsigned char> bufCnt;
		for (unsigned long long base = 0; base < offs.count; base += kChunk) {
			unsigned long long chunk = offs.count - base;
			if (chunk > kChunk) {
				chunk = kChunk;
			}
			unsigned long long offSize = chunk * TypeSize(offs.type);
			unsigned long long cntSize = chunk * TypeSize(cnts.type);
			bufOff.assign((size_t)offSize, 0);
			bufCnt.assign((size_t)cntSize, 0);
			if (!ReadAt(offs.dataOff + base * TypeSize(offs.type), &bufOff[0], offSize)) {
				return false;
			}
			if (!ReadAt(cnts.dataOff + base * TypeSize(cnts.type), &bufCnt[0], cntSize)) {
				return false;
			}
			for (unsigned long long j = 0; j < chunk; ++j) {
				unsigned long long o = (offs.type == 3)
					? Rd16(&bufOff[(size_t)(j * 2)])
					: Rd32(&bufOff[(size_t)(j * 4)]);
				unsigned long long c = (cnts.type == 3)
					? Rd16(&bufCnt[(size_t)(j * 2)])
					: Rd32(&bufCnt[(size_t)(j * 4)]);
				if (o > m_fileSize || c > m_fileSize - o) {
					return false;
				}
			}
		}
		return true;
	}

	// Largest-IFD verdict: the tree is collected first, then only the
	// largest IFD that declares dimensions gets the full CheckIFD verdict.
	// A thumbnail/preview IFD must not veto the full image: the Ricoh GR2
	// DNG's 160x120 thumbnail IFD has a BitsPerSample offset pointing at
	// "Canon " text, so its bits read as 28271, while the 3656x2744 full
	// IFD is perfectly plausible and LibRaw decodes it. Dimension bounds
	// and dimension pairing are checked for every IFD, except that an IFD
	// with explicit preview/thumbnail evidence (NewSubfileType bit 0 or
	// JPEG compression) and bad dimensions is skipped instead of vetoing
	// the file (converged Fix 1); without that evidence a malformed
	// dimension hidden in any SubIFD still fails closed. Only
	// the per-channel/strip detail verdict is limited to the largest IFD,
	// whose declarations govern the full decode. An IFD declaring neither
	// dimension is a pointer block (NEF JpgFromRaw, NRW IFD0) and skips
	// the verdict, as before, but its SubIFDs are still walked. Depth
	// <= 2, as before.
	// F-H05: total-node cap + visited-offset set. Without them a tiny
	// crafted TIFF can force ~1,049,601 nodes (1024 SubIFDs each pointing
	// at an IFD with 1024 more). A shared offset is walked once; exceeding
	// the cap fails the gate (fail closed), it never crashes.
	// F-01: the next-IFD chain is walked too, but only at the top
	// level (depth 0; no depth limit for the chain, the cap and
	// visited set bound it), because dcraw_mod's parse_tiff loops on
	// the next pointer and an absurd chained IFD must not slip past.
	// A SubIFD's chain is never followed: dcraw's tag 330 handling
	// parses each SubIFD once (parse_tiff_ifd) and never reads its
	// next pointer.
	// Fix 2: a broken IFD is skipped, not fatal - except IFD0 itself
	// and except the node cap. dcraw_mod's parse_tiff loops the chain
	// and breaks on a bad IFD (dcraw_mod.cpp:6325-6327: "while
	// ((doff = get4())) { ... if (parse_tiff_ifd (base)) break; }"),
	// and its tag 330 handling parses each SubIFD once and breaks out
	// of the SubIFD loop on failure while the outer IFD continues
	// (dcraw_mod.cpp:5984-5987). So a bad chain IFD ends the chain and
	// a bad SubIFD is skipped; only IFD0 (cur == ifdOff) still rejects
	// the file, and the 4096-node cap still fails closed (Fix 8
	// territory, explicitly not done): the cap is a separate result,
	// never confused with an unreadable IFD.
	TiffGateCollectResult CollectIFDTree(unsigned long long ifdOff, int depth, std::vector<TiffGateIFD>& out, std::vector<unsigned long long>& visited) const {
		// F-01: walk the next-IFD chain as well as the SubIFD tree.
		// dcraw_mod's parse_tiff loops on the next pointer, so a chain
		// IFD that declares absurd dimensions would otherwise bypass
		// the gate, but only at the top level (depth 0). A SubIFD's
		// next pointer is never followed: dcraw's tag 330 handling
		// parses each SubIFD once (parse_tiff_ifd) and never reads it.
		// The chain is walked iteratively with no depth limit (a
		// multi-page TIFF can chain many IFDs); the 4096-node cap and
		// the visited set bound it. SubIFD nesting keeps the depth
		// <= 2 limit.
		unsigned long long cur = ifdOff;
		while (cur != 0) {
			for (size_t i = 0; i < visited.size(); ++i) {
				if (visited[i] == cur) {
					return TiffGateCollect_Ok; // shared SubIFD / chain link already walked
				}
			}
			if (out.size() >= 4096) {
				SetRejection(GateReject_NodeCapExceeded, cur, (unsigned long long)out.size());
				return TiffGateCollect_CapExceeded;
			}
			visited.push_back(cur);
			TiffGateIFD info;
			unsigned long long nextIFD = 0;
			if (!ReadIFD(cur, info, nextIFD)) {
				// ReadIFD already recorded the reason (which check,
				// which offset). IFD0 itself is fatal; a later chain
				// IFD just ends the chain, exactly like dcraw's break
				// in parse_tiff (dcraw_mod.cpp:6325-6327).
				if (cur == ifdOff) {
					return TiffGateCollect_BadIfd;
				}
				break;
			}
			info.offset = cur;
			out.push_back(info);
			if (depth < 2) {
				for (size_t i = 0; i < info.subIFDs.size(); ++i) {
					unsigned long long sub = info.subIFDs[i];
					if (sub == 0) {
						continue; // 0 means "no SubIFD" for this entry
					}
					TiffGateCollectResult subResult = CollectIFDTree(sub, depth + 1, out, visited);
					if (subResult == TiffGateCollect_CapExceeded) {
						return TiffGateCollect_CapExceeded;
					}
					// BadIfd: skip that subtree and continue with the
					// next SubIFD, like dcraw's tag 330 loop breaking
					// out of the SubIFD loop while the outer IFD
					// continues (dcraw_mod.cpp:5984-5987). The reason
					// left behind by the failed subtree is stale and is
					// never reported: it is only read when CheckIFDTree
					// returns false, and every fatal failure site sets
					// it first (see SetRejection).
				}
			}
			// Only the top-level chain is followed (depth 0). A SubIFD's
			// next pointer is never read by dcraw (tag 330 handling
			// parses each SubIFD once), and manufacturers do not maintain
			// it, so those 4 bytes are often unrelated data: a D60 NEF
			// carries 197139 there, pointing at raw image bytes that
			// parse as a 22627-entry "IFD" with insane dimensions and
			// veto the file. Following it exceeds what dcraw decodes.
			cur = (depth == 0) ? nextIFD : 0;
		}
		return TiffGateCollect_Ok;
	}

	bool CheckIFDTree(unsigned long long ifdOff, int depth) const {
		std::vector<TiffGateIFD> all;
		std::vector<unsigned long long> visited;
		if (CollectIFDTree(ifdOff, depth, all, visited) != TiffGateCollect_Ok) {
			return false;
		}
		const TiffGateIFD* largest = NULL;
		unsigned long long largestPixels = 0;
		// Converged Fix 1: whether any IFD declared dimensions at all.
		bool anyDeclared = false;
		unsigned long long skippedDimIfds = 0;
		for (size_t i = 0; i < all.size(); ++i) {
			const TiffGateIFD& info = all[i];
			if (info.widthPresent || info.heightPresent) {
				anyDeclared = true;
			}
			// Dimensions must come in pairs, and a declared dimension must
			// be sane. A present-but-unreadable dimension reads as 0 and
			// therefore fails here, as before.
			// A dimension above MAX_IMAGE_DIMENSION is the hard upper
			// bound and is never skippable, not even with thumbnail
			// evidence: a decoder that adopts those dimensions would
			// allocate and read far beyond anything the file can hold.
			// Only a missing or zero dimension can be a malformed
			// thumbnail, so only that may be skipped below.
			bool dimTooBig = (info.widthPresent && info.width > 65535)
				|| (info.heightPresent && info.height > 65535);
			bool badDim = (info.widthPresent != info.heightPresent)
				|| (info.widthPresent && info.width == 0)
				|| (info.heightPresent && info.height == 0)
				|| dimTooBig;
			if (badDim) {
				// Converged Fix 1: skip this IFD only with explicit
				// preview/thumbnail evidence: NewSubfileType bit 0
				// (tag 254, reduced-resolution image) or JPEG
				// compression (tag 259 == 6/7). Without either, the
				// rejection stands exactly as before.
				// KNOWN RESIDUAL (accepted): a JPEG-compressed main-image
				// IFD with a missing or zero dimension is skipped too, so
				// the gate then validates the thumbnail while the
				// actually-decoded image's dimensions go unchecked. An
				// over-limit dimension is NOT part of that residual: it
				// rejects above, unconditionally.
				if (!dimTooBig && ((info.newSubfileType & 1) != 0 || info.compression == 6 || info.compression == 7)) {
					++skippedDimIfds;
					continue;
				}
				TiffGateRejectReason reason;
				if (dimTooBig) {
					reason = GateReject_DimTooBig;
				} else if (info.widthPresent != info.heightPresent) {
					reason = GateReject_DimUnpaired;
				} else {
					reason = GateReject_DimZero;
				}
				SetRejection(reason, info.offset, info.width, info.height);
				return false;
			}
			if (info.widthPresent) {
				// Both dimensions are present and bounded (see above), so
				// this product cannot overflow 64 bit (65535^2 < 2^32).
				unsigned long long pixels = info.width * info.height;
				if (largest == NULL || pixels > largestPixels) {
					largest = &all[i];
					largestPixels = pixels;
				}
			}
		}
		if (largest == NULL) {
			if (anyDeclared) {
				// Dimensions were declared, but every declaring IFD was
				// skipped as a thumbnail: no plausible image IFD is left
				// to validate, so the file is rejected.
				SetRejection(GateReject_DimAllSkipped, 0, skippedDimIfds);
				return false;
			}
			return true; // no IFD declares dimensions: pointer blocks only
		}
		return CheckIFD(*largest);
	}

private:
	// Records why the gate rejected the file. Const methods only: the
	// reader is built per IsPlausibleRawFile call and never shared
	// between threads, so mutable members are safe here.
	void SetRejection(TiffGateRejectReason reason, unsigned long long ifdOff, unsigned long long aux1 = 0, unsigned long long aux2 = 0) const {
		m_rejectReason = reason;
		m_rejectIfdOff = ifdOff;
		m_rejectAux1 = aux1;
		m_rejectAux2 = aux2;
	}

	HANDLE m_hFile;
	unsigned long long m_fileSize;
	bool m_little;
	bool m_isKdc;
	bool m_isSrf;
	mutable TiffGateRejectReason m_rejectReason;
	mutable unsigned long long m_rejectIfdOff;
	mutable unsigned long long m_rejectAux1;
	mutable unsigned long long m_rejectAux2;
};

static bool IsPlausibleRawFile(LPCTSTR fileName) {
	HANDLE hFile = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		// No handle, so no size/mtime for the dedup key: path-only.
		FILETIME ftZero = { 0, 0 };
		LogTiffGateRejectionOnce(fileName, 0, ftZero, GateReject_OpenFailed, 0, 0, 0);
		return false;
	}
	// mtime for the log dedup key (path + size + mtime, like the memo).
	FILETIME ftWrite = { 0, 0 };
	::GetFileTime(hFile, NULL, NULL, &ftWrite);
	LARGE_INTEGER liSize;
	if (!::GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0) {
		::CloseHandle(hFile);
		LogTiffGateRejectionOnce(fileName, 0, ftWrite, GateReject_SizeInvalid, 0, 0, 0);
		return false;
	}
	unsigned long long fileSize = (unsigned long long)liSize.QuadPart;
	unsigned char header[8];
	DWORD got = 0;
	if (!::ReadFile(hFile, header, 8, &got, NULL) || got != 8) {
		::CloseHandle(hFile);
		LogTiffGateRejectionOnce(fileName, fileSize, ftWrite, GateReject_HeaderReadFailed, 0, 0, 0);
		return false;
	}

	bool little;
	if (header[0] == 'I' && header[1] == 'I') {
		little = true;
	} else if (header[0] == 'M' && header[1] == 'M') {
		little = false;
	} else {
		::CloseHandle(hFile);
		return true; // not TIFF, leave it to the decoders
	}

	// Byte-order-aware magic, read straight from the header bytes.
	unsigned long long magic = little ? (header[2] | ((unsigned long long)header[3] << 8))
		: (((unsigned long long)header[2] << 8) | header[3]);
	if (magic != 42) {
		::CloseHandle(hFile);
		return true; // not a standard TIFF (includes BigTIFF, magic 43)
	}
	unsigned long long ifdOff = little
		? (header[4] | ((unsigned long long)header[5] << 8) |
			((unsigned long long)header[6] << 16) | ((unsigned long long)header[7] << 24))
		: (((unsigned long long)header[4] << 24) | ((unsigned long long)header[5] << 16) |
			((unsigned long long)header[6] << 8) | header[7]);
	if (ifdOff == 0) {
		::CloseHandle(hFile);
		LogTiffGateRejectionOnce(fileName, fileSize, ftWrite, GateReject_IfdOffsetZero, 0, 0, 0);
		return false; // claims to be TIFF but has no IFD
	}

	// KDC/SRF signal: Kodak DCS files are plain MM TIFFs, so the extension
	// (basename dot only, as in Helpers::GetImageFormat) is the only
	// reliable marker for the wrapper/nominal-tags exemptions in CheckIFD.
	bool isKdc = false;
	bool isSrf = false;
	{
		LPCTSTR base = _tcsrchr(fileName, _T('\\'));
		LPCTSTR baseAlt = _tcsrchr(fileName, _T('/'));
		if (baseAlt != NULL && (base == NULL || baseAlt > base)) base = baseAlt;
		base = (base == NULL) ? fileName : (base + 1);
		LPCTSTR dot = _tcsrchr(base, _T('.'));
		isKdc = (dot != NULL && _tcsicmp(dot + 1, _T("kdc")) == 0);
		isSrf = (dot != NULL && _tcsicmp(dot + 1, _T("srf")) == 0);
	}
	TiffGateReader reader(hFile, fileSize, little, isKdc, isSrf);
	bool ok = reader.CheckIFDTree(ifdOff, 0);
	::CloseHandle(hFile);
	// A non-TIFF pass-through returns true above and is never logged.
	if (!ok) {
		LogTiffGateRejectionOnce(fileName, fileSize, ftWrite, reader.GetRejectReason(),
			reader.GetRejectIfdOff(), reader.GetRejectAux1(), reader.GetRejectAux2());
	}
	return ok;
}

void CImageLoadThread::ProcessReadRAWRequest(CRequest * request) {
	bool bOutOfMemory = false;
	bool bSizeLimitExceeded = false; // R-04/P2-5: set by RawReader, mapped to the request below
	try {
		if (!IsPlausibleRawFile(request->FileName)) {
			// P0: a gate rejection is not a decode failure (no decoder
			// ran), so it must not be memoized as a bad file - see the
			// memo condition in Process(). This covers both gate entry
			// points: the IF_CameraRAW switch case and the TIFF fallback
			// in ProcessReadGDIPlusRequest, which both come through here.
			request->GateRejected = true;
			return;
		}
		// One-shot full-size reload (IDM_RELOAD_FULLSIZE): this request ignores
		// DisplayFullSizeRAW and uses mode 3 semantics (full size first, thumb
		// fallback), so a thumbnail-mode user can view the current image in
		// full size without changing the configured DisplayFullSizeRAW setting.
		// The flag lives on the request, so navigation and read-ahead requests
		// keep the configured mode. "Settings" here means the configured mode
		// only: the per-image processing parameters (gamma, rotation, etc.)
		// are intentionally reset for this view, because the full-size reload
		// shows the RAW original as-is (see CMainDlg::ReloadImageFullSize).
		int fullsize = request->ForceFullSizeRAW ? 3 : CSettingsProvider::This().DisplayFullSizeRAW();

#ifndef WINXP
		// Try with libraw
		CErrorModeGuard errorModeGuard;
		try {
			if (fullsize == 2 || fullsize == 3) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, bSizeLimitExceeded, fullsize == 2);
			}
			if (request->Image == NULL && fullsize == 2) {
				request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
			}
			if (request->Image == NULL) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, bSizeLimitExceeded, fullsize == 0 || fullsize == 3);
			}
		} catch (...) {
			// libraw.dll not found or VC++ Runtime not installed
		}
#else
		fullsize = fullsize == 1;
#endif

		// Try with dcraw_mod
		if (request->Image == NULL && fullsize != 1 && fullsize != 2) {
			request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
		}
	} catch (...) {
		delete request->Image;
		request->Image = NULL;
	}
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	}
	if (bSizeLimitExceeded) {
		request->SizeLimitExceeded = true;
	}
}

// TIFF WhitePoint (318) / PrimaryChromaticities (319) sanitization.
//
// A TIFF can carry perfectly valid primaries (e.g. sRGB: white 0.3127/0.3290,
// primaries 0.64/0.33, 0.30/0.60, 0.15/0.06) encoded with a huge denominator
// such as 0xFFFFFFFF instead of the usual 10000. Interpreted as unsigned
// rationals the values are correct, but a reader that treats the numerator
// or denominator as signed 32-bit sees negative primaries for any value
// above INT_MAX (e.g. 2748779007 for red-x) and converts colours wrong:
// that is the red/magenta cast on the GDI+ TIFF path for
// sample_1280x853.tiff, whose stripped copy displays normally. LibRaw
// cannot rescue this file (it rejects a plain RGB TIFF as "not RAW"),
// so the fix is to normalise the encoding before GDI+ sees it: any
// 318/319 rational above INT_MAX is rewritten in place, in a memory copy
// only, to denominator 10000 with the numerator rounded (3127/10000,
// 6400/10000, ...), which is the standard precision for these tags.
// Only tags that are normalisable are touched: type must be RATIONAL,
// counts must be 2 (318) / 6 (319), denominators must be non-zero and
// every value must lie in [0,1] as an unsigned rational. Anything else
// (wrong type/count, denominator 0, value > 1, out-of-bounds offset,
// more than 32 IFDs in the chain) aborts sanitization and the original
// file is loaded exactly as before, so this never turns a readable file
// into an unreadable one. Normal tags (denominator 10000 and friends)
// are left byte-identical, so normal TIFFs keep exact GDI+ behaviour.
static unsigned short TiffU16(const unsigned char* p, bool bBigEndian) {
	return bBigEndian ? (unsigned short)(((unsigned short)p[0] << 8) | p[1]) : (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
}
static unsigned int TiffU32(const unsigned char* p, bool bBigEndian) {
	return bBigEndian ? ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3] : (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static void TiffPutU32(unsigned char* p, bool bBigEndian, unsigned int v) {
	if (bBigEndian) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
	else { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
}
static bool TiffReadAtFile(HANDLE hFile, unsigned __int64 nOffset, unsigned char* pBuf, size_t nLen) {
	LARGE_INTEGER li;
	li.QuadPart = (LONGLONG)nOffset;
	if (!::SetFilePointerEx(hFile, li, NULL, FILE_BEGIN)) return false;
	DWORD nRead = 0;
	if (!::ReadFile(hFile, pBuf, (DWORD)nLen, &nRead, NULL)) return false;
	return nRead == nLen;
}
// F-20/E12: TIFF 318/319 probe and sanitizer, both walking SubIFDs.
//
// F-20: the old versions only followed the next-IFD chain, so 318/319 tags
// inside a SubIFD were never probed or cleaned. Both walks below recurse into
// SubIFD (tag 330) with the same shape as the Batch 2 gate's CollectIFDTree:
// depth <= 2 for SubIFD nesting, a visited-offset set, and a 32-IFD total cap
// (the old code already stopped its chain walk at 32 IFDs).
// E12: the old probe did a seek+read syscall pair per 12-byte entry. Each
// IFD's entry block is now read in one bulk read. The probe conclusion is also
// cached in the load memo (E6's cache - one cache for both), keyed by path +
// size + mtime, so a repeat open of the same TIFF skips the probe walk.
// E12 (batch 6): the probe also hands its open handle to the sanitizer, so
// the whole-file cleanup read reuses that open instead of opening the file
// a second time.

// Reads one SubIFD offset out of a tag-330 entry. The entry bytes are in hand
// (bulk-read entry block); external values are read from the file. Returns
// false when the value cannot be read.
static bool TiffSubIFDOffset(HANDLE hFile, const unsigned char* pEntry, bool bBE, unsigned __int64 nFileSize, unsigned int j, unsigned __int64& nSubIFD) {
	unsigned short nType = TiffU16(pEntry + 2, bBE);
	unsigned int nCount = TiffU32(pEntry + 4, bBE);
	unsigned __int64 typeSize = (nType == 3) ? 2 : 4;
	unsigned __int64 byteCount = typeSize * nCount;
	if (byteCount <= 4) {
		// Inline: the offsets live in the entry's own value field.
		if (nType == 3) {
			nSubIFD = TiffU16(pEntry + 8 + j * 2, bBE);
		} else {
			nSubIFD = TiffU32(pEntry + 8, bBE);
		}
		return true;
	}
	unsigned int nValOff = TiffU32(pEntry + 8, bBE);
	if ((unsigned __int64)nValOff + (unsigned __int64)j * typeSize + typeSize > nFileSize) {
		return false;
	}
	unsigned char buf[4];
	if (!TiffReadAtFile(hFile, nValOff + (unsigned __int64)j * typeSize, buf, (size_t)typeSize)) {
		return false;
	}
	nSubIFD = (nType == 3) ? TiffU16(buf, bBE) : TiffU32(buf, bBE);
	return true;
}

// Walks one IFD for the 318/319 probe: bulk-reads the entry block, checks any
// 318/319 rationals, recurses into SubIFDs (depth <= 2), then follows the
// next-IFD chain at the same depth. Returns false when the walk is malformed
// (fail closed: no sanitization, the original file loads). bSuspicious is set
// when a 318/319 rational above INT_MAX is found anywhere in the walk.
static bool TiffProbeWalk(HANDLE hFile, unsigned __int64 nIFD, bool bBE, unsigned __int64 nFileSize,
		int nDepth, unsigned __int64* pVisited, int& nVisitedCount, bool& bSuspicious) {
	if (nVisitedCount >= 32) {
		return true; // cap reached: stop, keep what was found (the old loop stopped at 32 too)
	}
	for (int i = 0; i < nVisitedCount; i++) {
		if (pVisited[i] == nIFD) {
			return true; // shared IFD: already checked
		}
	}
	pVisited[nVisitedCount++] = nIFD;
	if (nIFD < 8 || nIFD + 2 > nFileSize) {
		return false;
	}
	unsigned char cntBuf[2];
	if (!TiffReadAtFile(hFile, nIFD, cntBuf, 2)) {
		return false;
	}
	unsigned short nEntries = TiffU16(cntBuf, bBE);
	if (nEntries > 1024) {
		return false;
	}
	if (nIFD + 2 + (unsigned __int64)nEntries * 12 + 4 > nFileSize) {
		return false;
	}
	// E12: one bulk read for the whole entry block, not a seek+read per entry.
	unsigned char* pEntries = new(std::nothrow) unsigned char[(size_t)nEntries * 12];
	if (pEntries == NULL) {
		return false;
	}
	if (!TiffReadAtFile(hFile, nIFD + 2, pEntries, (size_t)nEntries * 12)) {
		delete[] pEntries;
		return false;
	}
	std::vector<unsigned __int64> subIFDs;
	bool bOk = true;
	for (unsigned short i = 0; i < nEntries && bOk; i++) {
		unsigned char* pEntry = pEntries + (size_t)i * 12;
		unsigned short nTag = TiffU16(pEntry, bBE);
		if (nTag == 330) {
			// F-20: SubIFD offsets for recursion after this IFD's tags.
			unsigned short nType = TiffU16(pEntry + 2, bBE);
			unsigned int nCount = TiffU32(pEntry + 4, bBE);
			if ((nType != 3 && nType != 4) || nCount == 0 || nCount > 1024) {
				bOk = false;
				break;
			}
			for (unsigned int j = 0; j < nCount; j++) {
				unsigned __int64 nSub = 0;
				if (!TiffSubIFDOffset(hFile, pEntry, bBE, nFileSize, j, nSub)) {
					bOk = false;
					break;
				}
				subIFDs.push_back(nSub);
			}
			continue;
		}
		if (nTag != 318 && nTag != 319) {
			continue;
		}
		unsigned short nType = TiffU16(pEntry + 2, bBE);
		unsigned int nCount = TiffU32(pEntry + 4, bBE);
		if (nType != 5) {
			bOk = false;
			break;
		}
		if ((nTag == 318 && nCount != 2) || (nTag == 319 && nCount != 6)) {
			bOk = false;
			break;
		}
		unsigned int nValOff = TiffU32(pEntry + 8, bBE);
		if ((unsigned __int64)nValOff + (unsigned __int64)nCount * 8 > nFileSize) {
			bOk = false;
			break;
		}
		unsigned char rats[48];
		if (!TiffReadAtFile(hFile, nValOff, rats, (size_t)nCount * 8)) {
			bOk = false;
			break;
		}
		for (unsigned int j = 0; j < nCount; j++) {
			unsigned int nNum = TiffU32(rats + j * 8, bBE);
			unsigned int nDen = TiffU32(rats + j * 8 + 4, bBE);
			if (nDen == 0 || nNum > nDen) {
				bOk = false;
				break;
			}
			if (nNum > 0x7FFFFFFF || nDen > 0x7FFFFFFF) {
				bSuspicious = true;
			}
		}
	}
	delete[] pEntries;
	if (!bOk) {
		return false;
	}
	// F-20: recurse into SubIFDs (depth <= 2, as in the gate).
	if (nDepth < 2) {
		for (size_t s = 0; s < subIFDs.size(); s++) {
			if (subIFDs[s] == 0) {
				continue; // 0 means "no SubIFD" for this entry
			}
			if (!TiffProbeWalk(hFile, subIFDs[s], bBE, nFileSize, nDepth + 1, pVisited, nVisitedCount, bSuspicious)) {
				return false;
			}
		}
	}
	unsigned char nextBuf[4];
	if (!TiffReadAtFile(hFile, nIFD + 2 + (unsigned __int64)nEntries * 12, nextBuf, 4)) {
		return false;
	}
	unsigned int nNext = TiffU32(nextBuf, bBE);
	if (nNext == 0) {
		return true;
	}
	if (nNext == nIFD) {
		return false;
	}
	return TiffProbeWalk(hFile, nNext, bBE, nFileSize, nDepth, pVisited, nVisitedCount, bSuspicious);
}

// E12 (batch 6): on a "needs sanitizing" verdict the probe hands its open
// handle to the caller through hFileOut (the caller closes it); on any other
// verdict hFileOut is NULL and the probe closes its handle itself.
static bool TiffChromaticityNeedsSanitization(LPCTSTR sFileName, HANDLE& hFileOut) {
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	hFileOut = NULL;
	if (hFile == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER liSize;
	bool bResult = false;
	if (::GetFileSizeEx(hFile, &liSize) && liSize.QuadPart >= 8) {
		// E12: reuse a previous probe conclusion for this exact file.
		FILETIME ftWrite = { 0, 0 };
		__int64 nSize = liSize.QuadPart;
		bool bIdentityKnown = ::GetFileTime(hFile, NULL, NULL, &ftWrite) != 0;
		if (bIdentityKnown) {
			LoadMemoEntry memo;
			if (LoadMemoLookup(sFileName, nSize, ftWrite, memo) && memo.bProbeKnown) {
				// E12: hand the probe handle to the caller when the verdict
				// is "sanitize", so the cleanup read reuses this open instead
				// of opening the file a second time. On "no sanitize" the
				// handle is closed here as before.
				if (memo.bProbeNeedsSanitize) {
					hFileOut = hFile;
					return true;
				}
				::CloseHandle(hFile);
				return false;
			}
		}
		unsigned char header[8];
		if (TiffReadAtFile(hFile, 0, header, 8)) {
			bool bBig = (header[0] == 'M' && header[1] == 'M');
			bool bLittle = (header[0] == 'I' && header[1] == 'I');
			if (bBig || bLittle) {
				bool bBE = bBig;
				if (TiffU16(header + 2, bBE) == 42) {
					unsigned __int64 nIFD = TiffU32(header + 4, bBE);
					unsigned __int64 visited[32];
					int nVisitedCount = 0;
					bool bSuspicious = false;
					bResult = TiffProbeWalk(hFile, nIFD, bBE, (unsigned __int64)liSize.QuadPart, 0, visited, nVisitedCount, bSuspicious) && bSuspicious;
				}
			}
		}
		if (bIdentityKnown) {
			LoadMemoRecord(sFileName, nSize, ftWrite, false, true, bResult);
		}
	}
	// E12: same transfer as the memo-hit path above: on "sanitize" the
	// caller takes over this handle; otherwise close it here.
	if (bResult) {
		hFileOut = hFile;
	} else {
		::CloseHandle(hFile);
	}
	return bResult;
}

// In-memory twin of TiffSubIFDOffset (see above): reads one SubIFD offset out
// of a tag-330 entry, from the buffer instead of the file.
static bool TiffSubIFDOffsetInMemory(const unsigned char* pBuffer, size_t nSize, const unsigned char* pEntry, bool bBE, unsigned int j, unsigned __int64& nSubIFD) {
	unsigned short nType = TiffU16(pEntry + 2, bBE);
	unsigned int nCount = TiffU32(pEntry + 4, bBE);
	unsigned __int64 typeSize = (nType == 3) ? 2 : 4;
	unsigned __int64 byteCount = typeSize * nCount;
	if (byteCount <= 4) {
		if (nType == 3) {
			nSubIFD = TiffU16(pEntry + 8 + j * 2, bBE);
		} else {
			nSubIFD = TiffU32(pEntry + 8, bBE);
		}
		return true;
	}
	unsigned int nValOff = TiffU32(pEntry + 8, bBE);
	if ((unsigned __int64)nValOff + (unsigned __int64)j * typeSize + typeSize > nSize) {
		return false;
	}
	if (nType == 3) {
		nSubIFD = TiffU16(pBuffer + nValOff + j * 2, bBE);
	} else {
		nSubIFD = TiffU32(pBuffer + nValOff + j * 4, bBE);
	}
	return true;
}

// In-memory twin of TiffProbeWalk: sanitizes 318/319 rationals in place,
// recursing into SubIFDs (F-20) and following the next-IFD chain. Same
// fail-closed semantics: any malformed entry aborts the whole sanitization
// (returns false) and the caller loads the original file.
static bool TiffSanitizeWalk(unsigned char* pBuffer, size_t nSize, unsigned __int64 nIFD, bool bBE,
		int nDepth, unsigned __int64* pVisited, int& nVisitedCount, bool& bSuspicious) {
	if (nVisitedCount >= 32) {
		return true; // cap reached: stop, keep what was sanitized (as the old loop did)
	}
	for (int i = 0; i < nVisitedCount; i++) {
		if (pVisited[i] == nIFD) {
			return true; // shared IFD: already sanitized
		}
	}
	pVisited[nVisitedCount++] = nIFD;
	if (nIFD < 8 || nIFD + 2 > nSize) {
		return false;
	}
	unsigned short nEntries = TiffU16(pBuffer + (size_t)nIFD, bBE);
	if (nEntries > 1024) {
		return false;
	}
	if (nIFD + 2 + (unsigned __int64)nEntries * 12 + 4 > nSize) {
		return false;
	}
	std::vector<unsigned __int64> subIFDs;
	for (unsigned short i = 0; i < nEntries; i++) {
		unsigned char* pEntry = pBuffer + (size_t)nIFD + 2 + (size_t)i * 12;
		unsigned short nTag = TiffU16(pEntry, bBE);
		if (nTag == 330) {
			unsigned short nType = TiffU16(pEntry + 2, bBE);
			unsigned int nCount = TiffU32(pEntry + 4, bBE);
			if ((nType != 3 && nType != 4) || nCount == 0 || nCount > 1024) {
				return false;
			}
			for (unsigned int j = 0; j < nCount; j++) {
				unsigned __int64 nSub = 0;
				if (!TiffSubIFDOffsetInMemory(pBuffer, nSize, pEntry, bBE, j, nSub)) {
					return false;
				}
				subIFDs.push_back(nSub);
			}
			continue;
		}
		if (nTag != 318 && nTag != 319) {
			continue;
		}
		unsigned short nType = TiffU16(pEntry + 2, bBE);
		unsigned int nCount = TiffU32(pEntry + 4, bBE);
		if (nType != 5) {
			return false;
		}
		if ((nTag == 318 && nCount != 2) || (nTag == 319 && nCount != 6)) {
			return false;
		}
		unsigned int nValOff = TiffU32(pEntry + 8, bBE);
		if ((unsigned __int64)nValOff + (unsigned __int64)nCount * 8 > nSize) {
			return false;
		}
		bool bTagSuspicious = false;
		for (unsigned int j = 0; j < nCount; j++) {
			unsigned int nNum = TiffU32(pBuffer + nValOff + j * 8, bBE);
			unsigned int nDen = TiffU32(pBuffer + nValOff + j * 8 + 4, bBE);
			if (nDen == 0 || nNum > nDen) {
				return false;
			}
			if (nNum > 0x7FFFFFFF || nDen > 0x7FFFFFFF) {
				bTagSuspicious = true;
			}
		}
		if (bTagSuspicious) {
			bSuspicious = true;
			for (unsigned int j = 0; j < nCount; j++) {
				unsigned int nNum = TiffU32(pBuffer + nValOff + j * 8, bBE);
				unsigned int nDen = TiffU32(pBuffer + nValOff + j * 8 + 4, bBE);
				double dVal = (double)nNum / (double)nDen;
				unsigned int nNewNum = (unsigned int)(dVal * 10000.0 + 0.5);
				if (nNewNum > 10000) nNewNum = 10000;
				TiffPutU32(pBuffer + nValOff + j * 8, bBE, nNewNum);
				TiffPutU32(pBuffer + nValOff + j * 8 + 4, bBE, 10000);
			}
		}
	}
	if (nDepth < 2) {
		for (size_t s = 0; s < subIFDs.size(); s++) {
			if (subIFDs[s] == 0) {
				continue;
			}
			if (!TiffSanitizeWalk(pBuffer, nSize, subIFDs[s], bBE, nDepth + 1, pVisited, nVisitedCount, bSuspicious)) {
				return false;
			}
		}
	}
	unsigned int nNext = TiffU32(pBuffer + (size_t)nIFD + 2 + (size_t)nEntries * 12, bBE);
	if (nNext == 0) {
		return true;
	}
	if (nNext == nIFD) {
		return false;
	}
	return TiffSanitizeWalk(pBuffer, nSize, nNext, bBE, nDepth, pVisited, nVisitedCount, bSuspicious);
}

static bool TiffSanitizeChromaticity(unsigned char* pBuffer, size_t nSize) {
	if (pBuffer == NULL || nSize < 8) return false;
	bool bBig = (pBuffer[0] == 'M' && pBuffer[1] == 'M');
	bool bLittle = (pBuffer[0] == 'I' && pBuffer[1] == 'I');
	if (!bBig && !bLittle) return false;
	bool bBE = bBig;
	if (TiffU16(pBuffer + 2, bBE) != 42) return false;
	unsigned __int64 nIFD = TiffU32(pBuffer + 4, bBE);
	unsigned __int64 visited[32];
	int nVisitedCount = 0;
	bool bSuspicious = false;
	if (!TiffSanitizeWalk(pBuffer, nSize, nIFD, bBE, 0, visited, nVisitedCount, bSuspicious)) return false;
	return bSuspicious;
}

// pEXIFData/nJPEGFileHash are only supplied by the JPEG CMYK fallback
// (R-09), so a GDI+ fallback keeps the EXIF block and file hash the
// success path would have attached. All other callers keep the defaults
// and behave exactly as before.
void CImageLoadThread::ProcessReadGDIPlusRequest(CRequest * request, void* pEXIFData, __int64 nJPEGFileHash) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	Gdiplus::Bitmap* pBitmap = NULL;
	bool isOutOfMemory = false, isAnimatedGIF = false, isSizeLimitExceeded = false;
	// Sanitized first try: if this TIFF carries suspicious 318/319, load a
	// memory copy, normalise those tags in place, and decode from a stream
	// (CreateStreamOnHGlobal, the same pattern the JPEG/PNG paths use).
	// The original file is never modified. If the sanitized decode yields
	// an image, that is the result; if it yields nothing, the code below
	// retries the original filename path, so a sanitized failure can never
	// turn a readable file into an unreadable one.
	// E12: the probe hands over its open handle when the TIFF needs
	// sanitizing, so the whole-file read below reuses that open instead of
	// opening the file a second time. The probe leaves the handle at an
	// arbitrary offset, so seek back to 0 before reading; if the seek fails
	// the sanitized attempt is skipped and the filename path below runs.
	// R-18: the sanitized copy below is deliberately a whole-file read
	// (GlobalAlloc(fileSize) + one ReadFile, capped at 0x7FFFFFFF).
	// TiffSanitizeChromaticity works on a whole in-memory buffer, and the
	// original file must never be modified in place, so any sanitizing
	// scheme needs one full copy somewhere. Streaming the file through a
	// temp file into GDI+ would avoid holding the whole file in memory,
	// but it needs a streaming sanitizer plus temp-file lifecycle on top
	// of the atomic-save machinery - a bigger change than this polish
	// batch. A GlobalAlloc failure here simply falls through to the plain
	// filename decode below, so the worst case is a missed sanitization,
	// never a missed image.
	HANDLE hProbeFile = NULL;
	if (Helpers::GetImageFormat(request->FileName) == IF_TIFF && TiffChromaticityNeedsSanitization(sFileName, hProbeFile)) {
		DeleteCachedGDIBitmap();
		HANDLE hFile = hProbeFile;
		if (hFile != INVALID_HANDLE_VALUE && hFile != NULL) {
			LARGE_INTEGER liZero;
			liZero.QuadPart = 0;
			bool bSeekOk = ::SetFilePointerEx(hFile, liZero, NULL, FILE_BEGIN) != 0;
			LARGE_INTEGER liSize;
			if (bSeekOk && ::GetFileSizeEx(hFile, &liSize) && liSize.QuadPart > 0 && liSize.QuadPart <= 0x7FFFFFFF) {
				HGLOBAL hBuffer = ::GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)liSize.QuadPart);
				void* pData = (hBuffer == NULL) ? NULL : ::GlobalLock(hBuffer);
				if (pData != NULL) {
					DWORD nRead = 0;
					if (::ReadFile(hFile, pData, (DWORD)liSize.QuadPart, &nRead, NULL) && nRead == (DWORD)liSize.QuadPart && TiffSanitizeChromaticity((unsigned char*)pData, (size_t)liSize.QuadPart)) {
						IStream* pStream = NULL;
						if (::CreateStreamOnHGlobal(hBuffer, FALSE, &pStream) == S_OK) {
							Gdiplus::Bitmap* pSanitizedBitmap = new Gdiplus::Bitmap(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
							if (pSanitizedBitmap != NULL && pSanitizedBitmap->GetLastStatus() == Gdiplus::Ok) {
								request->Image = ConvertGDIPlusBitmapToJPEGImage(pSanitizedBitmap, request->FrameIndex, pEXIFData, nJPEGFileHash, isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded);
							request->SizeLimitExceeded = request->Image == NULL && isSizeLimitExceeded;
							}
							delete pSanitizedBitmap;
							pStream->Release();
						}
					}
					::GlobalUnlock(hBuffer);
				}
				if (hBuffer != NULL) ::GlobalFree(hBuffer);
			}
			::CloseHandle(hFile);
		}
	}
	if (request->Image == NULL) {
		if (sFileName == m_sLastFileName) {
			pBitmap = m_pLastBitmap;
		} else {
			DeleteCachedGDIBitmap();
			m_pLastBitmap = pBitmap = new Gdiplus::Bitmap(sFileName, CSettingsProvider::This().UseEmbeddedColorProfiles());
			m_sLastFileName = sFileName;
		}
		request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, request->FrameIndex, pEXIFData, nJPEGFileHash, isOutOfMemory, isAnimatedGIF, isSizeLimitExceeded);
	}
	request->OutOfMemory = request->Image == NULL && isOutOfMemory;
	request->SizeLimitExceeded = request->Image == NULL && isSizeLimitExceeded;
	if (request->OutOfMemory && GetBitmapFormat(pBitmap) == IF_TIFF) {
		request->OutOfMemory = false; // reset before WIC retry
		DeleteCachedGDIBitmap();
		ProcessReadWICRequest(request);
		// fall through to the RAW fallback below if WIC also failed
	}
	if (!isAnimatedGIF) {
		DeleteCachedGDIBitmap();
	}
	// TIFF RAW fallback: a CFA TIFF (Canon/Kodak DCS) is a valid TIFF
	// container that neither GDI+ nor WIC demosaics, so both fail while
	// LibRaw decodes the full frame. Only .tif/.tiff files that GDI+
	// (and WIC, on the OOM path) already failed reach here, so normal
	// RGB TIFFs keep their exact GDI+ behaviour; this runs only on
	// failure. It goes through ProcessReadRAWRequest, so the gate and
	// DisplayFullSizeRAW still apply, and a file LibRaw also rejects
	// stays unreadable exactly as before.
	if (request->Image == NULL && Helpers::GetImageFormat(request->FileName) == IF_TIFF) {
		ProcessReadRAWRequest(request);
	}
}

static unsigned char* alloc(size_t sizeInBytes) {
	return new(std::nothrow) unsigned char[sizeInBytes];
}

static void dealloc(unsigned char* buffer) {
	delete[] buffer;
}

typedef unsigned char* Allocator(size_t sizeInBytes);
typedef void Deallocator(unsigned char* buffer);

__declspec(dllimport) unsigned char* __stdcall LoadImageWithWIC(LPCWSTR fileName, Allocator* allocator, Deallocator* deallocator,
	unsigned int* width, unsigned int* height);

void CImageLoadThread::ProcessReadWICRequest(CRequest* request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	try {
		uint32 nWidth, nHeight;
		unsigned char* pDIB = LoadImageWithWIC(sFileName, &alloc, &dealloc, &nWidth, &nHeight);
		if (pDIB != NULL) {
			request->Image = new CJPEGImage(nWidth, nHeight, pDIB, NULL, 4, 0, IF_WIC, false, 0, 1, 0);
		}
	} catch (...) {
		// fatal error in WIC
	}
}

// P2-6: tri-state (see ImageLoadThread.h). A rotation failure used to return
// false here and the caller treated every false as out-of-memory, so a bad
// rotation freed all memory, re-decoded the whole image, and then reported
// "not enough memory". Only a genuine allocation failure returns OOM now.
CImageLoadThread::EAfterLoadResult CImageLoadThread::ProcessImageAfterLoad(CRequest * request) {
	// set process parameters depending on filename
	request->Image->SetFileDependentProcessParams(request->FileName, &(request->ProcessParams));

	// First do rotation, this maybe modifies the width and height
	if (!request->Image->VerifyRotation(CRotationParams(request->ProcessParams.RotationParams, request->ProcessParams.RotationParams.Rotation + request->ProcessParams.UserRotation))) {
		return AfterLoad_Error;
	}

	// Do nothing (except rotation) if processing after load turned off
	if (GetProcessingFlag(request->ProcessParams.ProcFlags, PFLAG_NoProcessingAfterLoad)) {
		return AfterLoad_OK;
	}

	int nWidth = request->Image->OrigWidth();
	int nHeight = request->Image->OrigHeight();

	double dZoom = request->ProcessParams.Zoom;
	CSize newSize;
	if (dZoom < 0.0) {
		newSize = Helpers::GetImageRect(nWidth, nHeight, 
			request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight, request->ProcessParams.AutoZoomMode, dZoom);
	} else {
		newSize = CSize((int)(nWidth*dZoom + 0.5), (int)(nHeight*dZoom + 0.5));
	}

	newSize.cx = max(1, min(65535, newSize.cx));
	newSize.cy = max(1, min(65535, newSize.cy)); // max size must not be bigger than this after zoom

	// clip to target rectangle
	CSize clippedSize(min(request->ProcessParams.TargetWidth, newSize.cx), 
		min(request->ProcessParams.TargetHeight, newSize.cy));

	LimitOffsets(request->ProcessParams.Offsets, CSize(request->ProcessParams.TargetWidth, request->ProcessParams.TargetHeight), newSize);

	// this will process the image and cache the processed DIB in the CJPEGImage instance
	CPoint offsetInImage = request->Image->ConvertOffset(newSize, clippedSize, request->ProcessParams.Offsets);
	if (NULL != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
		request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags)) {
		return AfterLoad_OK;
	}
	// P2-6: the DIB prefetch failed. If the requested DIB itself exceeds the
	// pixel/byte caps it is a size-limit failure, not OOM - retrying after
	// freeing memory would just fail again on the same cap. Anything else is
	// treated as a genuine allocation failure: every NULL return on the DIB
	// path below the size gate comes from a failed (nothrow) allocation.
	if ((size_t)clippedSize.cx * (size_t)clippedSize.cy > CSettingsProvider::This().MaxImagePixels() ||
		!IsImageBufferSizeOk((size_t)clippedSize.cx * 4, (size_t)clippedSize.cy)) {
		request->SizeLimitExceeded = true;
		return AfterLoad_Error;
	}
	return AfterLoad_OOM;
}
