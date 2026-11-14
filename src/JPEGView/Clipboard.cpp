#include "StdAfx.h"
#include "Clipboard.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include "NLS.h"
#include <gdiplus.h>

static void CopyOriginalFileNameToClipboard(LPCWSTR filename) {
	int fileNameLength = (int)wcslen(filename);
	DWORD fileNameLengthBytes = sizeof(wchar_t) * (fileNameLength + 1);
	DROPFILES df = {sizeof(DROPFILES), {0, 0}, 0, TRUE};
	HGLOBAL hMem = ::GlobalAlloc(GMEM_ZEROINIT|GMEM_MOVEABLE|GMEM_DDESHARE, sizeof(DROPFILES) + fileNameLengthBytes + sizeof(wchar_t)); // for double NULL char
	int offset = sizeof(DROPFILES) / sizeof(wchar_t);
	WCHAR *pGlobal = (WCHAR *) ::GlobalLock(hMem);
	::CopyMemory(pGlobal, &df, sizeof(DROPFILES));
	::CopyMemory(pGlobal + offset, filename, fileNameLengthBytes); // that's pGlobal + 20 bytes (the size of DROPFILES);
	pGlobal[offset + fileNameLength + 1] = 0; // add additional NULL character
	::GlobalUnlock(hMem);
	::SetClipboardData(CF_HDROP, hMem);
}

static void CopyFileNameTextToClipboard(LPCWSTR filename) {
	int fileNameLength = (int)wcslen(filename);
	DWORD fileNameLengthBytes = sizeof(wchar_t) * (fileNameLength + 1);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_ZEROINIT | GMEM_MOVEABLE | GMEM_DDESHARE, fileNameLengthBytes + sizeof(wchar_t)); // for double NULL char
	WCHAR* pGlobal = (WCHAR*) ::GlobalLock(hMem);
	::CopyMemory(pGlobal, filename, fileNameLengthBytes);
	pGlobal[fileNameLength + 1] = 0; // add additional NULL character
	::GlobalUnlock(hMem);
	::SetClipboardData(CF_UNICODETEXT, hMem);
}

void CClipboard::CopyImageToClipboard(HWND hWnd, CJPEGImage * pImage, LPCTSTR fileName) {
	if (pImage == NULL) {
		return;
	}
	// A2-3 (R-10 follow-up): DIBPixelsLastProcessed(true) calls GetDIB,
	// which can throw on allocation failure; this runs on the UI thread
	// (IDM_COPY) with no try/catch above. Skip the copy on failure, as
	// CopyFullImageToClipboard does below.
	void* pDIBPixels = NULL;
	try {
		pDIBPixels = pImage->DIBPixelsLastProcessed(true);
	} catch (...) {
		return;
	}
	if (pDIBPixels == NULL) {
		return;
	}

	pImage->EnableDimming(false);
	// EnableDimming(false) drops the cached DIB when dim rects exist
	// (JPEGImage.cpp), so the DIB is generated again here -- and can
	// throw again. Restore dimming before returning on failure.
	try {
		pDIBPixels = pImage->DIBPixelsLastProcessed(true);
	} catch (...) {
		pImage->EnableDimming(true);
		return;
	}
	if (pDIBPixels == NULL) {
		pImage->EnableDimming(true);
		return;
	}
	DoCopy(hWnd, pImage->DIBWidth(), pImage->DIBHeight(), pDIBPixels, pImage->IsClipboardImage() ? NULL : fileName);
	pImage->EnableDimming(true);
}

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, const CImageProcessingParams& procParams,
										  EProcessingFlags eFlags, LPCTSTR fileName) {
	CopyFullImageToClipboard(hWnd, pImage, procParams, eFlags, CRect(0, 0, pImage->OrigWidth(), pImage->OrigHeight()), fileName);
}

