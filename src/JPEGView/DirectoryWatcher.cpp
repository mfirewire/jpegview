#include "StdAfx.h"
#include "DirectoryWatcher.h"
#include "MessageDef.h"
#include <process.h>

/////////////////////////////////////////////////////////////////////////////////////////////
// Static helpers
/////////////////////////////////////////////////////////////////////////////////////////////

static BOOL GetLastModificationTime(LPCTSTR fileName, FILETIME & lastModificationTime)
{
	HANDLE hFile = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		BOOL bSuccess;
		bSuccess = ::GetFileTime(hFile, NULL, NULL, &lastModificationTime);
		::CloseHandle(hFile);
		return bSuccess;
	}
	return FALSE;
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CDirectoryWatcher::CDirectoryWatcher(HWND hTargetWindow)
	: m_lock{ 0 }
{
	m_hTargetWindow = hTargetWindow;
	::InitializeCriticalSection(&m_lock);
	m_terminateEvent = ::CreateEvent(0, TRUE, FALSE, NULL);
	m_newDirectoryEvent = ::CreateEvent(0, TRUE, FALSE, NULL);
	m_bModificationTimeValid = FALSE;
	m_lastFileChangedTick = 0;

	m_hThread = (HANDLE)_beginthread(ThreadFunc, 0, this);
}

CDirectoryWatcher::~CDirectoryWatcher(void) {
	Abort();
	::DeleteCriticalSection(&m_lock);
	::CloseHandle(m_terminateEvent);
	::CloseHandle(m_newDirectoryEvent);
}

void CDirectoryWatcher::Terminate() { 
	m_bTerminate = true;
	if (m_hThread != NULL) {
		::SetEvent(m_terminateEvent);
		::WaitForSingleObject(m_hThread, 1000);
		m_hThread = NULL;
	}
}

