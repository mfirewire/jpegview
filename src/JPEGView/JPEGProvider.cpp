#include "StdAfx.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "ImageLoadThread.h"
#include "MessageDef.h"
#include "FileList.h"
#include "ProcessParams.h"
#include "BasicProcessing.h"

CJPEGProvider::CJPEGProvider(HWND handlerWnd, int nNumThreads, int nNumBuffers) {
	m_hHandlerWnd = handlerWnd;
	m_nNumThread = nNumThreads;
	m_nNumBuffers = nNumBuffers;
	m_nCurrentTimeStamp = 0;
	m_eOldDirection = FORWARD;
	m_pWorkThreads = new CImageLoadThread*[nNumThreads];
	for (int i = 0; i < nNumThreads; i++) {
		m_pWorkThreads[i] = new CImageLoadThread();
	}
}

CJPEGProvider::~CJPEGProvider(void) {
	// F-M30: same rule as R-21, extended to the requests. If any worker
	// outlives Terminate(), it may still SetEvent() a request's
	// EventFinished and PostMessage to the handler window, so the
	// requests are leaked like the thread objects instead of deleted.
	bool bAllThreadsTerminated = true;
	for (int i = 0; i < m_nNumThread; i++) {
		// R-21: Terminate() returns false when the worker outlives its 10 s
		// window. Deleting the thread object on that path frees memory the
		// still-running ThreadFunc reads (m_bTerminate / m_bCoInitialize
		// after its current decode), so leak the object instead and only
		// NULL the slot - the same rule StopAllThreads already uses.
		// (The CWorkThread destructor alone cannot fix this: it runs too
		// late, after the caller has already decided to delete.)
		if (m_pWorkThreads[i]->Terminate()) {
			delete m_pWorkThreads[i];
		} else {
			m_pWorkThreads[i] = NULL;
			bAllThreadsTerminated = false;
		}
	}
	delete[] m_pWorkThreads;
	if (!bAllThreadsTerminated) {
		// A worker may still be alive: do not touch the requests.
		return;
	}
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		delete (*iter)->Image;
		delete *iter;
	}
}

// ST-07/ST-08: the UI-thread waits (RequestImage,
// StartRequestAndWaitUntilReady, and ReleaseFile via
// CWorkThread::ProcessAndWait) share CWorkThread::
// WaitForEventWithPaintOnlyPump (WorkThread.cpp). It pumps repaint
// messages only, so the window repaints during a slow decode without
// letting a page key, timer, command or close run re-entrantly, and it
// returns false on WAIT_FAILED instead of spinning. A stuck decode still
// has to finish (the decoders offer no cancel point), so a hard cancel is
// out of scope; keys/close are deferred, not lost - they stay queued and
// are handled by the main loop once the wait returns.
// (The old comment here claimed ProcessAndWait kept its INFINITE wait on
// purpose because it ran on the load thread. That was wrong: its only
// caller is CImageLoadThread::ReleaseFile, reached from the UI thread via
// CJPEGProvider::ClearRequest, so it froze the UI exactly like the waits
// below. It pumps now, through the same helper.)

