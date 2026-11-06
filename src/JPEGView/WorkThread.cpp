
#include "StdAfx.h"
#include "WorkThread.h"
#include <process.h>

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CWorkThread::CWorkThread(bool bCoInitialize)
	: m_csList{ 0 }
{
	m_bTerminate = false;
	m_bStopped = false;
	m_bCoInitialize = bCoInitialize;
	::InitializeCriticalSection(&m_csList);
	m_wakeUp = ::CreateEvent(0, TRUE, FALSE, NULL);

	m_hThread = (HANDLE)_beginthread(ThreadFunc, 0, this);
}

CWorkThread::~CWorkThread(void) {
	bool bStopped = m_bStopped;
	if (!m_bTerminate) {
		bStopped = Terminate();
	}
	// Only release what the thread is done with. Terminate() waits 10 s and on
	// timeout leaves the thread running; deleting the critical section and the
	// event while it still uses them is a use-after-free. On that path leak them
	// instead - the process is exiting and the OS reclaims everything.
	if (bStopped) {
		::DeleteCriticalSection(&m_csList);
		::CloseHandle(m_wakeUp);
	}
}

void CWorkThread::ProcessAndWait(CRequestBase* pRequest) {
	bool bCreateEvent = pRequest->EventFinished == NULL;
	if (bCreateEvent) {
		pRequest->EventFinished = ::CreateEvent(0, TRUE, FALSE, NULL);
	}

	ProcessAsync(pRequest);
	// ST-07/R-03: the only caller is CImageLoadThread::ReleaseFile, which
	// runs on the UI thread via CJPEGProvider::ClearRequest, so the old
	// unpumped INFINITE wait froze the UI whenever a slow decode was in
	// progress. Pump repaint messages while waiting; the result is ignored
	// (the release is best-effort - on a failed wait the request is still
	// marked deleted below so the queue can drain).
	WaitForEventWithPaintOnlyPump(pRequest->EventFinished);

	if (bCreateEvent) {
		::CloseHandle(pRequest->EventFinished);
	}
	pRequest->Deleted = true; // make sure the request is removed from the queue
}

// Shared by the UI-thread waits in CJPEGProvider (RequestImage,
// StartRequestAndWaitUntilReady) and by ProcessAndWait above, so the three
// wait points cannot drift apart again (ST-07/ST-08).
bool CWorkThread::WaitForEventWithPaintOnlyPump(HANDLE hEvent) {
	if (hEvent == NULL) {
		return false;
	}
	for (;;) {
		DWORD dwRet = ::MsgWaitForMultipleObjects(1, &hEvent, FALSE, INFINITE, QS_PAINT | QS_SENDMESSAGE);
		if (dwRet == WAIT_OBJECT_0) {
			return true;
		}
		if (dwRet == WAIT_OBJECT_0 + 1) {
			MSG msg;
			// ST-08/R-02: repaint only. The old QS_ALLINPUT + unfiltered
			// PeekMessage dispatched keys, timers, commands and WM_CLOSE
			// here, so a page key during a slow decode re-entered GotoImage
			// and Esc/close deleted the provider under the waiting
			// RequestImage. Those messages stay queued now and are handled
			// by the main loop after the wait returns, with the provider
			// still alive. WM_PAINT is safe to dispatch: OnPaint only draws
			// m_pCurrentImage (NULL-safe) and never touches the provider.
			while (::PeekMessage(&msg, NULL, WM_PAINT, WM_PAINT, PM_REMOVE)) {
				::TranslateMessage(&msg);
				::DispatchMessage(&msg);
			}
			continue;
		}
		// WAIT_FAILED (e.g. the event handle was closed while waiting) or
		// any other unexpected result: return instead of spinning. The
		// caller must bail out without touching the request.
		return false;
	}
}

void CWorkThread::ProcessAsync(CRequestBase* pRequest) {
	::EnterCriticalSection(&m_csList);
	m_requestList.push_back(pRequest);
	::LeaveCriticalSection(&m_csList);

	// there is something to process now
	::SetEvent(m_wakeUp);
}