void CClipboard::CopyFullImageToClipboard(HWND hWnd, CJPEGImage * pImage, const CImageProcessingParams& procParams,
										  EProcessingFlags eFlags, CRect clipRect, LPCTSTR fileName) {
	if (pImage == NULL) {
		return;
	}

	clipRect.left = max(0, clipRect.left);
	clipRect.top = max(0, clipRect.top);
	clipRect.right = min(pImage->OrigWidth(), clipRect.right);
	clipRect.bottom = min(pImage->OrigHeight(), clipRect.bottom);

	pImage->EnableDimming(false);
	void* pDIB = NULL;
	try {
		pDIB = pImage->GetDIB(pImage->OrigSize(), clipRect.Size(), clipRect.TopLeft(), procParams, eFlags);
	} catch (...) {
		// R-10: allocation failure inside GetDIB must not kill the UI thread
		// (called from MainDlg IDM_COPY_FULL); skip the copy, as in the
		// pImage == NULL case above.
		pImage->EnableDimming(true);
		return;
	}
	if (pDIB == NULL) {
		// GetDIB returned NULL without throwing: skip the copy, as in the
		// catch branch above -- otherwise DoCopy would empty the clipboard
		// and write a fake black image.
		pImage->EnableDimming(true);
		return;
	}
	DoCopy(hWnd, clipRect.Width(), clipRect.Height(), pDIB, pImage->IsClipboardImage() ? NULL : fileName);
	pImage->EnableDimming(true);
}

void CClipboard::CopyPathToClipboard(HWND hWnd, CJPEGImage* pImage, LPCTSTR fileName) {
	if (pImage == NULL) {
		return;
	}
	// A2-3 (R-10 follow-up): see CopyImageToClipboard above. The DIB is
	// only a "processed image present" gate here, but generating it can
	// still throw on allocation failure, so the check is guarded too.
	bool bHasDIB = false;
	try {
		bHasDIB = pImage->DIBPixelsLastProcessed(true) != NULL;
	} catch (...) {
		return;
	}
	if (!bHasDIB) {
		return;
	}
	pImage->EnableDimming(false);
	DoCopyFileNameText(hWnd, fileName);
	pImage->EnableDimming(true);
}

// L-D06: validate the layout of a clipboard DIB before GDI+ sees it. The
// block belongs to another process, so biSize / biWidth / biHeight /
// biBitCount / biClrUsed are all untrusted: GDI+ would otherwise read a
// header larger than the block, a palette past its end, or a pixel area
// that runs off the buffer. On success nNumColors is the palette entry
// count (0 for >8 bpp unless BI_BITFIELDS masks follow a plain header).
static bool ValidateClipboardDIB(const BITMAPINFO* pbmInfo, SIZE_T nMemSize, int& nNumColors) {
	nNumColors = 0;
	if (pbmInfo == NULL || nMemSize < sizeof(BITMAPINFOHEADER)) {
		return false;
	}
	const BITMAPINFOHEADER& hdr = pbmInfo->bmiHeader;
	if (hdr.biSize < sizeof(BITMAPINFOHEADER) || (SIZE_T)hdr.biSize > nMemSize) {
		return false;
	}
	if (hdr.biPlanes != 1 || hdr.biWidth <= 0) {
		return false;
	}
	const long long nHeight = (long long)hdr.biHeight;
	if (nHeight == 0) {
		return false;
	}
	const long long nAbsHeight = (nHeight < 0) ? -nHeight : nHeight;
	if ((unsigned int)hdr.biWidth > MAX_IMAGE_DIMENSION || nAbsHeight > (long long)MAX_IMAGE_DIMENSION) {
		return false;
	}
	switch (hdr.biBitCount) {
		case 1: case 4: case 8: case 16: case 24: case 32:
			break;
		default:
			return false;
	}
	// P3-4 keeps the clamp on biClrUsed; the block bound below is what
	// makes the resulting pointer safe.
	int nColors = hdr.biClrUsed;
	if (nColors < 0) {
		nColors = 0;
	}
	if (hdr.biBitCount <= 8) {
		const int nMaxColors = 1 << hdr.biBitCount;
		if (nColors == 0) {
			nColors = nMaxColors;
		} else if (nColors > nMaxColors) {
			nColors = nMaxColors;
		}
	} else {
		nColors = 0;
	}
	if (hdr.biCompression == BI_BITFIELDS && hdr.biSize == sizeof(BITMAPINFOHEADER)) {
		nColors = 3;   // three colour masks stored after a plain v1 header
	}
	// P1-10: only the uncompressed layouts are validated below. BI_RLE8/4,
	// BI_JPEG/BI_PNG and BI_ALPHABITFIELDS would be handed to GDI+ with no
	// pixel-extent check at all, so reject everything else up front.
	if (hdr.biCompression != BI_RGB && hdr.biCompression != BI_BITFIELDS) {
		return false;
	}
	const SIZE_T nHeaderBytes = (SIZE_T)hdr.biSize + (SIZE_T)nColors * sizeof(RGBQUAD);
	if (nHeaderBytes >= nMemSize) {
		return false;
	}
	if (hdr.biCompression == BI_RGB || hdr.biCompression == BI_BITFIELDS) {
		const SIZE_T nStride = (((SIZE_T)hdr.biWidth * hdr.biBitCount + 31) / 32) * 4;
		if (nStride > nMemSize) {
			return false;
		}
		// R-A3: compute the pixel byte count in 64 bits. With a 32-bit
		// SIZE_T (Win32) nStride * nAbsHeight wraps: 32768x32768x32bpp
		// has stride 131072, so the product is exactly 2^32 and wraps to
		// 0, passing the extent check and letting GDI+ read pixels past
		// the end of the block. The true product is at most
		// 262140 * 65535 (< 2^35), so unsigned __int64 is exact here on
		// both Win32 and x64.
		const unsigned __int64 nPixels = (unsigned __int64)nStride * (unsigned __int64)nAbsHeight;
		if (nPixels > (unsigned __int64)(nMemSize - nHeaderBytes)) {
			return false;
		}
	}
	nNumColors = nColors;
	return true;
}