void CDirectoryWatcher::Abort() {
	try {
		if (m_hThread != NULL) {
			::SetEvent(m_terminateEvent);
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

void CDirectoryWatcher::SetCurrentFile(LPCTSTR fileName)
{
	// E18: reading the file time opens the file, which can stall on a
	// slow or network drive. Do it outside the lock; only the assignment
	// takes the lock, so lock waiters are never stuck behind file I/O.
	// The file name is copied first: m_sCurrentFile must not change
	// under the read.
	CString sFileName(fileName);
	FILETIME modificationTime = { 0 };
	BOOL bTimeValid = !sFileName.IsEmpty() && GetLastModificationTime(sFileName, modificationTime);
	::EnterCriticalSection(&m_lock);
	m_sCurrentFile = sFileName;
	m_bModificationTimeValid = bTimeValid;
	if (bTimeValid) {
		m_modificationTimeCurrentFile = modificationTime;
	}
	::LeaveCriticalSection(&m_lock);
}

void CDirectoryWatcher::SetCurrentDirectory(LPCTSTR directoryName)
{
	CString sDirectory(directoryName == NULL ? _T("") : directoryName);
	// B3 (L-D05): "C:" means "the current directory on drive C:", not the
	// root. Normalise a bare drive before resolving it.
	if (sDirectory.GetLength() == 2 && sDirectory[1] == _T(':')) {
		sDirectory += _T('\\');
	}
	TCHAR fullName[MAX_PATH]{ 0 };
	DWORD nFullPathLen = ::GetFullPathName(sDirectory, MAX_PATH, (LPTSTR)fullName, NULL);
	// B3 (decision 2026-11-08: log only). A path that does not fit stays
	// truncated exactly as before, but it is no longer silent: the watcher
	// then fails to arm for that directory, and this line is the only trace.
	if (nFullPathLen == 0 || nFullPathLen >= MAX_PATH) {
		CString sLog;
		sLog.Format(_T("JPEGView: DirectoryWatcher: GetFullPathName returned %u for \"%s\" (MAX_PATH=%d), the path may be truncated\n"),
			(unsigned int)nFullPathLen, (LPCTSTR)sDirectory, MAX_PATH);
		::OutputDebugString(sLog);
	}

	::EnterCriticalSection(&m_lock);

	m_sCurrentDirectory = fullName;

	::LeaveCriticalSection(&m_lock);

	::SetEvent(m_newDirectoryEvent);
}


/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

void CDirectoryWatcher::ThreadFunc(void* arg) {

	CDirectoryWatcher* thisPtr = (CDirectoryWatcher*) arg;
	bool bTerminate = false;
	bool bSetupNewDirectory = true;
	bool bFileChangePending = false; // H7b: a displayed-file change was detected but not yet posted
	CString sPendingFile; // A2-8: the file the pending notification was armed for (see WAIT_TIMEOUT)
	HANDLE waitHandles[4]{ 0 };
	do {
		waitHandles[0] = thisPtr->m_terminateEvent;
		waitHandles[1] = thisPtr->m_newDirectoryEvent;
		int numHandles = bSetupNewDirectory ? 2 : 4;

		// A2-7: as R-23 did for the mtime read, only snapshot the
		// directory under the lock; the FindFirstChangeNotification
		// calls run outside it, so the UI thread's SetCurrentFile is
		// never stuck behind a slow/network drive. A directory change
		// between snapshot and registration only costs one wait cycle:
		// SetCurrentDirectory sets m_newDirectoryEvent, which re-arms
		// setup and closes these handles at the bottom of the loop.
		CString sDirectory;
		::EnterCriticalSection(&thisPtr->m_lock);
		if (bSetupNewDirectory) {
			sDirectory = thisPtr->m_sCurrentDirectory;
		}
		::LeaveCriticalSection(&thisPtr->m_lock);
		if (bSetupNewDirectory && !sDirectory.IsEmpty()) {
			waitHandles[2] = NULL;
			waitHandles[3] = NULL;
			waitHandles[2] = ::FindFirstChangeNotification(sDirectory, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
			if (waitHandles[2] != NULL && waitHandles[2] != INVALID_HANDLE_VALUE)
			{
				numHandles++;
				waitHandles[3] = ::FindFirstChangeNotification(sDirectory, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
				if (waitHandles[3] != NULL && waitHandles[3] != INVALID_HANDLE_VALUE) {
					numHandles++;
				} else {
					// The last-write watch failed: close the file-name
					// watch again instead of leaving half a pair behind,
					// so the next loop iteration retries both from scratch.
					waitHandles[3] = NULL;
					::FindCloseChangeNotification(waitHandles[2]);
					waitHandles[2] = NULL;
					numHandles--;
				}
			} else {
				waitHandles[2] = NULL;
			}
		}

		// wait for events on file system or wakeup event. While a displayed-file
		// change is pending (H7b), wait only until 800 ms after the most recent
		// detected change; a new change during the wait re-arms the timer (see
		// the WAIT_OBJECT_0 + 3 case), so a burst produces one message for its
		// last change instead of dropping it.
		DWORD waitTimeout = INFINITE;
		if (bFileChangePending) {
			DWORD elapsed = ::GetTickCount() - thisPtr->m_lastFileChangedTick;
			waitTimeout = (elapsed >= 800) ? 0 : 800 - elapsed;
		}
		DWORD waitStatus = ::WaitForMultipleObjects(numHandles, waitHandles, FALSE, waitTimeout);

		switch (waitStatus) {
			case WAIT_OBJECT_0:
				// m_terminateEvent is set
				bTerminate = true;
				break;
			case WAIT_OBJECT_0 + 1:
				// m_newDirectoryEvent is set
				::ResetEvent(thisPtr->m_newDirectoryEvent);
				bSetupNewDirectory = true;
				break;
			case WAIT_OBJECT_0 + 2:
				// file added or deleted in directory, send message to registered window
				bSetupNewDirectory = ::FindNextChangeNotification(waitHandles[2]) == FALSE;
				::PostMessage(thisPtr->m_hTargetWindow, WM_ACTIVE_DIRECTORY_FILELIST_CHANGED, 0, 0);
				::WaitForSingleObject(thisPtr->m_terminateEvent, 500); // don't flood the window with change notifications
				break;
			case WAIT_OBJECT_0 + 3:
				{
				// file written in directory, check if the displayed file has changed and send message to registered window if yes
				bSetupNewDirectory = ::FindNextChangeNotification(waitHandles[3]) == FALSE;
				
				// wait a short time, otherwise the file may can not be read yet
				::WaitForSingleObject(thisPtr->m_terminateEvent, 250);

				// R-23: same fix as SetCurrentFile above (E18): reading the
				// file time opens the file, which can stall on a slow or
				// network drive. The lock now only snapshots the file name
				// + last known time; the GetLastModificationTime calls run
				// outside the lock, so the UI thread's SetCurrentFile is
				// never stuck behind file I/O. Retry semantics are
				// unchanged: one retry after a 250 ms wait on failure.
				// (Snapshot-once also fixes a small race in the old code,
				// which re-read m_sCurrentFile on retry and could compare
				// the new file's time against the old file's stored time.)
				bool sendMessage = false;

				CString sCurrentFile;
				FILETIME lastKnownTime = { 0 };
				bool bTimeValid = false;
				::EnterCriticalSection(&thisPtr->m_lock);
				if (thisPtr->m_bModificationTimeValid) {
					sCurrentFile = thisPtr->m_sCurrentFile;
					lastKnownTime = thisPtr->m_modificationTimeCurrentFile;
					bTimeValid = true;
				}
				::LeaveCriticalSection(&thisPtr->m_lock);

				if (bTimeValid) {
					FILETIME fileTime;
					bool canReadModificationTime = true;
					if (!GetLastModificationTime(sCurrentFile, fileTime)) {
						::WaitForSingleObject(thisPtr->m_terminateEvent, 250);
						if (!GetLastModificationTime(sCurrentFile, fileTime)) {
							canReadModificationTime = false;
						}
					}
					sendMessage = canReadModificationTime && ::memcmp(&fileTime, &lastKnownTime, sizeof(FILETIME)) != 0;
				}

				if (sendMessage) {
					// H7b trailing-edge debounce: arm (or re-arm) the pending
					// notification instead of posting immediately. The message
					// goes out only after 800 ms with no further change, so
					// the last change in a burst is delivered. The old code
					// discarded any change within 800 ms of the last posted
					// message, which lost exactly the tail change.
					bFileChangePending = true;
					sPendingFile = sCurrentFile;
					thisPtr->m_lastFileChangedTick = ::GetTickCount();
				}
				break;
				}
			case WAIT_TIMEOUT:
				// 800 ms passed with no new change: deliver the pending notification.
				if (bFileChangePending) {
					bFileChangePending = false;
					// A2-8: the snapshot was taken up to 800 ms ago. If the
					// user flipped to another image in the meantime, the
					// pending notification belongs to the old file: re-check
					// under the lock that the displayed file is still the
					// one the notification was armed for, and drop it if
					// not -- otherwise MainDlg would reload the new image
					// for a change to the old one. A flip away and back to
					// the same file still delivers, which is correct: the
					// file did change on disk and it is current again.
					bool bStillCurrentFile = false;
					::EnterCriticalSection(&thisPtr->m_lock);
					bStillCurrentFile = (thisPtr->m_sCurrentFile == sPendingFile);
					::LeaveCriticalSection(&thisPtr->m_lock);
					sPendingFile.Empty();
					if (bStillCurrentFile) {
						::PostMessage(thisPtr->m_hTargetWindow, WM_DISPLAYED_FILE_CHANGED_ON_DISK, 0, 0);
					}
				}
				break;
			default:
				// unexpected, terminate
				bTerminate = true;
				break;
		}

		if (bTerminate || bSetupNewDirectory) {
			// A pending notification belongs to the old watched directory;
			// drop it when the directory is re-set up instead of posting
			// a stale change for a file that is no longer watched.
			if (bSetupNewDirectory) {
				bFileChangePending = false;
				sPendingFile.Empty();
			}
			if (waitHandles[2] != NULL && waitHandles[2] != INVALID_HANDLE_VALUE) {
				::FindCloseChangeNotification(waitHandles[2]);
				waitHandles[2] = NULL;
			}
			if (waitHandles[3] != NULL && waitHandles[3] != INVALID_HANDLE_VALUE) { 
				::FindCloseChangeNotification(waitHandles[3]);
				waitHandles[3] = NULL;
			}
		}
	} while (!bTerminate);

	_endthread();
}