CJPEGImage* CJPEGProvider::RequestImage(CFileList* pFileList, EReadAheadDirection eDirection,
                                        LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams,
                                        bool& bOutOfMemory, bool& bExceptionError, bool forceFullSizeRAW, bool* pbSizeLimitExceeded) {
	if (strFileName == NULL) {
		bOutOfMemory = false;
		bExceptionError = false;
		if (pbSizeLimitExceeded != NULL) *pbSizeLimitExceeded = false;
		return NULL;
	}

	// Search if we have the requested image already present or in progress
	CImageRequest* pRequest = FindRequest(strFileName, nFrameIndex);
	bool bDirectionChanged = eDirection != m_eOldDirection || eDirection == TOGGLE;
	bool bRemoveAlsoActiveRequests = bDirectionChanged; // if direction changed, all read-ahead requests are wrongly guessed
	bool bWasOutOfMemory = false;
	m_eOldDirection = eDirection;

	if (pRequest == NULL) {
		// no request pending for this file, add to request queue and start async
		pRequest = StartNewRequest(strFileName, nFrameIndex, processParams, forceFullSizeRAW);
		if (pRequest == NULL) {
			// A2-9: the request object (or its load request) could not be
			// allocated. Report out-of-memory like any other failed load:
			// MainDlg maps it to the "out of memory" message and shows no
			// image, instead of dereferencing NULL below.
			bOutOfMemory = true;
			bExceptionError = false;
			if (pbSizeLimitExceeded != NULL) *pbSizeLimitExceeded = false;
			return NULL;
		}
		// wait with read ahead when direction changed - maybe user just wants to re-see last image
		if (!bDirectionChanged && eDirection != NONE) {
			// start parallel if more than one thread
			StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumThread - 1, NULL);
		}
	}

	// wait for request if not yet ready
	if (!pRequest->Ready) {
#ifdef DEBUG
		::OutputDebugString(_T("Waiting for request: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		// ST-08: on a failed wait (WAIT_FAILED) the request may already be
		// gone, so bail out without touching it or any provider member.
		if (!CWorkThread::WaitForEventWithPaintOnlyPump(pRequest->EventFinished)) {
			bOutOfMemory = false;
			bExceptionError = false;
			if (pbSizeLimitExceeded != NULL) *pbSizeLimitExceeded = false;
			return NULL;
		}
		GetLoadedImageFromWorkThread(pRequest);
	} else {
		CJPEGImage* pImage = pRequest->Image;
		if (pImage != NULL) {
			// make sure the initial parameters are reset as when keep params was on before they are wrong
			EProcessingFlags procFlags = processParams.ProcFlags;
			pImage->RestoreInitialParameters(strFileName, processParams.ImageProcParams, procFlags, 
				processParams.RotationParams.Rotation, processParams.Zoom, processParams.Offsets, 
				CSize(processParams.TargetWidth, processParams.TargetHeight), processParams.MonitorSize);
		}
#ifdef DEBUG
		::OutputDebugString(_T("Found in cache: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
	}

	// set before removing unused images!
	pRequest->InUse = true;
	pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;

	if (pRequest->OutOfMemory) {
		// The request could not be satisfied because the system is out of memory.
		// Clear all memory and try again - maybe some readahead requests can be deleted
#ifdef DEBUG
		::OutputDebugString(_T("Retrying request because out of memory: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		bWasOutOfMemory = true;
		if (FreeAllPossibleMemory()) {
			DeleteElement(pRequest);
			pRequest = StartRequestAndWaitUntilReady(strFileName, nFrameIndex, processParams, forceFullSizeRAW);
			if (pRequest == NULL) {
				// ST-08: the retry's wait failed; do not touch the (possibly
				// already deleted) retried request.
				bOutOfMemory = false;
				bExceptionError = false;
				if (pbSizeLimitExceeded != NULL) *pbSizeLimitExceeded = false;
				return NULL;
			}
		}
	}

	// cleanup stuff no longer used
	RemoveUnusedImages(bRemoveAlsoActiveRequests);
	ClearOldestInactiveRequest();

	// check if we shall start new requests (don't start another request if we are short of memory!)
	if (m_requestList.size() < (unsigned int)m_nNumBuffers && !bDirectionChanged && !bWasOutOfMemory && eDirection != NONE) {
		StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumThread, pRequest);
	}

	bOutOfMemory = pRequest->OutOfMemory;
	bExceptionError = pRequest->ExceptionError;
	// P2-5: a size-limit rejection is not out-of-memory (and never triggered
	// the retry above, which only runs on pRequest->OutOfMemory). Report it
	// through the optional out-param; the on-screen "size exceeded" text is
	// owned by the MainDlg/HelpersGUI error display, not this class.
	if (pbSizeLimitExceeded != NULL) {
		*pbSizeLimitExceeded = pRequest->SizeLimitExceeded;
	}
	return pRequest->Image;
}

void CJPEGProvider::NotifyNotUsed(CJPEGImage* pImage) {
	// mark image as unused but do not remove yet from request queue
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Image == pImage) {
			(*iter)->InUse = false;
			(*iter)->IsActive = false;
			return;
		}
	}
	// image not found in request queue - delete it as it is no longer used and will not be cached
	if (pImage != NULL) delete pImage;
}

void CJPEGProvider::ClearAllRequests(bool releaseLockedFile) {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (ClearRequest((*iter)->Image, releaseLockedFile)) {
			// removed from iteration, restart iteration to remove the rest
			ClearAllRequests(releaseLockedFile);
			break;
		}
	}
}

bool CJPEGProvider::FreeAllPossibleMemory() {
	bool bCouldFreeMemory = false;
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		CImageRequest* pRequest = *iter;
		if (!pRequest->InUse && pRequest->Ready) {
			DeleteElementAt(iter);
			FreeAllPossibleMemory();
			bCouldFreeMemory = true;
			break;
		}
	}
	return bCouldFreeMemory;
}