CJPEGImage* CClipboard::PasteImageFromClipboard(HWND hWnd, const CImageProcessingParams& procParams, 
												EProcessingFlags eFlags) {
	if (!::OpenClipboard(hWnd)) {
		return NULL;
	}

	CJPEGImage* pImage = NULL;
	HANDLE handle = ::GetClipboardData(CF_DIB);
	if (handle != NULL) {
		BITMAPINFO* pbmInfo = (BITMAPINFO*)::GlobalLock(handle);
		if (pbmInfo != NULL) {
			// L-D06: validate the whole layout against GlobalSize before
			// GDI+ reads the header and the pixel area.
			SIZE_T nMemSize = ::GlobalSize(handle);
			int nNumColors = 0;
			char* pDIBBits = NULL;
			if (ValidateClipboardDIB(pbmInfo, nMemSize, nNumColors)) {
				pDIBBits = (char*)pbmInfo + pbmInfo->bmiHeader.biSize + nNumColors*sizeof(RGBQUAD);
			}
			if (pDIBBits != NULL) {
				// R-10: 'new Gdiplus::Bitmap' and 'new CJPEGImage' below (its
				// CLocalDensityCorr member) can throw on allocation failure;
				// this runs on the UI thread (GotoImage, no try/catch above).
				// On failure the paste returns NULL, which the caller already
				// maps to PasteFromClipboardFailed.
				Gdiplus::Bitmap* pBitmap = NULL;
				try {
					pBitmap = new Gdiplus::Bitmap(pbmInfo, pDIBBits);
				} catch (...) {
					pBitmap = NULL;
				}
				if (pBitmap != NULL && pBitmap->GetLastStatus() == Gdiplus::Ok) {
					Gdiplus::Rect bmRect(0, 0, pBitmap->GetWidth(), pBitmap->GetHeight());
					Gdiplus::BitmapData bmData;
					if (pBitmap->LockBits(&bmRect, Gdiplus::ImageLockModeRead, PixelFormat32bppRGB, &bmData) == Gdiplus::Ok) {
						assert(bmData.PixelFormat == PixelFormat32bppRGB);
						void* pDIB = CBasicProcessing::ConvertGdiplus32bppRGB(bmRect.Width, bmRect.Height, bmData.Stride, bmData.Scan0);
						try {
							pImage = (pDIB == NULL) ? NULL : new CJPEGImage(bmRect.Width, bmRect.Height, pDIB, NULL, 4, 0, IF_CLIPBOARD, false, 0, 1, 0);
						} catch (...) {
							// R-10: the constructor threw (CLocalDensityCorr);
							// the converted DIB was never adopted, free it.
							delete[] (uint32*)pDIB;
							pImage = NULL;
						}
						pBitmap->UnlockBits(&bmData);
					}
				}
				delete pBitmap;
			}

			::GlobalUnlock(handle);
		}
	}

	::CloseClipboard();

	return pImage;
}