bool CWorkThread::Terminate() { 
	m_bTerminate = true;
	if (m_hThread != NULL) {
		::SetEvent(m_wakeUp);
		// The return value was ignored: a thread that outlives the 10 s window
		// is still running, and anything deleted afterwards is a use-after-free.
		const bool bStopped = WAIT_OBJECT_0 == ::WaitForSingleObject(m_hThread, 10000);
		m_hThread = NULL;
		m_bStopped = bStopped;
		return bStopped;
	}
	// A NULL handle means Terminate already ran once: report what that run
	// actually achieved instead of claiming success. After a timeout
	// m_bStopped is false, and a second call must not rewrite it to true
	// (the destructor relies on this to leak, not delete, the thread's
	// critical section and event while it is still running).
	return m_bStopped;
}

void CWorkThread::Abort() {
	try {
		if (m_hThread != NULL) {
			::SetEvent(m_wakeUp);
			if (WAIT_TIMEOUT == ::WaitForSingleObject(m_hThread, 100)) {
				::TerminateThread(m_hThread, 1);
				::WaitForSingleObject(m_hThread, 100);
			}
			m_hThread = NULL;
		}
	} catch (...) {
		// do not crash
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

void CWorkThread::ThreadFunc(void* arg) {

	CWorkThread* thisPtr = (CWorkThread*) arg;
	if (thisPtr->m_bCoInitialize) {
		::CoInitialize(NULL);
	}
	do {
		::EnterCriticalSection(&thisPtr->m_csList);

		// Delete the requests marked for deletion from request queue
		DeleteAllRequestsMarkedForDeletion(thisPtr);

		// search a request that is not yet processed
		CRequestBase* requestHandled = NULL;
		int nNumUnprocessedRequests = 0;
		std::list<CRequestBase*>::iterator iter;
		// Deliberately no break here: the loop ends with requestHandled on the
		// last unprocessed request (LIFO). For read-ahead that favours the most
		// recently submitted request. Correctness does not depend on the order -
		// completion is matched by handle, and the thread only sleeps once none
		// are left.
		for (iter = thisPtr->m_requestList.begin( ); iter != thisPtr->m_requestList.end( ); iter++ ) {
			if ((*iter)->Processed == false) {
				requestHandled = *iter;
				nNumUnprocessedRequests++;
			}
		}

		::LeaveCriticalSection(&thisPtr->m_csList);

		// process this request
		if (requestHandled != NULL) {
			try {
				thisPtr->ProcessRequest(*requestHandled);
			} catch (...) {
				// A decoder that throws must not escape the thread function. Even
				// if the process survived that, Processed would stay false and the
				// event below would never be set, so every ProcessAndWait caller
				// (ReleaseFile runs on the UI thread) would block forever. Swallow
				// it and fall through to the normal completion path: the request
				// reports failure by leaving its image NULL.
			}
			requestHandled->Processed = true;

			// signal end of processing
			if (requestHandled->EventFinished != NULL) {
				if (requestHandled->EventFinishedCounter == NULL) {
					::SetEvent(requestHandled->EventFinished);
				} else {
					LONG nNewValue = ::InterlockedDecrement(requestHandled->EventFinishedCounter);
					if (nNewValue <= 0) {
						::SetEvent(requestHandled->EventFinished);
					}
				}
			}
			if (!thisPtr->m_bTerminate) {
				thisPtr->AfterFinishProcess(*requestHandled);
			}
			nNumUnprocessedRequests--;
		}

		// if there are no more requests, sleep until woke up
		if (nNumUnprocessedRequests == 0 && !thisPtr->m_bTerminate) {
			::WaitForSingleObject(thisPtr->m_wakeUp, INFINITE);
			::ResetEvent(thisPtr->m_wakeUp);
		}
	} while (!thisPtr->m_bTerminate);
	if (thisPtr->m_bCoInitialize) {
		::CoUninitialize();
	}
	_endthread();
}

void CWorkThread::DeleteAllRequestsMarkedForDeletion(CWorkThread* thisPtr) {
	// Iterative erase-returning-next, where the old code deleted one element and
	// then recursed to rescan the whole list - quadratic and, for a long queue,
	// deep recursion.
	std::list<CRequestBase*>::iterator iter = thisPtr->m_requestList.begin();
	while (iter != thisPtr->m_requestList.end()) {
		if ((*iter)->Deleted) {
			delete *iter;
			iter = thisPtr->m_requestList.erase(iter);
		} else {
			++iter;
		}
	}
}
