
#include "StdAfx.h"
#include "ImageLoadThread.h"
#include <gdiplus.h>
#include <vector>
#include "JPEGImage.h"
#include "MessageDef.h"
#include "Helpers.h"
#include "SettingsProvider.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "BasicProcessing.h"
#include "dcraw_mod.h"
#include "TJPEGWrapper.h"
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
	// Zero-initialised: a file shorter than 16 bytes leaves the tail as zero
	// instead of uninitialised stack, and the format checks below then compare
	// against fixed bytes rather than garbage.
	unsigned char header[16] = { 0 };
	int nSize = (int)fread((void*)header, 1, 16, fptr);
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
	} else if (header[0] == 0x01 && header[1] == 0x00 && header[2] == 0x00 && header[3] == 0x00) {
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
	__int64 nJPEGHash, bool &isOutOfMemory, bool &isAnimatedGIF) {

	isOutOfMemory = false;
	isAnimatedGIF = false;
	Gdiplus::Status lastStatus = pBitmap->GetLastStatus();
	if (lastStatus != Gdiplus::Ok) {
		isOutOfMemory = lastStatus == Gdiplus::OutOfMemory;
		return NULL;
	}

	if (pBitmap->GetWidth() > MAX_IMAGE_DIMENSION || pBitmap->GetHeight() > MAX_IMAGE_DIMENSION)
	{
		return NULL;
	}
	if ((double)pBitmap->GetWidth() * pBitmap->GetHeight() > MAX_IMAGE_PIXELS)
	{
		isOutOfMemory = true;
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
	}
}

int CImageLoadThread::AsyncLoad(LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams, HWND targetWnd, HANDLE eventFinished) {
	CRequest* pRequest = new CRequest(strFileName, nFrameIndex, targetWnd, processParams, eventFinished);

	ProcessAsync(pRequest);

	return pRequest->RequestHandle;
}

CImageData CImageLoadThread::GetLoadedImage(int nHandle) {
	Helpers::CAutoCriticalSection criticalSection(m_csList);
	CJPEGImage* imageFound = NULL;
	bool bFailedMemory = false;
	bool bFailedException = false;
	std::list<CRequestBase*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		CRequest* pRequest = (CRequest*)(*iter);
		if (pRequest->Processed && pRequest->Deleted == false && pRequest->RequestHandle == nHandle) {
			imageFound = pRequest->Image;
			bFailedMemory = pRequest->OutOfMemory;
			bFailedException = pRequest->ExceptionError;
			// only mark as deleted
			pRequest->Deleted = true;
			break;
		}
	}
	return CImageData(imageFound, bFailedMemory, bFailedException);
}