void CClipboard::DoCopy(HWND hWnd, int nWidth, int nHeight, const void* pSourceImageDIB32, LPCTSTR fileName) {
	if (!::OpenClipboard(hWnd)) {
		return;
	}

	// get needed size of memory block
	// R-30: size_t + IsImageBufferSizeOk gate (was uint32: a future decoder
	// bypassing the byte cap could wrap the size and heap-overflow in
	// Convert32bppTo24bppDIB below). nWidth/nHeight are image dimensions,
	// already bounded by MAX_IMAGE_DIMENSION, so nWidth*3 cannot overflow
	// int before DoPadding; the gate still checks the real product.
	size_t nSizeLinePadded = (size_t)Helpers::DoPadding(nWidth*3, 4);
	size_t nSizeBytes = 0;
	if (!IsImageBufferSizeOk(nSizeLinePadded, (size_t)nHeight, &nSizeBytes)) {
		::CloseClipboard();
		return;
	}
	nSizeBytes += sizeof(BITMAPINFO);
	if (nSizeBytes > MAX_IMAGE_BYTES) { // header on top of a max-size image
		::CloseClipboard();
		return;
	}
	
	// Allocate memory
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, nSizeBytes);
	if (hMem == NULL) {
		// P3-5: DoCopy returns void and the callers do not check for
		// failure, so report the failure here. The clipboard has not
		// been emptied yet (see below), so its previous contents survive.
		::MessageBox(hWnd, CNLS::GetString(_T("Not enough memory to copy the image to the clipboard.")), _T("JPEGView"), MB_OK | MB_ICONERROR);
		::CloseClipboard();
		return;
	}
	// The lock must be checked before EmptyClipboard: locking a freshly
	// allocated block can only fail under extreme memory pressure, but if
	// it does, the clipboard must still hold the previous contents (same
	// class as the GlobalAlloc failure above).
	void* pMemory = ::GlobalLock(hMem);
	if (pMemory == NULL) {
		::MessageBox(hWnd, CNLS::GetString(_T("Not enough memory to copy the image to the clipboard.")), _T("JPEGView"), MB_OK | MB_ICONERROR);
		::GlobalFree(hMem);
		::CloseClipboard();
		return;
	}
	// P3-5: empty the clipboard only now that the allocation succeeded;
	// emptying it first and then failing the allocation silently
	// destroyed the user's previous clipboard contents.
	::EmptyClipboard();

	BITMAPINFO* pBMInfo = (BITMAPINFO*) pMemory;
	memset(pBMInfo, 0, sizeof(BITMAPINFO));

	pBMInfo->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	pBMInfo->bmiHeader.biWidth = nWidth;
	pBMInfo->bmiHeader.biHeight = nHeight;
	pBMInfo->bmiHeader.biPlanes = 1;
	pBMInfo->bmiHeader.biBitCount = 24;
	pBMInfo->bmiHeader.biCompression = BI_RGB;
	pBMInfo->bmiHeader.biXPelsPerMeter = 10000;
	pBMInfo->bmiHeader.biYPelsPerMeter = 10000;
	pBMInfo->bmiHeader.biClrUsed = 0;

	uint8* pDIBPixelsTarget = (uint8*)pMemory + sizeof(BITMAPINFO) - sizeof(RGBQUAD);
	CBasicProcessing::Convert32bppTo24bppDIB(nWidth, nHeight, pDIBPixelsTarget, pSourceImageDIB32, true);

	::GlobalUnlock(hMem); 
	if (::SetClipboardData(CF_DIB, hMem) == NULL) {
		// The handle belongs to the clipboard only on success; on
		// failure it is still ours and must be freed.
		::GlobalFree(hMem);
	}

	if (fileName != NULL) {
		CopyOriginalFileNameToClipboard(fileName);
	}

	::CloseClipboard();
}

void CClipboard::DoCopyFileNameText(HWND hWnd, LPCTSTR fileName) {
	if (!::OpenClipboard(hWnd)) {
		return;
	}
	::EmptyClipboard();

	CopyFileNameTextToClipboard(fileName);

	::CloseClipboard();
}