#include "StdAfx.h"
#include "ProcessingThreadPool.h"
#include "SettingsProvider.h"

CProcessingThreadPool* CProcessingThreadPool::sm_instance;

///////////////////////////////////////////////////////////////////////////////////
// Supporting classes
///////////////////////////////////////////////////////////////////////////////////

// A request wrapping another request and adding information about the image strip to process.
class CWrappedRequest : public CRequestBase {
public:
	// nOffset, nSize defines the strip to process: 'nSize' rows, starting at row 'nOffset'
	CWrappedRequest(CProcessingRequest * pRequest, int nOffset, int nSize, HANDLE hEventFinished) : CRequestBase(hEventFinished) {
		InnerRequest = pRequest;
		Offset = nOffset;
		SizeY = nSize;
	}

	CProcessingRequest* InnerRequest;
	int Offset;
	int SizeY;
};


// Worker thread in thread pool, executing image processing operations on image strips
class CProcessingThread : public CWorkThread {
public:
	CProcessingThread(void) : CWorkThread(false) {}
	~CProcessingThread(void) {}

	// Start a processing request on this thread asynchronously, returns immediately
	void StartProcess(CWrappedRequest* pRequest);

	// Processes a request synchronously on the calling thread
	static void DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY);
private:

	virtual void ProcessRequest(CRequestBase& request);
};

///////////////////////////////////////////////////////////////////////////////////
// CProcessingThreadPool
///////////////////////////////////////////////////////////////////////////////////

CProcessingThreadPool& CProcessingThreadPool::This() {
	if (sm_instance == NULL) {
		sm_instance = new CProcessingThreadPool();
	}
	return *sm_instance;
}

void CProcessingThreadPool::CreateThreadPoolThreads() {
	m_nNumThreads = CSettingsProvider::This().NumberOfCoresToUse() - 1;
	if (m_nNumThreads > 0) {
		m_threads = new CProcessingThread*[m_nNumThreads];
		for (int i = 0; i < m_nNumThreads; i++) {
			m_threads[i] = new CProcessingThread();
		}
	} else {
		m_threads = NULL;
	}
}

void CProcessingThreadPool::StopAllThreads() {
	// R-21 note: this leak-on-timeout rule is the model CJPEGProvider's
	// destructor now follows too. Be aware this function currently has NO
	// callers anywhere in the code base (the pool is a process-lifetime
	// singleton), so it is dead-code hardening, not the live shutdown path.
	for (int i = 0; i < m_nNumThreads; i++) {
		// Terminate() returns false when the thread outlives its wait window; in
		// that case deleting it would free an object the thread is still using, so
		// leak it instead - this only runs at process exit and the OS reclaims it.
		if (m_threads[i]->Terminate()) {
			delete m_threads[i];
		}
	}
	delete[] m_threads;   // the pointer array itself used to leak
	m_nNumThreads = 0;
	m_threads = NULL;
}

