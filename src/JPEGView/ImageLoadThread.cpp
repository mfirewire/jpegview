
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
// R-07 (not fixed, annotated only): the invert-all-four step above is
// validated only for Adobe-polarity output, which is what all measured
// test files produce (all three carry APP14). A genuinely non-Adobe CMYK
// file without APP14 was never measured on a device decoder, so its
// polarity is unknown and this path may invert it wrongly. No branch is
// added here without such a measurement; see the decision list.
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
						request->Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nFileSize));
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
// intermittently (observed: 2 crashes in 4 opens). Parse the TIFF IFDs here
// (IFD0 plus SubIFDs), before either decoder runs, and compare the declared
// data size with the file size using 64-bit arithmetic. Files that are not
// TIFF-shaped are let through to the decoders rather than being mis-classified.
// R-03/R-04/R-05: this gate is a 32-bit TIFF gate, not a general RAW gate.
// It deliberately does NOT cover BigTIFF (magic 43, passed through below),
// non-TIFF RAW formats (CRW/RAF/X3F and friends, passed through below), or
// the "next IFD" chain after each IFD's entries. Those still rely on the
// decoders' own protection. What it does cover: IFD0 plus SubIFDs (tag
// 330, depth <= 2), strip (273/279) and tile (324/325) arrays, read by
// seeking to their file offsets instead of from a 1 MB prefix, so an IFD
// past 1 MB is no longer falsely rejected and external values past 1 MB
// are no longer silently skipped. A width/height above JPEGView's
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
	bool samplesPresent;
	bool bitsPresent;
	TiffGateArray stripOff;
	TiffGateArray stripCnt;
	TiffGateArray tileOff;
	TiffGateArray tileCnt;
	std::vector<unsigned long long> subIFDs;
	TiffGateIFD() : width(0), height(0), compression(1), samples(0), bits(0),
		samplesPresent(false), bitsPresent(false) {}
};

class TiffGateReader {
public:
	TiffGateReader(HANDLE hFile, unsigned long long fileSize, bool little)
		: m_hFile(hFile), m_fileSize(fileSize), m_little(little) {}

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
	bool ReadIFD(unsigned long long ifdOff, TiffGateIFD& info) const {
		unsigned char countBuf[2];
		if (!ReadAt(ifdOff, countBuf, 2)) {
			return false;
		}
		unsigned long long n = Rd16(countBuf);
		if (n == 0) {
			return false;
		}
		// n is a uint16, so the entry block is at most 65535 * 12 bytes.
		unsigned long long entriesSize = n * 12;
		if (ifdOff + 2 > m_fileSize || entriesSize > m_fileSize - (ifdOff + 2)) {
			return false;
		}
		std::vector<unsigned char> entries((size_t)entriesSize);
		if (!ReadAt(ifdOff + 2, &entries[0], entriesSize)) {
			return false;
		}
		for (unsigned long long k = 0; k < n; ++k) {
			const unsigned char* e = &entries[(size_t)(k * 12)];
			unsigned long long tag = Rd16(e);
			unsigned long long type = Rd16(e + 2);
			unsigned long long count = Rd32(e + 4);
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
				case 256: info.width = ReadScalar(dataOff, type); break;
				case 257: info.height = ReadScalar(dataOff, type); break;
				case 259: info.compression = ReadScalar(dataOff, type); break;
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
						return false;
					}
					for (unsigned long long j = 0; j < count; ++j) {
						unsigned long long v = ReadElement(dataOff, type, j);
						if (v == kInvalid) {
							return false;
						}
						info.subIFDs.push_back(v);
					}
					break;
				default: break;
			}
		}
		return true;
	}

	// Plausibility verdict for one parsed IFD. Strip/tile ranges stream
	// both partner arrays in lockstep (R-05: no 512 cap, nothing stored).
	bool CheckIFD(const TiffGateIFD& info) const {
		if (info.width == 0 || info.height == 0 || info.width > 65535 || info.height > 65535) {
			return false;
		}
		// R-02: with these caps the product below cannot overflow 64 bit
		// (65535^2 * 8 * 64 / 8 is far below 2^64).
		if (info.samplesPresent && info.samples > 8) {
			return false;
		}
		if (info.bitsPresent && info.bits > 64) {
			return false;
		}
		unsigned long long declaredPixels = info.width * info.height;
		if (info.compression == 1 || info.compression == 0) {
			// R-16: missing samples/bits no longer default to 1. A
			// missing declaration makes the bound meaningless, so the
			// file is rejected instead of passing a weakened check.
			if (!info.samplesPresent || !info.bitsPresent) {
				return false;
			}
			unsigned long long declaredBytes = declaredPixels * info.samples * info.bits / 8;
			if (declaredBytes > m_fileSize) {
				return false;
			}
		} else {
			if (m_fileSize < declaredPixels / 8) {
				return false;
			}
			if (!CheckArrayPair(info.stripOff, info.stripCnt)) {
				return false;
			}
			if (!CheckArrayPair(info.tileOff, info.tileCnt)) {
				return false;
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

	// R-04: IFD0 plus SubIFDs, depth <= 2 (IFD0 is depth 0; SubIFDs found
	// at depth 2 are not followed). Every IFD visited must pass the same
	// verdict: a malformed dimension hidden in a SubIFD is exactly what
	// this gate is for, and a plausible-looking IFD0 preview does not
	// excuse it.
	bool CheckIFDTree(unsigned long long ifdOff, int depth) const {
		TiffGateIFD info;
		if (!ReadIFD(ifdOff, info)) {
			return false;
		}
		if (!CheckIFD(info)) {
			return false;
		}
		if (depth >= 2) {
			return true;
		}
		for (size_t i = 0; i < info.subIFDs.size(); ++i) {
			unsigned long long sub = info.subIFDs[i];
			if (sub == 0) {
				continue; // 0 means "no SubIFD" for this entry
			}
			if (!CheckIFDTree(sub, depth + 1)) {
				return false;
			}
		}
		return true;
	}

private:
	HANDLE m_hFile;
	unsigned long long m_fileSize;
	bool m_little;
};

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
	unsigned long long fileSize = (unsigned long long)liSize.QuadPart;
	unsigned char header[8];
	DWORD got = 0;
	if (!::ReadFile(hFile, header, 8, &got, NULL) || got != 8) {
		::CloseHandle(hFile);
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
		return false; // claims to be TIFF but has no IFD
	}

	TiffGateReader reader(hFile, fileSize, little);
	bool ok = reader.CheckIFDTree(ifdOff, 0);
	::CloseHandle(hFile);
	return ok;
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

// pEXIFData/nJPEGFileHash are only supplied by the JPEG CMYK fallback
// (R-09), so a GDI+ fallback keeps the EXIF block and file hash the
// success path would have attached. All other callers keep the defaults
// and behave exactly as before.
void CImageLoadThread::ProcessReadGDIPlusRequest(CRequest * request, void* pEXIFData, __int64 nJPEGFileHash) {
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
	request->Image = ConvertGDIPlusBitmapToJPEGImage(pBitmap, request->FrameIndex, pEXIFData, nJPEGFileHash, isOutOfMemory, isAnimatedGIF);
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
