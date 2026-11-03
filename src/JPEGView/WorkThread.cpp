
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
	::WaitForSingleObject(pRequest->EventFinished, INFINITE);

	if (bCreateEvent) {
		::CloseHandle(pRequest->EventFinished);
	}
	pRequest->Deleted = true; // make sure the request is removed from the queue
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