bool CProcessingThreadPool::Process(CProcessingRequest* pRequest) {
	int nTargetCX = pRequest->ClippedTargetSize.cx;
	int nTargetCY = pRequest->ClippedTargetSize.cy;
	if (m_nNumThreads == 0) {
		CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
	} else {
		if ((long long)nTargetCX * nTargetCY < 100000 || nTargetCY <= 12) {
			CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
		} else {
			// Important: All slices must have a height dividable by 'StripPadding', except the last one
			int nNumThreadsUsed = m_nNumThreads + 1; // we also use the calling thread, thus +1
			int nSliceCY = 0;
			// The > 1 guard below stops the loop from reaching zero threads: with
			// AVX2 StripPadding is 16, so a target height of 13..15 would make the
			// slice 0 and the next division by zero.
			while (nNumThreadsUsed > 1 && (nSliceCY = ~(pRequest->StripPadding - 1) & (nTargetCY / nNumThreadsUsed)) < pRequest->StripPadding) {
				nNumThreadsUsed--;
			}
			if (nNumThreadsUsed == 1) {
				// Nothing below would run: the loop that hands out strips has
				// zero iterations, so no wrapped request ever decrements
				// nRequestThreadCounter and eventFinished is never set. Waiting
				// for it afterwards blocked the caller forever. Process the whole
				// image on this thread and skip the event machinery entirely.
				CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
				return pRequest->Success;
			}
			int nLastCY = nTargetCY - (nNumThreadsUsed - 1)*nSliceCY;
			volatile LONG nRequestThreadCounter = nNumThreadsUsed - 1;
			int nCurrCY = 0;
			HANDLE eventFinished = ::CreateEvent(0, TRUE, FALSE, NULL);
			CWrappedRequest** pAllWrappedRequests = new CWrappedRequest*[nNumThreadsUsed-1];
			for (int i = 0; i < nNumThreadsUsed-1; i++) {
				pAllWrappedRequests[i] = new CWrappedRequest(pRequest, nCurrCY, nSliceCY, eventFinished);
				pAllWrappedRequests[i]->EventFinishedCounter = &nRequestThreadCounter;
				m_threads[i]->StartProcess(pAllWrappedRequests[i]);
				nCurrCY += nSliceCY;
			}
			CProcessingThread::DoProcess(pRequest, nCurrCY, nLastCY);
			::WaitForSingleObject(eventFinished, INFINITE);
			::CloseHandle(eventFinished);
			for (int i = 0; i < nNumThreadsUsed-1; i++) {
				pAllWrappedRequests[i]->Deleted = true; // thread pool threads will remove the requests from the queue
			}
			delete [] pAllWrappedRequests;
		}
	}
	return pRequest->Success;
}

CProcessingThreadPool::CProcessingThreadPool(void) {
	m_threads = NULL;
	m_nNumThreads = 0;
}


void CProcessingThread::StartProcess(CWrappedRequest* pRequest) {
	ProcessAsync(pRequest);
}

void CProcessingThread::DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY) {
	// Processing is done in strips to reduce memory consumption and increase cache hit rate.
	// The following constant gives the number of pixels to process per strip.
	const uint32 MAX_SRC_PIXELS_PER_STRIP = 1024 * 100;
	uint32 nNumberOfPixelsInSource = (uint32)((pRequest->SourceSize.cx * (double)pRequest->ClippedTargetSize.cx / pRequest->FullTargetSize.cx) *
		(pRequest->SourceSize.cy * (double)nSizeY / pRequest->FullTargetSize.cy));
	uint32 nStrips = 1 + nNumberOfPixelsInSource / MAX_SRC_PIXELS_PER_STRIP;
	uint32 nStripHeight = nSizeY / nStrips;
	uint32 minimalStripHeight = min(16, pRequest->StripPadding);

	if (nStrips > 1) {
		nStripHeight = nStripHeight & ~(pRequest->StripPadding - 1); // must be dividable by 'StripPadding', except last strip
		nStripHeight = min(nSizeY, max(nStripHeight, minimalStripHeight));
	}
	int nSizeProcessed = 0;
	int nCurrentSizeY = nStripHeight;
	while (nSizeProcessed < nSizeY) {
		int nCurrentOffsetY = nOffsetY + nSizeProcessed;
		if (!pRequest->ProcessStrip(nCurrentOffsetY, nCurrentSizeY)) {
			pRequest->Success = false;
			break;
		}
		nSizeProcessed += nCurrentSizeY;
		nCurrentSizeY = min(nStripHeight, nSizeY - nSizeProcessed);
	}
}

void CProcessingThread::ProcessRequest(CRequestBase& request) {
	CWrappedRequest* pWrappedRequest = (CWrappedRequest*)&request;
	DoProcess(pWrappedRequest->InnerRequest, pWrappedRequest->Offset, pWrappedRequest->SizeY);
}