void CImageLoadThread::ReleaseFile(LPCTSTR strFileName) {
	CReleaseFileRequest* pRequest = new CReleaseFileRequest(strFileName);
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
		return;
	}

	CRequest& rq = (CRequest&)request;
	double dStartTime = Helpers::GetExactTickCount(); 
	// Get image format and read the image
	switch (GetImageFormat(rq.FileName)) {
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
	if (rq.Image != NULL) {
		rq.Image->SetLoadTickCount(Helpers::GetExactTickCount() - dStartTime); 
		if (!ProcessImageAfterLoad(&rq)) {
			delete rq.Image;
			rq.Image = NULL;
			rq.OutOfMemory = true;
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
	WebpReaderWriter::DeleteCache();
	m_sLastWebpFileName.Empty();
}

void CImageLoadThread::DeleteCachedPngDecoder() {
#ifndef WINXP
	PngReader::DeleteCache();
	m_sLastPngFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedJxlDecoder() {
#ifndef WINXP
	JxlReader::DeleteCache();
	m_sLastJxlFileName.Empty();
#endif
}

void CImageLoadThread::DeleteCachedAvifDecoder() {
#ifndef WINXP
	// prevent crashing when libavif/dav1d fail or missing
	CErrorModeGuard errorModeGuard;
	try {
		AvifReader::DeleteCache();
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
// otherwise CreateTransform falls back to the shipped cmyk.icm. Returns NULL
// when no transform could be built (lcms2 or cmyk.icm missing, e.g. the XP
// build), so the caller can still try GDI+.
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

void CImageLoadThread::ProcessReadJPEGRequest(CRequest * request) {
	HANDLE hFile = ::CreateFile(request->FileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return;
	}

	HGLOBAL hFileBuffer = NULL;
	void* pBuffer = NULL;
	try {
		// Don't read too huge files
		long long nFileSize = Helpers::GetFileSize(hFile);
		if (nFileSize > MAX_JPEG_FILE_SIZE) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		hFileBuffer  = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
		pBuffer = (hFileBuffer == NULL) ? NULL : ::GlobalLock(hFileBuffer);
		if (pBuffer == NULL) {
			if (hFileBuffer) ::GlobalFree(hFileBuffer);
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		unsigned int nNumBytesRead;
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD) &nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			bool bUseGDIPlus = CSettingsProvider::This().ForceGDIPlus();
			if (bUseGDIPlus) {
				IStream* pStream = NULL;
				if (::CreateStreamOnHGlobal(hFileBuffer, FALSE, &pStream) == S_OK) {
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
					bool isOutOfMemory, isAnimatedGIF;
					request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, Helpers::FindEXIFBlock(pBuffer, nFileSize),
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), isOutOfMemory, isAnimatedGIF);
					request->OutOfMemory = request->Image == NULL && isOutOfMemory;
					if (request->Image != NULL) {
						request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					}
					pStream->Release();
					delete pBitmap;
				} else {
					request->OutOfMemory = true;
				}
			}
			if (!bUseGDIPlus || request->OutOfMemory) {
				int nWidth, nHeight, nBPP;
				TJSAMP eChromoSubSampling;
				bool bOutOfMemory;
				// int nTicks = ::GetTickCount();

				void* pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, eChromoSubSampling, bOutOfMemory, pBuffer, nFileSize);
				
				/*
				TCHAR buffer[20];
				_stprintf_s(buffer, 20, _T("%d"), ::GetTickCount() - nTicks);
				::MessageBox(NULL, CString(_T("Elapsed ticks: ")) + buffer, _T("Time"), MB_OK);
				*/

				// CMYK/YCCK JPEGs decode to a 4-component C-M-Y-K plane; convert to BGR here.
				if (pPixelData != NULL && nBPP == 4) {
					void* pRGB = ConvertCmykToRgb(pPixelData, nWidth, nHeight, pBuffer, (int) nFileSize);
					delete[] pPixelData;
					if (pRGB != NULL) {
						request->Image = new CJPEGImage(nWidth, nHeight, pRGB, 
							Helpers::FindEXIFBlock(pBuffer, nFileSize), 3, 
							Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
						request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
						request->Image->SetJPEGChromoSampling(eChromoSubSampling);
					} else {
						// No CMYK transform available (lcms2 or cmyk.icm missing, e.g. XP build).
						ProcessReadGDIPlusRequest(request);
					}
				} else if (pPixelData != NULL && (nBPP == 3 || nBPP == 1)) {
					ApplyEmbeddedICCProfile(pPixelData, nWidth, nHeight, nBPP, pBuffer, (int) nFileSize);
					request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, 
						Helpers::FindEXIFBlock(pBuffer, nFileSize), nBPP, 
						Helpers::CalculateJPEGFileHash(pBuffer, nFileSize), IF_JPEG, false, 0, 1, 0);
					request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
					request->Image->SetJPEGChromoSampling(eChromoSubSampling);
				} else if (bOutOfMemory) {
					request->OutOfMemory = true;
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
	if (pBuffer) ::GlobalUnlock(hFileBuffer);
	if (hFileBuffer) ::GlobalFree(hFileBuffer);
}


void CImageLoadThread::ProcessReadBMPRequest(CRequest * request) {
	bool bOutOfMemory;
	request->Image = CReaderBMP::ReadBmpImage(request->FileName, bOutOfMemory);
	if (bOutOfMemory) {
		request->OutOfMemory = true;
	} else if (request->Image == NULL) {
		// probably one of the bitmap formats that can not be read directly, try with GDI+
		ProcessReadGDIPlusRequest(request);
	}
}

void CImageLoadThread::ProcessReadTGARequest(CRequest * request) {
	bool bOutOfMemory;
	request->Image = CReaderTGA::ReadTgaImage(request->FileName, CSettingsProvider::This().ColorTransparency(), bOutOfMemory);
	if (bOutOfMemory) {
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

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight;
			bool bHasAnimation = bUseCachedDecoder;
			int nFrameCount = 1;
			int nFrameTimeMs = 0;
			int nBPP;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData && nBPP == 4) {
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

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
		delete[] pBuffer;
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
	HGLOBAL hFileBuffer = NULL;
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
			hFileBuffer = ::GlobalAlloc(GMEM_MOVEABLE, nFileSize);
			pBuffer = (hFileBuffer == NULL) ? NULL : ::GlobalLock(hFileBuffer);
			if (pBuffer == NULL) {
				if (hFileBuffer) ::GlobalFree(hFileBuffer);
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		} else {
			nFileSize = 0; // to avoid compiler warnings, not used
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
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
				pPixelData = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
#endif

			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastPngFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_PNG, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
			} else {
				DeleteCachedPngDecoder();
				
				IStream* pStream = NULL;
				if (::CreateStreamOnHGlobal(hFileBuffer, FALSE, &pStream) == S_OK) {
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, CSettingsProvider::This().UseEmbeddedColorProfiles());
					bool isOutOfMemory, isAnimatedGIF;
					pEXIFData = PngReader::GetEXIFBlock(pBuffer, nFileSize);
					request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, 0, pEXIFData, 0, isOutOfMemory, isAnimatedGIF);
					request->OutOfMemory = request->Image == NULL && isOutOfMemory;
					pStream->Release();
					delete pBitmap;
				} else {
					request->OutOfMemory = true;
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
		if (pBuffer) ::GlobalUnlock(hFileBuffer);
		if (hFileBuffer) ::GlobalFree(hFileBuffer);
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
			void* pEXIFData;
			uint8* pPixelData = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastJxlFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, 4, 0, IF_JXL, bHasAnimation, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			} else {
				DeleteCachedJxlDecoder();
			}
		}
	}
	catch (...) {
		delete request->Image;
		request->Image = NULL;
		request->ExceptionError = true;
	}
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		// delete[] pBuffer;
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
	try {
		long long nFileSize = 0;
		unsigned int nNumBytesRead;
		if (!bUseCachedDecoder) {
			// Don't read too huge files
			nFileSize = Helpers::GetFileSize(hFile);
			if (nFileSize > MAX_HEIF_FILE_SIZE) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}

			pBuffer = new(std::nothrow) char[nFileSize];
			if (pBuffer == NULL) {
				request->OutOfMemory = true;
				::CloseHandle(hFile);
				return;
			}
		}
		if (bUseCachedDecoder || (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize)) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			bool bHasAnimation;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, request->FrameIndex, 
				nFrameCount, nFrameTimeMs, pEXIFData, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (bHasAnimation)
					m_sLastAvifFileName = sFileName;
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

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
	if (!bUseCachedDecoder) {
		::CloseHandle(hFile);
		delete[] pBuffer;
	}
	if (!bSuccess)
		return ProcessReadHEIFRequest(request);
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

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP, nFrameCount, nFrameTimeMs;
			nFrameCount = 1;
			nFrameTimeMs = 0;
			void* pEXIFData;
			uint8* pPixelData = (uint8*)HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIFData, request->OutOfMemory, request->FrameIndex, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				// Multiply alpha value into each AABBGGRR pixel
				uint32* pImage32 = (uint32*)pPixelData;
				for (int i = 0; i < nWidth * nHeight; i++)
					*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());

				request->Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nBPP, 0, IF_HEIF, false, request->FrameIndex, nFrameCount, nFrameTimeMs);
				free(pEXIFData);
			}
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
	delete[] pBuffer;
}

