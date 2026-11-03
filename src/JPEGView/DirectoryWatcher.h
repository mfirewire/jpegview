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

	// Kindly terminates the thread by setting the m_terminateEvent
	void Terminate();
	// First tries to terminate the thread by setting m_terminateEvent, when no reaction -> kills the thread
	void Abort();

private:
	CRITICAL_SECTION m_lock;
	HANDLE m_terminateEvent; // Event to terminate the thread
	HANDLE m_newDirectoryEvent; // New directory needs to be watched
	HANDLE m_hThread; // watchdog thread
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
