
#pragma once

#include <cstddef> // E8: size_t for m_nReadBufferCapacity
#include "ProcessParams.h"
#include "WorkThread.h"
#include <gdiplus.h>
#include "PNGWrapper.h"
#include "WEBPWrapper.h"
#ifndef WINXP
#include "JXLWrapper.h"
#include "AVIFWrapper.h"
#endif

class CJPEGImage;

// returned image data by CImageLoadThread.GetLoadedImage() method
class CImageData
{
public:
	// Gets the image
	CJPEGImage* Image;
	// True if the request failed due to memory
	bool IsRequestFailedOutOfMemory;
	// True if the request failed due to an unhandled exception
	bool IsRequestFailedException;
	// P2-5: true if the image was rejected for exceeding the pixel cap
	// (MaxImagePixelsMP). Distinct from out-of-memory: no retry, and the
	// UI must not report "not enough memory" for it.
	bool IsRequestFailedSizeLimit;

	CImageData(CJPEGImage* pImage, bool isRequestFailedOutOfMemory, bool isRequestFailedException, bool isRequestFailedSizeLimit = false) {
		Image = pImage;
		IsRequestFailedOutOfMemory = isRequestFailedOutOfMemory;
		IsRequestFailedException = isRequestFailedException;
		IsRequestFailedSizeLimit = isRequestFailedSizeLimit;
	}
};

// Read ahead and processing thread
class CImageLoadThread : public CWorkThread
{
public:
	CImageLoadThread(void);
	~CImageLoadThread(void);

	// Asynchronous loading  of an image. The message WM_IMAGE_LOAD_COMPLETED is
	// posted to the given window's message queue when loading is finished and the given event is signaled (if not NULL).
	// Returns identifier to query for resulting image. The query can be done as soon as the message is
	// received or the event has been signaled.
	// The file to load is given by its filename (with path) and the frame index (for multiframe images). The
	// frame index needs to be zero when the image only has one frame.
	// Returns -1 when the load request itself could not be allocated (A2-9).
	int AsyncLoad(LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams, HWND targetWnd, HANDLE eventFinished, bool forceFullSizeRAW = false);

	// Get loaded image, CImageData::Image is null if not (yet) available - use handle returned by AsyncLoad().
	// Call after having received the WM_IMAGE_LOAD_COMPLETED message to retrieve the loaded image.
	// Marks the request for deletion - only call once with the same handle
	CImageData GetLoadedImage(int nHandle);

	// Releases the cached image file if an image of the specified name is cached
	void ReleaseFile(LPCTSTR strFileName);

	// Gets the request handle value used for the last request
	static int GetCurHandleValue() { return m_curHandle; }

private:

	// Request for loading an image
	class CRequest : public CRequestBase {
	public:
		CRequest(LPCTSTR strFileName, int nFrameIndex, HWND wndTarget, const CProcessParams& processParams, HANDLE eventFinished, bool forceFullSizeRAW = false) 
			: CRequestBase(eventFinished), ProcessParams(processParams) {
			FileName = strFileName;
			FrameIndex = nFrameIndex;
			TargetWnd = wndTarget;
			RequestHandle = ::InterlockedIncrement((LONG*)&m_curHandle);
			Image = NULL;
			OutOfMemory = false;
			ExceptionError = false;
			SizeLimitExceeded = false;
			GateRejected = false;
			ForceFullSizeRAW = forceFullSizeRAW;
		}

		CString FileName;
		int FrameIndex;
		HWND TargetWnd;
		int RequestHandle;
		CJPEGImage* Image;
		CProcessParams ProcessParams;
		bool OutOfMemory;  // load caused an out of memory condition
		bool ExceptionError;  // an unhandled exception caused the load to fail
		bool SizeLimitExceeded;  // P2-5: image exceeded the pixel cap (not OOM)
		bool GateRejected;  // TIFF gate rejected the file (not a decode failure: never memoized)
		bool ForceFullSizeRAW;  // one-shot: load this request in full size, ignoring DisplayFullSizeRAW
	};

	// Request to release image file
	class CReleaseFileRequest : public CRequestBase {
	public:
		CReleaseFileRequest(LPCTSTR strFileName) : CRequestBase() {
			FileName = strFileName;
			Type = ReleaseFileRequest;
		}

		enum { ReleaseFileRequest = 1 };

		CString FileName;
	};