void CImageLoadThread::ProcessReadPSDRequest(CRequest* request) {
	request->Image = PsdReader::ReadImage(request->FileName, request->OutOfMemory);
	if (request->Image == NULL && !request->OutOfMemory) {
		request->Image = PsdReader::ReadThumb(request->FileName, request->OutOfMemory);
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

		pBuffer = new(std::nothrow) char[nFileSize];
		if (pBuffer == NULL) {
			request->OutOfMemory = true;
			::CloseHandle(hFile);
			return;
		}
		if (::ReadFile(hFile, pBuffer, nFileSize, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == nFileSize) {
			int nWidth, nHeight, nBPP;
			void* pPixelData = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, request->OutOfMemory, pBuffer, nFileSize);
			if (pPixelData != NULL) {
				if (nBPP == 4) {
					// Multiply alpha value into each AABBGGRR pixel
					uint32* pImage32 = (uint32*)pPixelData;
					for (int i = 0; i < nWidth * nHeight; i++)
						*pImage32++ = Helpers::AlphaBlendBackground(*pImage32, CSettingsProvider::This().ColorTransparency());
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
	delete[] pBuffer;
}

// Decoder-independent RAW gate. Both libraw and dcraw_mod allocate and read the
// image according to the dimensions the file declares, and neither checks those
// dimensions against the file's real size. A DNG that declares 65535x21846 but
// carries only a few KB reads past its buffer inside them and crashes
// intermittently (observed: 2 crashes in 4 opens). Parse the TIFF IFD0 here,
// before either decoder runs, and compare the declared data size with the file
// size using 64-bit arithmetic. Files that are not TIFF-shaped are let through
// to the decoders rather than being mis-classified.
static bool IsPlausibleRawFile(LPCTSTR fileName) {
	HANDLE hFile = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER liSize;
	if (!::GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0) {
		::CloseHandle(hFile);
		return false;
	}
	const unsigned long long fileSize = (unsigned long long)liSize.QuadPart;

	// TIFF metadata sits at the front of the file; the first 1 MB covers the
	// IFD and virtually every tag's external value. If it cannot be read, let
	// the decoder report its own error rather than guessing.
	const unsigned long long metaBytes = (fileSize < 1048576ULL) ? fileSize : 1048576ULL;
	std::vector<unsigned char> buf((size_t)metaBytes);
	DWORD got = 0;
	if (!::ReadFile(hFile, &buf[0], (DWORD)metaBytes, &got, NULL) || got < 8) {
		::CloseHandle(hFile);
		return false;
	}
	::CloseHandle(hFile);

	bool little;
	if (buf[0] == 'I' && buf[1] == 'I') {
		little = true;
	} else if (buf[0] == 'M' && buf[1] == 'M') {
		little = false;
	} else {
		return true; // not TIFF, leave it to the decoders
	}

	const unsigned long long kInvalid = (unsigned long long)-1;
	auto u16 = [&](unsigned long long off) -> unsigned long long {
		if (off + 2 > (unsigned long long)buf.size()) return kInvalid;
		if (little) return buf[(size_t)off] | ((unsigned long long)buf[(size_t)off + 1] << 8);
		return ((unsigned long long)buf[(size_t)off] << 8) | buf[(size_t)off + 1];
	};
	auto u32 = [&](unsigned long long off) -> unsigned long long {
		if (off + 4 > (unsigned long long)buf.size()) return kInvalid;
		if (little)
			return buf[(size_t)off] | ((unsigned long long)buf[(size_t)off + 1] << 8) |
				((unsigned long long)buf[(size_t)off + 2] << 16) | ((unsigned long long)buf[(size_t)off + 3] << 24);
		return ((unsigned long long)buf[(size_t)off] << 24) | ((unsigned long long)buf[(size_t)off + 1] << 16) |
			((unsigned long long)buf[(size_t)off + 2] << 8) | buf[(size_t)off + 3];
	};

	if (u16(2) != 42) return true; // not a standard TIFF

	const unsigned long long ifdOff = u32(4);
	if (ifdOff == kInvalid || ifdOff + 2 > (unsigned long long)buf.size()) {
		return false; // claims to be TIFF but the IFD is unreadable
	}
	const unsigned long long n = u16(ifdOff);
	if (n == kInvalid || n == 0 || n > 512) {
		return false;
	}

	unsigned long long width = 0, height = 0, compression = 1, samples = 0, bits = 0;
	unsigned long long stripOffsets[512];
	unsigned long long stripByteCounts[512];
	int nStripOff = 0, nStripCnt = 0;

	for (unsigned long long k = 0; k < n; ++k) {
		const unsigned long long e = ifdOff + 2 + k * 12;
		if (e + 12 > (unsigned long long)buf.size()) break;
		const unsigned long long tag = u16(e);
		const unsigned long long type = u16(e + 2);
		const unsigned long long count = u32(e + 4);
		if (count == kInvalid || count == 0 || count > 512) continue;

		unsigned long long typeSize = 0;
		switch (type) {
			case 1: case 2: case 6: case 7: typeSize = 1; break;
			case 3: typeSize = 2; break;
			case 4: case 9: case 11: typeSize = 4; break;
			case 5: case 10: typeSize = 8; break;
			default: continue;
		}
		const unsigned long long byteCount = typeSize * count;

		// Inline value (<= 4 bytes) or offset to the external value.
		unsigned long long valOff = e + 8;
		if (byteCount > 4) {
			valOff = u32(e + 8);
			if (valOff == kInvalid || valOff + byteCount > (unsigned long long)buf.size()) continue;
		}

		// Read the first value in its own width. A SHORT is left-justified in the
		// 4-byte value field, so reading it as a LONG would shift it by 16 bits
		// on big-endian files.
		unsigned long long val = 0;
		switch (type) {
			case 3: val = u16(valOff); break; // SHORT
			case 1: case 2: case 6: case 7: val = buf[(size_t)valOff]; break; // BYTE/ASCII
			default: val = u32(valOff); break; // LONG/SLONG/FLOAT/RATIONAL numerator
		}

		switch (tag) {
			case 256: width = val; break;
			case 257: height = val; break;
			case 259: compression = val; break;
			case 277: samples = val; break;
			case 258: bits = val; break;
			case 273:
				for (unsigned long long j = 0; j < count && nStripOff < 512; ++j)
					stripOffsets[nStripOff++] = u32(valOff + j * 4);
				break;
			case 279:
				for (unsigned long long j = 0; j < count && nStripCnt < 512; ++j)
					stripByteCounts[nStripCnt++] = u32(valOff + j * 4);
				break;
		}
	}

	// No sane declared size, or a size beyond any real sensor, is itself
	// suspicious. Keeping width/height <= 65535 also keeps the products below
	// from overflowing.
	if (width == 0 || height == 0 || width > 65535 || height > 65535) {
		return false;
	}

	const unsigned long long declaredPixels = width * height;

	if (compression == 1 || compression == 0) {
		// Uncompressed (0 is a missing/invalid value, treated conservatively as
		// uncompressed): the file must actually hold every declared pixel.
		const unsigned long long samplesPerPixel = samples ? samples : 1;
		const unsigned long long bitsPerSample = bits ? bits : 1;
		const unsigned long long declaredBytes = declaredPixels * samplesPerPixel * bitsPerSample / 8;
		if (declaredBytes > fileSize) return false;
	} else {
		// Compressed: at least one bit per pixel must fit, and every strip's
		// data range must lie inside the file.
		if (fileSize < declaredPixels / 8) return false;
		int nStrip = nStripOff < nStripCnt ? nStripOff : nStripCnt;
		for (int i = 0; i < nStrip; ++i) {
			if (stripOffsets[i] + stripByteCounts[i] > fileSize) return false;
		}
	}
	return true;
}

void CImageLoadThread::ProcessReadRAWRequest(CRequest * request) {
	bool bOutOfMemory = false;
	try {
		if (!IsPlausibleRawFile(request->FileName)) {
			return;
		}
		int fullsize = CSettingsProvider::This().DisplayFullSizeRAW();

#ifndef WINXP
		// Try with libraw
		CErrorModeGuard errorModeGuard;
		try {
			if (fullsize == 2 || fullsize == 3) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 2);
			}
			if (request->Image == NULL && fullsize == 2) {
				request->Image = CReaderRAW::ReadRawImage(request->FileName, bOutOfMemory);
			}
			if (request->Image == NULL) {
				request->Image = RawReader::ReadImage(request->FileName, bOutOfMemory, fullsize == 0 || fullsize == 3);
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
}

void CImageLoadThread::ProcessReadGDIPlusRequest(CRequest * request) {
	const wchar_t* sFileName;
	sFileName = (const wchar_t*)request->FileName;

	Gdiplus::Bitmap* pBitmap = NULL;
	if (sFileName == m_sLastFileName) {
		pBitmap = m_pLastBitmap;
	} else {
		DeleteCachedGDIBitmap();
		m_pLastBitmap = pBitmap = new Gdiplus::Bitmap(sFileName, CSettingsProvider::This().UseEmbeddedColorProfiles());
		m_sLastFileName = sFileName;
	}
	bool isOutOfMemory, isAnimatedGIF;
	request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, request->FrameIndex, NULL, 0, isOutOfMemory, isAnimatedGIF);
	request->OutOfMemory = request->Image == NULL && isOutOfMemory;
	if (request->OutOfMemory && GetBitmapFormat(pBitmap) == IF_TIFF) {
		request->OutOfMemory = false; // reset before WIC retry
		DeleteCachedGDIBitmap();
		return ProcessReadWICRequest(request);
	}
	if (!isAnimatedGIF) {
		DeleteCachedGDIBitmap();
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

bool CImageLoadThread::ProcessImageAfterLoad(CRequest * request) {
	// set process parameters depending on filename
	request->Image->SetFileDependentProcessParams(request->FileName, &(request->ProcessParams));

	// First do rotation, this maybe modifies the width and height
	if (!request->Image->VerifyRotation(CRotationParams(request->ProcessParams.RotationParams, request->ProcessParams.RotationParams.Rotation + request->ProcessParams.UserRotation))) {
		return false;
	}

	// Do nothing (except rotation) if processing after load turned off
	if (GetProcessingFlag(request->ProcessParams.ProcFlags, PFLAG_NoProcessingAfterLoad)) {
		return true;
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
	return NULL != request->Image->GetDIB(newSize, clippedSize, offsetInImage,
		request->ProcessParams.ImageProcParams, request->ProcessParams.ProcFlags);
}
