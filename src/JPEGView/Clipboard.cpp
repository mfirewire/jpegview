#include "StdAfx.h"
#include "Clipboard.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
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
			int nNumColors = pbmInfo->bmiHeader.biClrUsed;
			if (nNumColors == 0 && pbmInfo->bmiHeader.biBitCount <= 8) {
				nNumColors = 1 << pbmInfo->bmiHeader.biBitCount;
			}
			// P3-4: biClrUsed comes from the clipboard DIB header and is
			// attacker-controlled; an unbounded value pushes pDIBBits past
			// the end of the buffer (on Win32 the nNumColors*sizeof(RGBQUAD)
			// product can also wrap). Clamp to the palette size the format
			// can actually have: at most 1 << biBitCount entries for
			// <= 8 bpp, at most 256 otherwise. (BI_BITFIELDS below still
			// overrides this with 3.)
			if (nNumColors < 0) {
				nNumColors = 0;
			} else if (pbmInfo->bmiHeader.biBitCount <= 8) {
				int nMaxColors = 1 << pbmInfo->bmiHeader.biBitCount;
				if (nNumColors > nMaxColors) {
					nNumColors = nMaxColors;
				}
			} else if (nNumColors > 256) {
				nNumColors = 256;
			}
			if (pbmInfo->bmiHeader.biCompression == BI_BITFIELDS) {
				nNumColors = 3;
			}
			char* pDIBBits = (char*)pbmInfo + pbmInfo->bmiHeader.biSize + nNumColors*sizeof(RGBQUAD);

			if (pbmInfo->bmiHeader.biWidth <= MAX_IMAGE_DIMENSION && abs(pbmInfo->bmiHeader.biHeight) <= MAX_IMAGE_DIMENSION) {
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
		::MessageBox(hWnd, _T("Not enough memory to copy the image to the clipboard."), _T("JPEGView"), MB_OK | MB_ICONERROR);
		::CloseClipboard();
		return;
	}
	// P3-5: empty the clipboard only now that the allocation succeeded;
	// emptying it first and then failing the allocation silently
	// destroyed the user's previous clipboard contents.
	::EmptyClipboard();
	void* pMemory = ::GlobalLock(hMem); 

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
	::SetClipboardData(CF_DIB, hMem);

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