void CJPEGProvider::FileHasRenamed(LPCTSTR sOldFileName, LPCTSTR sNewFileName) {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (_tcsicmp(sOldFileName, (*iter)->FileName) == 0) {
			(*iter)->FileName = sNewFileName;
		}
	}
}

bool CJPEGProvider::ClearRequest(CJPEGImage* pImage, bool releaseLockedFile) {
	if (pImage == NULL) {
		return false;
	}
	bool bErased = false;
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Image == pImage) {
			if (releaseLockedFile) {
				// E2 follow-up: every load thread owns its own cached decoders
				// (animated WebP/PNG/JXL/AVIF, GDI+ bitmap), keyed on file
				// name. Releasing only on thread [0] left a thread-1 cache
				// behind, so a reload after an external edit could still
				// serve stale frames. The NULL guard mirrors the destructor:
				// a slot is NULLed when its thread outlived Terminate().
				for (int i = 0; i < m_nNumThread; i++) {
					if (m_pWorkThreads[i] != NULL) {
						m_pWorkThreads[i]->ReleaseFile((*iter)->FileName);
					}
				}
			}
			// images that are not ready cannot be removed yet
			if ((*iter)->Ready) {
				DeleteElementAt(iter);
				bErased = true;
			} else {
				(*iter)->Deleted = true;
			}

			break;
		}
	}
	return bErased;
}

void CJPEGProvider::OnImageLoadCompleted(int nHandle) {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Handle == nHandle) {
			GetLoadedImageFromWorkThread(*iter);
			if ((*iter)->Deleted) {
				// this request was deleted, delete image now
				ClearRequest((*iter)->Image);
			}
			break;
		}
	}
}

CJPEGProvider::CImageRequest* CJPEGProvider::FindRequest(LPCTSTR strFileName, int nFrameIndex) {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (_tcsicmp((*iter)->FileName, strFileName) == 0 && (*iter)->FrameIndex == nFrameIndex && !(*iter)->Deleted) {
			return *iter;
		}
	}
	return NULL;
}

CJPEGProvider::CImageRequest* CJPEGProvider::StartRequestAndWaitUntilReady(LPCTSTR sFileName, int nFrameIndex, const CProcessParams & processParams, bool forceFullSizeRAW) {
	CImageRequest* pRequest = StartNewRequest(sFileName, nFrameIndex, processParams, forceFullSizeRAW);
	// A2-9: NULL when the request could not be allocated; the caller (the
	// OOM retry in RequestImage) already checks for NULL before touching
	// pRequest.
	if (pRequest == NULL) {
		return NULL;
	}
	// ST-08: NULL on a failed wait (WAIT_FAILED) - the caller (the OOM
	// retry in RequestImage) must check for it before touching pRequest.
	if (!CWorkThread::WaitForEventWithPaintOnlyPump(pRequest->EventFinished)) {
		return NULL;
	}
	GetLoadedImageFromWorkThread(pRequest);
	return pRequest;
}

