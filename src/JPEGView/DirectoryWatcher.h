#pragma once

// Watcher thread for a directory. Sends the two messages WM_DISPLAYED_FILE_CHANGED_ON_DISK and WM_ACTIVE_DIRECTORY_FILELIST_CHANGED
// to the specified window.
class CDirectoryWatcher {
public:
	// The messages are sent to the specified target window.
	CDirectoryWatcher(HWND hTargetWindow);
	virtual ~CDirectoryWatcher(void);

	void SetCurrentFile(LPCTSTR fileName);
	void SetCurrentDirectory(LPCTSTR directoryName);

	// Kindly terminates the thread by setting the m_terminateEvent.
	// Returns true only when the worker is confirmed to have returned from
	// ThreadFunc. When it returns false the worker is stuck in a blocking
	// call (e.g. a dead network path) and still dereferences this object:
	// the caller must NOT delete it, otherwise the critical section and
	// both events are freed under the running thread (P0-4). The object is
	// leaked instead, like the image load threads (R-21/F-M30).
	bool Terminate();

private:
	CRITICAL_SECTION m_lock;
	HANDLE m_terminateEvent; // Event to terminate the thread
	HANDLE m_newDirectoryEvent; // New directory needs to be watched
	HANDLE m_hThread; // watchdog thread
	HANDLE m_threadFinishedEvent; // set by the worker as its last action; the only safe-delete criterion (P0-4)
	HWND m_hTargetWindow;
	bool m_bTerminate;

	CString m_sCurrentDirectory;
	CString m_sCurrentFile;
	BOOL m_bModificationTimeValid;
	FILETIME m_modificationTimeCurrentFile;
	DWORD m_lastFileChangedTick; // GetTickCount() of the most recent detected displayed-file change (H7b trailing-edge debounce).
	// DWORD, not __int64: GetTickCount itself is a DWORD that wraps every 49.7 days, and the elapsed-time
	// subtraction below is done in DWORD arithmetic where the wrap is well-defined, so a wrapped tick still
	// yields the correct small difference for the 800 ms window. (GetTickCount64 would be cleaner but needs
	// Vista+, and this still targets XP.)

	static void  __cdecl ThreadFunc(void* arg);
};