	static volatile int m_curHandle; // Request handle returned by AsyncLoad()

	Gdiplus::Bitmap* m_pLastBitmap; // Last read GDI+ bitmap, cached to speed up GIF animations
	CString m_sLastFileName; // Only for GDI+ files
	CString m_sLastWebpFileName; // Only for animated WebP files
	CString m_sLastPngFileName; // Only for animated PNG files
	CString m_sLastJxlFileName; // Only for animated JPEG XL files
	CString m_sLastAvifFileName; // Only for animated AVIF files

	// R-01: one animation decoder cache per load thread. The four readers
	// used to keep their caches in process-wide statics, so the two load
	// threads freed each other's in-flight decodes (heap corruption
	// 0xC0000374 when paging between animated images). Each thread now owns
	// its reader instances; the caches live inside them and are only ever
	// touched by their owning thread. The readers are deliberately left
	// without destructors that release the caches: CImageLoadThread's
	// destructor owns the release and skips it when Terminate() times out.
	PngReader m_pngReader;
	WebpReaderWriter m_webpReader;
#ifndef WINXP
	JxlReader m_jxlReader;
	AvifReader m_avifReader;
#endif

	// E8: per-thread reusable whole-file read buffer (see
	// ReadWholeFileIntoReuseBuffer in ImageLoadThread.cpp). Grows on
	// demand, released after a request once past 64 MB (P3-1); one
	// buffer per load thread, never shared (R-01).
	char* m_pReadBuffer;
	size_t m_nReadBufferCapacity;

	virtual void ProcessRequest(CRequestBase& request);
	virtual void AfterFinishProcess(CRequestBase& request);
	void DeleteCachedGDIBitmap();
	void DeleteCachedWebpDecoder();
	void DeleteCachedPngDecoder();
	void DeleteCachedJxlDecoder();
	void DeleteCachedAvifDecoder();

	void ProcessReadJPEGRequest(CRequest * request);
	void ProcessReadPNGRequest(CRequest * request);
	void ProcessReadBMPRequest(CRequest * request);
	void ProcessReadTGARequest(CRequest * request);
	void ProcessReadWEBPRequest(CRequest * request);
	void ProcessReadJXLRequest(CRequest* request);
	void ProcessReadAVIFRequest(CRequest* request);
	void ProcessReadHEIFRequest(CRequest * request);
	// E8: HEIF decode from an already-read buffer (AVIF fallback reuses the
	// buffer it just read instead of opening and reading the file again).
	void ProcessReadHEIFRequest(CRequest * request, const char* pBuffer, long long nFileSize);
	void ProcessReadQOIRequest(CRequest * request);
	void ProcessReadPPMRequest(CRequest * request);
	void ProcessReadHDRRequest(CRequest * request);
	void ProcessReadPBMRequest(CRequest * request);
	void ProcessReadPAMRequest(CRequest * request);
	void ProcessReadJP2Request(CRequest * request);
	void ProcessReadEXRRequest(CRequest * request);
	void ProcessReadSVGRequest(CRequest * request);
	void ProcessReadPSDRequest(CRequest * request);
	void ProcessReadRAWRequest(CRequest * request);
	void ProcessReadGDIPlusRequest(CRequest * request, void* pEXIFData = NULL, __int64 nJPEGFileHash = 0);
	void ProcessReadWICRequest(CRequest* request);
	// E8: read a whole file into m_pReadBuffer (grows it if needed).
	// Returns false only when the buffer cannot be grown.
	bool ReadWholeFileIntoReuseBuffer(HANDLE hFile, long long nFileSize, char*& pBufferOut, unsigned int& nBytesReadOut);

	static void SetFileDependentProcessParams(CRequest * request);
	// P2-6: ProcessImageAfterLoad is tri-state now. OK = image processed and
	// its DIB prefetched; OOM = only a genuine allocation failure (the caller
	// may free memory and retry); Error = anything else (rotation failure,
	// size-limit trip, ...) - no retry, no "out of memory" report.
	enum EAfterLoadResult { AfterLoad_OK, AfterLoad_OOM, AfterLoad_Error };
	static EAfterLoadResult ProcessImageAfterLoad(CRequest * request);

	// E6/E12: drop the load memo (bad-file / TIFF-probe conclusions) for a
	// file, e.g. when it is released for reload after an external edit.
	static void InvalidateLoadMemo(LPCTSTR sFileName);
};