void CJPEGProvider::StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection, 
										  const CProcessParams & processParams, int nNumRequests, CImageRequest* pLastReadyRequest) {
	if (nNumRequests == 0 || pFileList == NULL) {
		return;
	}
	for (int i = 0; i < nNumRequests; i++) {
		bool bSwitchImage = true;
		int nFrameIndex = (pLastReadyRequest != NULL) ? Helpers::GetFrameIndex(pLastReadyRequest->Image, eDirection == FORWARD, true, bSwitchImage) : 0;
		LPCTSTR sFileName = bSwitchImage ? pFileList->PeekNextPrev(i + 1, eDirection == FORWARD, eDirection == TOGGLE) : pFileList->Current();
		if (sFileName != NULL && FindRequest(sFileName, nFrameIndex) == NULL) {
			if (GetProcessingFlag(PFLAG_NoProcessingAfterLoad, processParams.ProcFlags)) {
				// The read ahead threads need this flag to be deleted - we can speculatively process the image with good hit rate
				CProcessParams paramsCopied = processParams;
				paramsCopied.ProcFlags = SetProcessingFlag(paramsCopied.ProcFlags, PFLAG_NoProcessingAfterLoad, false);
				StartNewRequest(sFileName, nFrameIndex, paramsCopied);
			} else {
				StartNewRequest(sFileName, nFrameIndex, processParams);
			}
		}
	}
}

CJPEGProvider::CImageRequest* CJPEGProvider::StartNewRequest(LPCTSTR sFileName, int nFrameIndex, const CProcessParams & processParams, bool forceFullSizeRAW) {
#ifdef DEBUG
	::OutputDebugString(_T("Start new request: ")); ::OutputDebugString(sFileName); ::OutputDebugString(_T("\n"));
#endif
	// A2-9: 'new CImageRequest' can throw on allocation failure; this runs
	// on the UI thread (RequestImage / StartRequestAndWaitUntilReady) with
	// no try/catch above. Use nothrow and report NULL instead -- both
	// callers already handle a NULL request (RequestImage above,
	// StartRequestAndWaitUntilReady's wait-failure path).
	CImageRequest* pRequest = new(std::nothrow) CImageRequest(sFileName, nFrameIndex);
	if (pRequest == NULL) {
		return NULL;
	}
	try {
		m_requestList.push_back(pRequest);
	} catch (...) {
		// The list node allocation failed: nothing references the request yet.
		delete pRequest;
		return NULL;
	}
	pRequest->HandlingThread = SearchThreadForNewRequest();
	// AsyncLoad returns -1 when its own 'new CRequest' fails (see
	// ImageLoadThread.cpp): the request was never queued, so its event
	// would never fire and RequestImage would wait forever. Drop the
	// request and report NULL instead of leaving a dead entry behind.
	pRequest->Handle = pRequest->HandlingThread->AsyncLoad(pRequest->FileName, nFrameIndex,
		processParams, m_hHandlerWnd, pRequest->EventFinished, forceFullSizeRAW);
	if (pRequest->Handle < 0) {
		m_requestList.remove(pRequest);
		delete pRequest;
		return NULL;
	}
	return pRequest;
}

void CJPEGProvider::GetLoadedImageFromWorkThread(CImageRequest* pRequest) {
	if (pRequest->HandlingThread != NULL) {
#ifdef DEBUG
		::OutputDebugString(_T("Finished request: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		CImageData imageData = pRequest->HandlingThread->GetLoadedImage(pRequest->Handle);
		pRequest->Image = imageData.Image;
		pRequest->OutOfMemory = imageData.IsRequestFailedOutOfMemory;
		pRequest->ExceptionError = imageData.IsRequestFailedException;
		pRequest->SizeLimitExceeded = imageData.IsRequestFailedSizeLimit;
		pRequest->Ready = true;
		pRequest->HandlingThread = NULL;
	}
}

CImageLoadThread* CJPEGProvider::SearchThreadForNewRequest(void) {
	int nSmallestHandle = INT_MAX;
	CImageLoadThread* pBestOccupiedThread = NULL;
	for (int i = 0; i < m_nNumThread; i++) {
		bool bFree = true;
		CImageLoadThread* pThisThread = m_pWorkThreads[i];
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->Handle < nSmallestHandle && (*iter)->HandlingThread != NULL) {
				nSmallestHandle = (*iter)->Handle;
				pBestOccupiedThread = (*iter)->HandlingThread;
			}
			if ((*iter)->HandlingThread == pThisThread) {
				bFree = false;
				break;
			}
		}
		if (bFree) {
			return pThisThread;
		}
	}
	// all threads are occupied, return thread working on smallest handle (will finish earliest)
	return (pBestOccupiedThread == NULL) ? m_pWorkThreads[0] : pBestOccupiedThread;
}

void CJPEGProvider::RemoveUnusedImages(bool bRemoveAlsoActiveRequests) {
	bool bRemoved = false;
	int nTimeStampToRemove = -2;
	do {
		bRemoved = false;
		int nSmallestTimeStamp = INT_MAX;
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->InUse == false && (*iter)->Ready && ((*iter)->IsActive == false || bRemoveAlsoActiveRequests || IsDestructivelyProcessed((*iter)->Image))) {
				// search element with smallest timestamp
				if ((*iter)->AccessTimeStamp < nSmallestTimeStamp) {
					nSmallestTimeStamp = (*iter)->AccessTimeStamp;
				}
				// remove the readahead images - if we get here with read ahead, the strategy was wrong and
				// the read ahead image is not used.
				if ((*iter)->AccessTimeStamp == nTimeStampToRemove || IsDestructivelyProcessed((*iter)->Image)) {
#ifdef DEBUG
					::OutputDebugString(_T("Delete request: ")); ::OutputDebugString((*iter)->FileName); ::OutputDebugString(_T("\n"));
#endif
					DeleteElementAt(iter);
					bRemoved = true;
					break;
				}
			}
		}
		nTimeStampToRemove = -2;
		// Make one buffer free for next readahead (except when bRemoveAlsoActiveRequests)
		int nMaxListSize = bRemoveAlsoActiveRequests ? (unsigned int)m_nNumBuffers : (unsigned int)m_nNumBuffers - 1;
		if (m_requestList.size() > (unsigned int)nMaxListSize) {
			// remove element with smallest timestamp
			if (nSmallestTimeStamp < INT_MAX) {
				bRemoved = true;
				nTimeStampToRemove = nSmallestTimeStamp;
			}
		}
	} while (bRemoved); // repeat until no element could be removed anymore
}

void CJPEGProvider::ClearOldestInactiveRequest() {
	if (m_requestList.size() >= (unsigned int)m_nNumBuffers) {
		int nFirstHandle = INT_MAX;
		CImageRequest* pFirstRequest = NULL;
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->IsActive) {
				// mark very old requests for removal
				if (CImageLoadThread::GetCurHandleValue() - (*iter)->Handle > m_nNumBuffers) {
					(*iter)->IsActive = false;
				}
				if ((*iter)->Handle < nFirstHandle) {
					nFirstHandle = (*iter)->Handle;
					pFirstRequest = *iter;
				}
			}
		}
		if (pFirstRequest != NULL) {
			pFirstRequest->IsActive = false;
			ClearOldestInactiveRequest();
		}
	}
}

void CJPEGProvider::DeleteElementAt(std::list<CImageRequest*>::iterator iteratorAt) {
	delete (*iteratorAt)->Image;
	delete *iteratorAt;
	m_requestList.erase(iteratorAt);
}

void CJPEGProvider::DeleteElement(CImageRequest* pRequest) {
	delete pRequest->Image;
	delete pRequest;
	m_requestList.remove(pRequest);
}

bool CJPEGProvider::IsDestructivelyProcessed(CJPEGImage* pImage) {
	return pImage != NULL && pImage->IsDestructivelyProcessed();
}